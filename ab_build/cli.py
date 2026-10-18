"""Command line entry point (run on Windows by ab-build.ps1).

Builds a fix branch and its upstream base side by side, each with the
telemetry branch merged in, so an A/B run in game-mcp reports the same live
signals on both sides and the builds differ only by the fix
(Ecredes/xenia-canary#31).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import traceback
from dataclasses import dataclass
from pathlib import Path

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_BROKEN = 2

SIDES = ("base", "fix")
# game-mcp detects telemetry support the same way (plugins/xenia.py).
TELEMETRY_MARK = b"telemetry_file"
MERGE_MESSAGE = "[Testing] A/B: merge telemetry"


class Broken(Exception):
    """Something failed; the A/B pair is not usable. Exit 2."""


def git(repo: Path, *args: str) -> str:
    proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    if proc.returncode:
        raise Broken(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout


def git_ok(repo: Path, *args: str) -> bool:
    return subprocess.run(["git", "-C", str(repo), *args],
                          capture_output=True).returncode == 0


def rev(repo: Path, ref: str) -> str:
    return git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}").strip()


def side_name(ref: str) -> str:
    """A branch-safe name for the pair, from the fix ref."""
    name = re.sub(r"[^A-Za-z0-9._-]+", "-", ref).strip("-.")
    if not name:
        raise Broken(f"can't derive a name from {ref!r}; pass --name")
    return name


@dataclass
class Side:
    label: str
    source_sha: str
    branch: str
    path: Path


@dataclass
class Plan:
    repo: Path
    name: str
    base_sha: str
    fix_sha: str
    telemetry_sha: str
    sides: dict[str, Side]


def repo_root(start: Path) -> Path:
    """The main checkout, also when started from a linked worktree."""
    common = git(start, "rev-parse", "--path-format=absolute", "--git-common-dir")
    return Path(common.strip()).parent


def make_plan(repo: Path, fix: str, base: str | None = None,
              telemetry: str = "origin/testing/telemetry",
              upstream: str = "upstream/canary_experimental",
              name: str | None = None) -> Plan:
    fix_sha = rev(repo, fix)
    base_sha = rev(repo, base) if base else git(
        repo, "merge-base", fix_sha, rev(repo, upstream)).strip()
    if base_sha == fix_sha:
        raise Broken(f"{fix} has no commits on top of its base {base_sha[:9]}")
    telemetry_sha = rev(repo, telemetry)
    name = side_name(name or fix)
    worktrees = repo / ".claude" / "worktrees"
    sides = {
        label: Side(label, sha, f"ab/{name}/{label}", worktrees / f"ab-{name}-{label}")
        for label, sha in (("base", base_sha), ("fix", fix_sha))
    }
    return Plan(repo, name, base_sha, fix_sha, telemetry_sha, sides)


def _contains(repo: Path, ancestor: str, ref: str) -> bool:
    return git_ok(repo, "merge-base", "--is-ancestor", ancestor, ref)


def prepare_side(plan: Plan, side: Side, reuse: bool) -> None:
    """Create the side's worktree and merge telemetry into it."""
    if side.path.exists():
        if not reuse:
            raise Broken(f"{side.path} already exists; pass --reuse to keep it "
                         f"or --clean to remove the pair")
        for needed, what in ((side.source_sha, "the source commit"),
                             (plan.telemetry_sha, "telemetry")):
            if not _contains(side.path, needed, "HEAD"):
                raise Broken(f"{side.path} doesn't contain {what} "
                             f"({needed[:9]}); finish the merge or --clean")
        # Submodules are excluded: `xb setup` leaves some marked as modified.
        if git(side.path, "status", "--porcelain", "--untracked-files=no",
               "--ignore-submodules").strip():
            raise Broken(f"{side.path} has uncommitted changes")
        return
    if git_ok(plan.repo, "rev-parse", "--verify", f"refs/heads/{side.branch}"):
        raise Broken(f"branch {side.branch} already exists; pass --clean first")
    git(plan.repo, "worktree", "add", "-b", side.branch, str(side.path),
        side.source_sha)
    proc = subprocess.run(
        ["git", "-C", str(side.path), "-c", "rerere.enabled=true", "merge",
         "--no-ff", "-m", MERGE_MESSAGE, plan.telemetry_sha],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode:
        conflicts = git(side.path, "diff", "--name-only",
                        "--diff-filter=U").split()
        raise Broken(
            f"telemetry doesn't merge cleanly into {side.label} "
            f"({side.source_sha[:9]}); conflicts: {', '.join(conflicts) or '?'}\n"
            f"  Resolve them in {side.path}, `git commit`, then rerun with "
            f"--reuse (rerere records the resolution for the other side).")


def check_isolation(plan: Plan) -> list[str]:
    """Files whose A/B diff differs from the fix's own diff (empty: clean).

    The two builds must differ by exactly the fix. If telemetry and the fix
    touch the same lines, the merge changes what the fix does and an A/B
    difference can't be attributed to the fix alone.
    """
    base, fix = (plan.sides[s].branch for s in SIDES)
    want = _per_file_diff(plan.repo, plan.base_sha, plan.fix_sha)
    got = _per_file_diff(plan.repo, base, fix)
    return sorted(path for path in set(want) | set(got)
                  if want.get(path) != got.get(path))


def _per_file_diff(repo: Path, a: str, b: str) -> dict[str, str]:
    """Path -> patch body, ignoring where the hunks sit in the file.

    Drops the `index` line (blob ids differ whenever telemetry also touches
    the file) and the hunk positions (telemetry lines earlier in the file
    shift them). Changed and context lines are kept, so telemetry edits
    within the fix's hunks still count as overlap.
    """
    patch = git(repo, "diff", "--no-color", "--no-ext-diff", "--binary", a, b)
    files: dict[str, list[str]] = {}
    current: list[str] | None = None
    for line in patch.splitlines():
        if line.startswith("diff --git "):
            path = line.split(" b/", 1)[-1]
            current = files.setdefault(path, [])
        elif current is None or line.startswith("index "):
            continue
        elif line.startswith("@@"):
            current.append("@@")
        else:
            current.append(line)
    return {path: "\n".join(body) for path, body in files.items()}


def xb(path: Path, *args: str) -> None:
    # By absolute path: cmd doesn't search the working directory when
    # NoDefaultCurrentDirectoryInExePath is set.
    proc = subprocess.run(["cmd", "/c", str(path / "xb.bat"), *args],
                          cwd=str(path))
    if proc.returncode:
        raise Broken(f"xb {' '.join(args)} failed in {path} "
                     f"(exit {proc.returncode})")


def exe_path(path: Path, config: str) -> Path:
    return path / "build" / "bin" / "Windows" / config.capitalize() / "xenia_canary.exe"


def has_telemetry(exe: Path) -> bool:
    return TELEMETRY_MARK in exe.read_bytes()


def build_side(side: Side, config: str) -> Path:
    try:
        xb(side.path, "setup")
    except Broken as e:
        # The parallel submodule fetch in a fresh worktree fails transiently.
        print(f"{e}; retrying once", file=sys.stderr)
        xb(side.path, "setup")
    xb(side.path, "build", f"--config={config}", "--target=xenia-app")
    exe = exe_path(side.path, config)
    if not exe.is_file():
        raise Broken(f"build finished but {exe} is missing")
    if not has_telemetry(exe):
        raise Broken(f"{exe} has no --telemetry_file; is the telemetry merge "
                     f"in this worktree?")
    return exe


def clean(repo: Path, name: str) -> None:
    name = side_name(name)
    for label in SIDES:
        path = repo / ".claude" / "worktrees" / f"ab-{name}-{label}"
        branch = f"ab/{name}/{label}"
        if path.exists():
            git(repo, "worktree", "remove", "--force", str(path))
            print(f"removed {path}")
        if git_ok(repo, "rev-parse", "--verify", f"refs/heads/{branch}"):
            git(repo, "branch", "-D", branch)
            print(f"deleted {branch}")


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="ab_build",
        description="Build a fix branch and its upstream base, both with "
                    "telemetry merged in, for an A/B run in game-mcp.")
    ap.add_argument("fix", help="the fix branch or commit")
    ap.add_argument("--base", help="base commit (default: merge-base with --upstream)")
    ap.add_argument("--telemetry", default="origin/testing/telemetry")
    ap.add_argument("--upstream", default="upstream/canary_experimental")
    ap.add_argument("--name", help="pair name (default: from the fix ref)")
    ap.add_argument("--config", default="release", choices=("release", "debug", "checked"))
    ap.add_argument("--reuse", action="store_true",
                    help="keep existing worktrees (after resolving a conflict)")
    ap.add_argument("--no-build", action="store_true",
                    help="prepare and check the pair without building")
    ap.add_argument("--allow-overlap", action="store_true",
                    help="build even if telemetry changes the fix's diff")
    ap.add_argument("--clean", action="store_true",
                    help="remove the pair's worktrees and branches, then exit")
    ap.add_argument("--repo", default=".", help=argparse.SUPPRESS)
    return ap.parse_args(argv)


