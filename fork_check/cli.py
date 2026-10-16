"""Command line entry point (run inside WSL by check.ps1)."""

from __future__ import annotations

import argparse
import sys
import traceback

from fork_check import runners, snapshot
from fork_check.diffparse import parse_changed_lines
from fork_check.excludes import analyzable, load_codacy_excludes
from fork_check.findings import (EXIT_BROKEN, EXIT_CLEAN, EXIT_FINDINGS,
                                 CheckerBroken, format_report, select)


def _tool_list(text: str) -> list[str]:
    tools = [t.strip() for t in text.split(",") if t.strip()]
    unknown = [t for t in tools if t not in runners.TOOLS]
    if unknown or not tools:
        raise argparse.ArgumentTypeError(
            f"unknown tool(s) {unknown}; choose from {', '.join(runners.TOOLS)}")
    return tools


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="fork_check")
    ap.add_argument("--win-repo", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--scope", choices=("changed", "files", "full"), default="changed")
    ap.add_argument("--tools", type=_tool_list, default=list(runners.TOOLS))
    ap.add_argument("--dirty", action="store_true")
    return ap.parse_args(argv)


def _run(args: argparse.Namespace) -> int:
    runners.require(args.tools)
    if args.dirty:
        print("warning: uncommitted changes are not analyzed", file=sys.stderr)
    root = snapshot.prepare(args.win_repo, args.commit, args.base)
    changed = parse_changed_lines(snapshot.git(
        root, "-c", "core.quotePath=false", "diff", "-U0", "--no-color",
        "--no-ext-diff", "-M", args.base, args.commit))
    codacy = root / ".codacy.yaml"
    excludes = load_codacy_excludes(codacy.read_text()) if codacy.exists() else []
    if args.scope == "full":
        candidates = snapshot.git(root, "ls-files").splitlines()
    else:
        candidates = list(changed)
    files = sorted(p for p in candidates if analyzable(p, excludes))
    if not files:
        print("-- no C/C++ files to check")
        return EXIT_CLEAN

    findings = []
    notes: list[str] = []
    if "cppcheck" in args.tools:
        findings += runners.run_cppcheck(root, files)
    if "flawfinder" in args.tools:
        findings += runners.run_flawfinder(root, files)
    if "cpplint" in args.tools:
        findings += runners.run_cpplint(root, files)
    if "clang-tidy" in args.tools:
        build = snapshot.configure(root)
        tidy_findings, notes = runners.run_clang_tidy(root, build, files)
        findings += tidy_findings

    selected = [x for x in select(findings, args.scope, changed)
                if analyzable(x.path, excludes)]
    for note in notes:
        print(note, file=sys.stderr)
    print(f"checked {len(files)} file(s), {args.base[:10]}..{args.commit[:10]}, "
          f"scope {args.scope}", file=sys.stderr)
    print(format_report(selected))
    return EXIT_FINDINGS if selected else EXIT_CLEAN


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return _run(args)
    except CheckerBroken as e:
        print(f"checker broken: {e}", file=sys.stderr)
        return EXIT_BROKEN
    except Exception:  # A crash must never read as exit 1, "findings".
        traceback.print_exc()
        print("checker broken: unexpected error (traceback above)", file=sys.stderr)
        return EXIT_BROKEN
