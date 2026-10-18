import io
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from ab_build import cli

LINES = [f"line {i}\n" for i in range(1, 41)]


def run(repo, *args):
    subprocess.run(["git", "-C", str(repo), *args], check=True,
                   capture_output=True)


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
    """A repo with `main` (the upstream), a `fix` branch and a `telemetry`
    branch, both cut from main."""

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

    def tearDown(self):
        # Worktrees hold no locks after their processes exit, but Windows can
        # still refuse to delete read-only pack files.
        shutil.rmtree(self.tmp, onerror=lambda f, p, e: (Path(p).chmod(0o700), f(p)))

    def branch(self, name, files):
        run(self.repo, "checkout", "-q", "-b", name, "main")
        for path, lines in files.items():
            write(self.repo, path, lines)
        commit(self.repo, name)
        run(self.repo, "checkout", "-q", "main")

    def plan(self, **kwargs):
        return cli.make_plan(self.repo, "fix", telemetry="telemetry",
                             upstream="main", **kwargs)

    def prepare(self, plan, reuse=False):
        for label in cli.SIDES:
            cli.prepare_side(plan, plan.sides[label], reuse)

    def main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["fix", "--telemetry", "telemetry", "--upstream", "main",
                             "--repo", str(self.repo), *args])
        return code, out.getvalue(), err.getvalue()


