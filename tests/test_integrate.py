import io
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from integrate import cli

LINES = [f"line {i}\n" for i in range(1, 41)]


def run(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True,
                          capture_output=True, text=True).stdout


def write(repo, name, lines):
    (repo / name).write_text("".join(lines), encoding="utf-8")


def commit(repo, message):
    run(repo, "add", "-A")
    run(repo, "commit", "-q", "-m", message)


def edited(index, text):
    lines = list(LINES)
    lines[index] = text
    return lines


class RepoTest(unittest.TestCase):
    """A repo whose `main` stands in for upstream; branches are cut from it."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        run(self.repo, "init", "-q", "-b", "main")
        for key, value in (("user.name", "t"), ("user.email", "t@t"),
                           ("commit.gpgsign", "false"), ("core.autocrlf", "false")):
            run(self.repo, "config", key, value)
        write(self.repo, "a.cc", LINES)
        write(self.repo, "b.cc", LINES)
        commit(self.repo, "base")
        self.list = self.tmp / "integration.txt"
        self.worktree = self.repo / ".claude" / "worktrees" / "integration"

    def tearDown(self):
        shutil.rmtree(self.tmp, onerror=lambda f, p, e: (Path(p).chmod(0o700), f(p)))

    def branch(self, name, files, start="main"):
        run(self.repo, "checkout", "-q", "-b", name, start)
        for path, lines in files.items():
            write(self.repo, path, lines)
        commit(self.repo, name)
        run(self.repo, "checkout", "-q", "main")

    def listing(self, *refs):
        self.list.write_text("# comment\n\n" + "\n".join(refs) + "  # trailing\n",
                             encoding="utf-8")

    def main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["--list", str(self.list), "--upstream", "main",
                             "--repo", str(self.repo), "--no-fetch",
                             "--no-build", *args])
        return code, out.getvalue(), err.getvalue()

    def content(self, name):
        return (self.worktree / name).read_text(encoding="utf-8")


class ListTest(RepoTest):
    def test_comments_and_blanks_are_skipped(self):
        self.listing("x", "y")
        self.assertEqual(cli.read_list(self.list), ["x", "y"])

    def test_empty_list_is_broken(self):
        self.list.write_text("# nothing\n", encoding="utf-8")
        with self.assertRaises(cli.Broken):
            cli.read_list(self.list)


class MergeTest(RepoTest):
    def test_merges_every_branch_on_upstream(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        self.branch("fix-b", {"b.cc": edited(5, "fixed b\n")})
        self.listing("fix-a", "fix-b")
        code, out, _ = self.main()
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("fixed a", self.content("a.cc"))
        self.assertIn("fixed b", self.content("b.cc"))
        self.assertEqual(run(self.worktree, "log", "-1", "--format=%s").strip(),
                         "[Integration] Merge fix-b")

    def test_regenerates_from_the_new_upstream(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        self.listing("fix-a")
        self.assertEqual(self.main()[0], cli.EXIT_OK)
        write(self.repo, "b.cc", edited(30, "upstream moved on\n"))
        commit(self.repo, "later upstream")
        self.assertEqual(self.main()[0], cli.EXIT_OK)
        self.assertIn("upstream moved on", self.content("b.cc"))
        self.assertEqual(run(self.worktree, "rev-parse", "HEAD^1").strip(),
                         run(self.repo, "rev-parse", "main").strip())

    def test_upstreamed_branch_is_skipped_and_reported(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        run(self.repo, "cherry-pick", "fix-a")  # lands upstream as a new commit
        self.listing("fix-a")
        code, out, _ = self.main()
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("already upstream", out)
        self.assertEqual(run(self.worktree, "rev-parse", "HEAD").strip(),
                         run(self.repo, "rev-parse", "main").strip())

    def test_branch_inside_a_listed_stack_is_contained(self):
        self.branch("step-1", {"a.cc": edited(5, "step 1\n")})
        self.branch("step-2", {"b.cc": edited(5, "step 2\n")}, start="step-1")
        self.listing("step-2", "step-1")
        code, out, _ = self.main()
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("inside a listed stack):\n  step-1", out)

    def test_conflict_resume_and_replay(self):
        self.branch("fix-a", {"a.cc": edited(10, "fixed a\n")})
        self.branch("fix-b", {"a.cc": edited(10, "fixed b\n")})
        self.branch("fix-c", {"b.cc": edited(5, "fixed c\n")})
        self.listing("fix-a", "fix-b", "fix-c")
        code, _, err = self.main()
        self.assertEqual(code, cli.EXIT_CONFLICT)
        self.assertIn("conflict merging fix-b: a.cc", err)

        # Resuming before resolving stays broken.
        self.assertEqual(self.main("--resume")[0], cli.EXIT_BROKEN)
        # A plain rerun refuses to discard the merge in progress.
        self.assertEqual(self.main()[0], cli.EXIT_BROKEN)

        write(self.worktree, "a.cc", edited(10, "fixed a and b\n"))
        run(self.worktree, "add", "a.cc")
        code, out, _ = self.main("--resume")
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("fixed c", self.content("b.cc"))

        # The next regeneration replays the recorded resolution by itself.
        code, out, _ = self.main()
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("conflicts replayed by rerere:\n  fix-b", out)
        self.assertIn("fixed a and b", self.content("a.cc"))

    def test_dirty_worktree_is_broken(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        self.listing("fix-a")
        self.main()
        write(self.worktree, "b.cc", edited(1, "stray edit\n"))
        code, _, err = self.main()
        self.assertEqual(code, cli.EXIT_BROKEN)
        self.assertIn("uncommitted", err)

    def test_clean_removes_worktree_and_branch(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        self.listing("fix-a")
        self.main()
        self.assertEqual(self.main("--clean")[0], cli.EXIT_OK)
        self.assertFalse(self.worktree.exists())
        self.assertFalse(cli.git_ok(self.repo, "rev-parse", "--verify",
                                    "refs/heads/integration"))

    def test_builds_after_merging(self):
        self.branch("fix-a", {"a.cc": edited(5, "fixed a\n")})
        self.listing("fix-a")
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(cli, "build",
                               return_value=Path("xenia_canary.exe")) as build, \
             redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["--list", str(self.list), "--upstream", "main",
                             "--repo", str(self.repo), "--no-fetch"])
        self.assertEqual(code, cli.EXIT_OK)
        build.assert_called_once_with(self.worktree, "release", False)


if __name__ == "__main__":
    unittest.main()
