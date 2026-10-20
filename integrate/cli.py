"""Command line entry point (run on Windows by integrate.ps1).

Regenerates the `integration` branch: the latest upstream with every branch
in config/integration.txt merged in, in order, so one build carries all of
the fork's pending fixes and features. The branch is only for running and
testing: new work is never cut from it, and nothing is fixed on it that
isn't also on a topic branch. Conflict resolutions are recorded by rerere,
so the next regeneration replays them.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import traceback
from dataclasses import dataclass, field
from pathlib import Path

from ab_build.cli import (Broken, exe_path, git, git_ok, has_telemetry,
                          repo_root, rev, xb)

EXIT_OK = 0
EXIT_CONFLICT = 1
EXIT_BROKEN = 2

BRANCH = "integration"
WORKTREE = "integration"
DEFAULT_LIST = Path(__file__).resolve().parent.parent / "config" / "integration.txt"
# rerere replays recorded resolutions; autoupdate stages them so a merge
# whose conflicts were all replayed can be committed without a person.
RERERE = ("-c", "rerere.enabled=true", "-c", "rerere.autoupdate=true")


def read_list(path: Path) -> list[str]:
    """Refs to merge, in order; `#` starts a comment."""
    refs = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            refs.append(line)
    if not refs:
        raise Broken(f"{path} lists no branches")
    return refs


@dataclass
class Report:
    merged: list[str] = field(default_factory=list)
    replayed: list[str] = field(default_factory=list)
    contained: list[str] = field(default_factory=list)
    upstreamed: list[str] = field(default_factory=list)


def is_upstreamed(repo: Path, upstream: str, ref: str) -> bool:
    """Every non-merge commit of ref already has an equivalent upstream."""
    return not any(line.startswith("+") for line in
                   git(repo, "cherry", upstream, ref).splitlines())


def unmerged_paths(path: Path) -> list[str]:
    return git(path, "diff", "--name-only", "--diff-filter=U").split()


def merge_in_progress(path: Path) -> bool:
    return git_ok(path, "rev-parse", "--verify", "-q", "MERGE_HEAD")


def dirty(path: Path) -> bool:
    # Submodules are excluded: `xb setup` leaves some marked as modified.
    return bool(git(path, "status", "--porcelain", "--untracked-files=no",
                    "--ignore-submodules").strip())


def commit_merge(path: Path) -> None:
    # With rerere on, so the resolution is recorded for the next run.
    git(path, *RERERE, "commit", "--no-edit")


def start(repo: Path, path: Path, upstream_sha: str) -> None:
    """Point the integration worktree at upstream, creating it if needed."""
    if not path.exists():
        git(repo, "worktree", "add", "-B", BRANCH, str(path), upstream_sha)
        return
    if merge_in_progress(path):
        raise Broken(f"{path} has a merge in progress; resolve it and rerun "
                     f"with --resume, or --clean")
    if dirty(path):
        raise Broken(f"{path} has uncommitted changes")
    if git(path, "rev-parse", "--abbrev-ref", "HEAD").strip() != BRANCH:
        raise Broken(f"{path} isn't on branch {BRANCH}")
    git(path, "reset", "-q", "--hard", upstream_sha)


def resume(path: Path) -> None:
    if not path.exists():
        raise Broken(f"{path} doesn't exist; run without --resume")
    if merge_in_progress(path):
        conflicts = unmerged_paths(path)
        if conflicts:
            raise Broken(f"still unresolved in {path}: {', '.join(conflicts)}")
        commit_merge(path)


def merge_all(repo: Path, path: Path, refs: list[str], upstream: str,
              report: Report) -> str | None:
    """Merge each ref in order. Returns the ref that conflicted, if any."""
    for ref in refs:
        sha = rev(repo, ref)
        if is_upstreamed(repo, upstream, sha):
            report.upstreamed.append(ref)
            continue
        if git_ok(path, "merge-base", "--is-ancestor", sha, "HEAD"):
            # Already merged (on --resume), or inside a branch listed earlier.
            report.contained.append(ref)
            continue
        proc = subprocess.run(
            ["git", "-C", str(path), *RERERE, "merge", "--no-ff", "-m",
             f"[Integration] Merge {ref}", sha],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        if proc.returncode == 0:
            report.merged.append(ref)
            continue
        if not merge_in_progress(path):
            raise Broken(f"merging {ref} failed: {proc.stderr.strip()}")
        if unmerged_paths(path):
            return ref
        commit_merge(path)
        report.replayed.append(ref)
    return None


def build(path: Path, config: str, want_telemetry: bool) -> Path:
    try:
        xb(path, "setup")
    except Broken as e:
        # The parallel submodule fetch in a fresh worktree fails transiently.
        print(f"{e}; retrying once", file=sys.stderr)
        xb(path, "setup")
    xb(path, "build", f"--config={config}", "--target=xenia-app")
    exe = exe_path(path, config)
    if not exe.is_file():
        raise Broken(f"build finished but {exe} is missing")
    if want_telemetry and not has_telemetry(exe):
        raise Broken(f"{exe} has no --telemetry_file, but telemetry is listed")
    return exe


def clean(repo: Path) -> None:
    path = repo / ".claude" / "worktrees" / WORKTREE
    if path.exists():
        git(repo, "worktree", "remove", "--force", str(path))
        print(f"removed {path}")
    if git_ok(repo, "rev-parse", "--verify", f"refs/heads/{BRANCH}"):
        git(repo, "branch", "-D", BRANCH)
        print(f"deleted {BRANCH}")


def print_report(report: Report) -> None:
    for label, refs in (("merged", report.merged),
                        ("merged, conflicts replayed by rerere", report.replayed),
                        ("already in the branch (merged before --resume, or "
                         "inside a listed stack)", report.contained),
                        ("skipped, already upstream (drop from the list)",
                         report.upstreamed)):
        if refs:
            print(f"{label}:")
            for ref in refs:
                print(f"  {ref}")


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="integrate",
        description="Regenerate the integration branch: upstream plus every "
                    "listed fork branch, merged, then build it.")
    ap.add_argument("--list", default=str(DEFAULT_LIST),
                    help="branch list (default: config/integration.txt)")
    ap.add_argument("--upstream", default="upstream/canary_experimental")
    ap.add_argument("--config", default="release",
                    choices=("release", "debug", "checked"))
    ap.add_argument("--resume", action="store_true",
                    help="after resolving a conflict: commit it and continue")
    ap.add_argument("--no-fetch", action="store_true",
                    help="don't fetch origin and upstream first")
    ap.add_argument("--no-build", action="store_true", help="merge only")
    ap.add_argument("--push", action="store_true",
                    help="force-push the branch to origin (for a Fork CI build)")
    ap.add_argument("--clean", action="store_true",
                    help="remove the worktree and branch, then exit")
    ap.add_argument("--repo", default=".", help=argparse.SUPPRESS)
    return ap.parse_args(argv)


def _run(args: argparse.Namespace) -> int:
    repo = repo_root(Path(args.repo))
    if args.clean:
        clean(repo)
        return EXIT_OK
    refs = read_list(Path(args.list))
    if not args.no_fetch:
        for remote in {r.split("/", 1)[0] for r in [args.upstream, *refs]
                       if "/" in r and git_ok(repo, "remote", "get-url",
                                               r.split("/", 1)[0])}:
            git(repo, "fetch", "-q", "--prune", remote)
    path = repo / ".claude" / "worktrees" / WORKTREE
    upstream_sha = rev(repo, args.upstream)
    if args.resume:
        resume(path)
    else:
        start(repo, path, upstream_sha)
    print(f"{BRANCH}: {args.upstream} {upstream_sha[:9]} + {len(refs)} listed")
    report = Report()
    conflict = merge_all(repo, path, refs, args.upstream, report)
    print_report(report)
    if conflict:
        print(f"\nconflict merging {conflict}: "
              f"{', '.join(unmerged_paths(path))}\n"
              f"  Resolve and `git add` them in {path} (don't commit), then "
              f"rerun with --resume.\n  If a topic branch is wrong, fix it "
              f"there instead and rerun without --resume.", file=sys.stderr)
        return EXIT_CONFLICT
    if args.push:
        git(path, "push", "-q", "--force-with-lease", "origin",
            f"{BRANCH}:{BRANCH}")
        print(f"pushed {BRANCH} to origin")
    if args.no_build:
        return EXIT_OK
    want_telemetry = any("telemetry" in ref for ref in refs)
    exe = build(path, args.config, want_telemetry)
    print(f"\nexe: {exe}\n\ngame_launch(plugin=\"xenia\", exe=<path above>, "
          f"path=<game>)")
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return _run(args)
    except Broken as e:
        print(f"integrate broken: {e}", file=sys.stderr)
        return EXIT_BROKEN
    except Exception:  # A traceback must not read as exit 1 (conflict).
        traceback.print_exc()
        return EXIT_BROKEN