class PlanTest(RepoTest):
    def test_side_name(self):
        self.assertEqual(cli.side_name("gpu/vulkan-edram-compute-modified"),
                         "gpu-vulkan-edram-compute-modified")
        with self.assertRaises(cli.Broken):
            cli.side_name("//")

    def test_base_is_the_merge_base(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        run(self.repo, "checkout", "-q", "main")
        write(self.repo, "a.cc", edited(30, "upstream moved on\n"))
        commit(self.repo, "later upstream")
        plan = self.plan()
        self.assertEqual(plan.base_sha, cli.rev(self.repo, "main~1"))
        self.assertEqual(plan.sides["fix"].branch, "ab/fix/fix")
        self.assertEqual(plan.sides["base"].path,
                         self.repo / ".claude" / "worktrees" / "ab-fix-base")

    def test_fix_without_commits_is_broken(self):
        run(self.repo, "branch", "fix", "main")
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        with self.assertRaises(cli.Broken):
            self.plan()


class IsolationTest(RepoTest):
    def test_telemetry_in_other_files_is_clean(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        plan = self.plan()
        self.prepare(plan)
        self.assertEqual(cli.check_isolation(plan), [])
        for label in cli.SIDES:
            path = plan.sides[label].path
            self.assertIn("probe", (path / "b.cc").read_text())

    def test_telemetry_far_away_in_the_same_file_is_clean(self):
        # Telemetry adds lines above the fix, shifting its hunk positions.
        self.branch("fix", {"a.cc": edited(30, "fixed\n")})
        self.branch("telemetry", {"a.cc": ["probe\n", "probe\n"] + LINES})
        plan = self.plan()
        self.prepare(plan)
        self.assertEqual(cli.check_isolation(plan), [])

    def test_telemetry_next_to_the_fix_is_overlap(self):
        self.branch("fix", {"a.cc": edited(10, "fixed\n")})
        self.branch("telemetry", {"a.cc": edited(12, "probe\n")})
        plan = self.plan()
        self.prepare(plan)
        self.assertEqual(cli.check_isolation(plan), ["a.cc"])


class PrepareTest(RepoTest):
    def test_conflict_is_broken_and_names_the_file(self):
        self.branch("fix", {"a.cc": edited(10, "fixed\n")})
        self.branch("telemetry", {"a.cc": edited(10, "probe\n")})
        plan = self.plan()
        with self.assertRaises(cli.Broken) as cm:
            cli.prepare_side(plan, plan.sides["fix"], reuse=False)
        self.assertIn("a.cc", str(cm.exception))
        self.assertIn("--reuse", str(cm.exception))

    def test_existing_worktree_needs_reuse(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        plan = self.plan()
        self.prepare(plan)
        with self.assertRaises(cli.Broken):
            self.prepare(plan)
        self.prepare(plan, reuse=True)  # no error

    def test_reuse_rejects_an_unfinished_merge(self):
        self.branch("fix", {"a.cc": edited(10, "fixed\n")})
        self.branch("telemetry", {"a.cc": edited(10, "probe\n")})
        plan = self.plan()
        with self.assertRaises(cli.Broken):
            cli.prepare_side(plan, plan.sides["fix"], reuse=False)
        run(plan.sides["fix"].path, "merge", "--abort")
        with self.assertRaises(cli.Broken) as cm:
            cli.prepare_side(plan, plan.sides["fix"], reuse=True)
        self.assertIn("telemetry", str(cm.exception))

    def test_clean_removes_worktrees_and_branches(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        plan = self.plan()
        self.prepare(plan)
        with redirect_stdout(io.StringIO()):
            cli.clean(self.repo, "fix")
        for side in plan.sides.values():
            self.assertFalse(side.path.exists())
            self.assertFalse(cli.git_ok(self.repo, "rev-parse", "--verify",
                                        f"refs/heads/{side.branch}"))


class MainTest(RepoTest):
    def test_clean_pair_without_build_exits_0(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        code, out, _ = self.main("--no-build")
        self.assertEqual(code, cli.EXIT_OK)
        self.assertIn("isolation: the A/B diff equals the fix's diff", out)

    def test_overlap_exits_1_without_building(self):
        self.branch("fix", {"a.cc": edited(10, "fixed\n")})
        self.branch("telemetry", {"a.cc": edited(12, "probe\n")})
        with mock.patch.object(cli, "build_side") as build:
            code, _, err = self.main()
        self.assertEqual(code, cli.EXIT_MISMATCH)
        self.assertIn("a.cc", err)
        build.assert_not_called()

    def test_conflict_exits_2(self):
        self.branch("fix", {"a.cc": edited(10, "fixed\n")})
        self.branch("telemetry", {"a.cc": edited(10, "probe\n")})
        code, _, err = self.main("--no-build")
        self.assertEqual(code, cli.EXIT_BROKEN)
        self.assertIn("ab-build broken", err)

    def test_builds_both_sides_and_prints_exes(self):
        self.branch("fix", {"a.cc": edited(5, "fixed\n")})
        self.branch("telemetry", {"b.cc": edited(5, "probe\n")})
        built = []

        def fake_build(side, config):
            built.append(side.label)
            return side.path / "xenia_canary.exe"

        with mock.patch.object(cli, "build_side", side_effect=fake_build):
            code, out, _ = self.main()
        self.assertEqual(code, cli.EXIT_OK)
        self.assertEqual(built, ["base", "fix"])
        self.assertIn("ab-fix-fix", out)


class BuildSideTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.side = cli.Side("fix", "0" * 40, "ab/x/fix", self.tmp)
        self.exe = cli.exe_path(self.tmp, "release")
        self.exe.parent.mkdir(parents=True)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_exe_with_telemetry_passes(self):
        self.exe.write_bytes(b"\0--telemetry_file\0")
        with mock.patch.object(cli, "xb") as xb:
            self.assertEqual(cli.build_side(self.side, "release"), self.exe)
        self.assertEqual(xb.call_args_list[1].args[1:],
                         ("build", "--config=release", "--target=xenia-app"))

    def test_setup_is_retried_once(self):
        self.exe.write_bytes(b"\0--telemetry_file\0")
        calls = []

        def flaky(path, *args):
            calls.append(args[0])
            if calls == ["setup"]:
                raise cli.Broken("xb setup failed")

        with mock.patch.object(cli, "xb", side_effect=flaky), \
             redirect_stderr(io.StringIO()):
            cli.build_side(self.side, "release")
        self.assertEqual(calls, ["setup", "setup", "build"])

    def test_setup_failing_twice_is_broken(self):
        with mock.patch.object(cli, "xb", side_effect=cli.Broken("no")), \
             redirect_stderr(io.StringIO()), self.assertRaises(cli.Broken):
            cli.build_side(self.side, "release")

    def test_exe_without_telemetry_is_broken(self):
        self.exe.write_bytes(b"plain build")
        with mock.patch.object(cli, "xb"), self.assertRaises(cli.Broken):
            cli.build_side(self.side, "release")

    def test_missing_exe_is_broken(self):
        self.exe.parent.rmdir()
        with mock.patch.object(cli, "xb"), self.assertRaises(cli.Broken):
            cli.build_side(self.side, "release")


if __name__ == "__main__":
    unittest.main()