def _run(args: argparse.Namespace) -> int:
    repo = repo_root(Path(args.repo))
    if args.clean:
        clean(repo, args.name or args.fix)
        return EXIT_OK
    plan = make_plan(repo, args.fix, args.base, args.telemetry, args.upstream,
                     args.name)
    print(f"pair {plan.name}: base {plan.base_sha[:9]}, fix {plan.fix_sha[:9]}, "
          f"telemetry {plan.telemetry_sha[:9]}")
    for label in SIDES:
        prepare_side(plan, plan.sides[label], args.reuse)
    overlap = check_isolation(plan)
    if overlap:
        print("telemetry changes the fix's diff in: " + ", ".join(overlap),
              file=sys.stderr)
        if not args.allow_overlap:
            print("an A/B difference can't be attributed to the fix alone; "
                  "rerun with --allow-overlap to build anyway", file=sys.stderr)
            return EXIT_MISMATCH
    else:
        print("isolation: the A/B diff equals the fix's diff")
    if args.no_build:
        return EXIT_OK
    exes = {label: build_side(plan.sides[label], args.config) for label in SIDES}
    print()
    for label in SIDES:
        print(f"{label}: {exes[label]}")
    print("\ngame_launch(plugin=\"xenia\", exe=<path above>, path=<game>), "
          "once per side; telemetry is detected automatically.")
    return EXIT_MISMATCH if overlap else EXIT_OK


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return _run(args)
    except Broken as e:
        print(f"ab-build broken: {e}", file=sys.stderr)
        return EXIT_BROKEN
    except Exception:  # A traceback must not read as exit 1 (mismatch).
        traceback.print_exc()
        return EXIT_BROKEN
