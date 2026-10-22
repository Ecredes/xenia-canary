"""Dependency and workflow audit (`python3 -m fork_check audit`, run by audit.ps1).

Whole-repo, not per-branch: osv-scanner for known vulnerabilities in the
third_party submodules and zizmor for GitHub Actions workflow problems.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import traceback

from fork_check import snapshot
from fork_check.findings import (EXIT_BROKEN, EXIT_CLEAN, EXIT_FINDINGS,
                                 CheckerBroken, Finding, format_report, select)
from fork_check.parsers import rel

INSTALL = {
    "osv-scanner": "see README.md (pinned release binary, checksum verified)",
    "zizmor": "pipx install zizmor==1.30.1",
}

_SECTION_RE = re.compile(r'^\s*\[submodule\s+"[^"]*"\]\s*$')
_PATH_RE = re.compile(r"^\s*path\s*=\s*(.+?)\s*$")
# `git submodule status`: state prefix (' ', '-', '+' or 'U'), sha, path.
_STATUS_RE = re.compile(r"^([ +U-])([0-9a-f]{40}) (\S+)(?: \(.*\))?$")


def submodule_lines(gitmodules: str) -> dict[str, int]:
    """Maps each submodule path to the line of its `[submodule ...]` header."""
    out: dict[str, int] = {}
    section = 0
    for no, line in enumerate(gitmodules.splitlines(), 1):
        if _SECTION_RE.match(line):
            section = no
        elif section and (match := _PATH_RE.match(line)):
            out[match.group(1)] = section
    return out


def parse_submodule_status(text: str) -> dict[str, tuple[str, bool]]:
    """Maps each submodule path to (pinned sha, checked out)."""
    out = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = _STATUS_RE.match(line)
        if match is None:
            raise CheckerBroken(f"git submodule status: unexpected line: {line}")
        state, sha, path = match.groups()
        out[path] = (sha, state != "-")
    return out


def parse_osv(data: dict, root: str, sections: dict[str, int],
              pinned: dict[str, str]) -> tuple[list[Finding], int]:
    """Findings for top-level submodules, and how many other packages were skipped.

    Skipped packages are developer lockfiles inside submodules (yarn.lock,
    requirements.txt) and nested submodules, none of which xenia builds.
    """
    if "results" not in data:
        raise CheckerBroken("osv-scanner: JSON output has no `results`")
    findings = []
    skipped = 0
    for result in data["results"]:
        path = rel(result["source"]["path"], root)
        packages = result.get("packages", [])
        if path not in sections:
            skipped += len(packages)
            continue
        for package in packages:
            # osv-scanner estimates the closest known commit from file
            # contents, so `matched` can be older than the pinned commit.
            matched = package["package"].get("commit", "")[:10] or "?"
            summaries = {v["id"]: v.get("summary", "") for v in package["vulnerabilities"]}
            for group in package.get("groups", []):
                vuln_id = group["ids"][0]
                severity = group.get("max_severity") or "?"
                message = (f"{path}: {summaries.get(vuln_id) or 'no summary'} "
                           f"[CVSS {severity}] (matched {matched}, "
                           f"pinned {pinned.get(path, '?')[:10]})")
                findings.append(Finding(".gitmodules", sections[path], "osv",
                                        vuln_id, message))
    return findings, skipped


def parse_zizmor(data: list) -> list[Finding]:
    findings = []
    for item in data:
        if item.get("ignored"):
            continue
        primary = [loc for loc in item["locations"]
                   if loc["symbolic"]["kind"] == "Primary"]
        if not primary:
            raise CheckerBroken(f"zizmor: {item['ident']} has no primary location")
        loc = primary[0]
        key = loc["symbolic"]["key"]
        if "Local" not in key:
            raise CheckerBroken(f"zizmor: unexpected location key {key}")
        det = item["determinations"]
        message = (f"{det['severity']}/{det['confidence']}: {item['desc']}: "
                   f"{loc['symbolic']['annotation']}")
        findings.append(Finding(key["Local"]["verbatim_path"],
                                loc["concrete"]["location"]["start_point"]["row"] + 1,
                                "zizmor", item["ident"], message))
    return findings


def zizmor_ok(code: int) -> bool:
    # 10-14: findings, by highest severity (unknown through high).
    return code == 0 or 10 <= code <= 14


def _run(cmd: list[str], cwd) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                          errors="replace")


def fetch_missing_submodules(root, status: dict[str, tuple[str, bool]]) -> list[str]:
    """Checks out submodules the Linux build skips (DirectX-Headers, ...).

    Shallow and not recursive: osv-scanner only needs the top-level files.
    Returns the paths that still couldn't be checked out.
    """
    missing = [p for p, (_, present) in status.items() if not present]
    failed = []
    for path in missing:
        args = ["git", "-C", str(root), "submodule", "update", "--init", "--", path]
        if _run(args[:-2] + ["--depth", "1", "--", path], None).returncode:
            if _run(args, None).returncode:
                failed.append(path)
    return failed


def run_osv(root) -> dict:
    proc = _run(["osv-scanner", "scan", "source", "-r", "--format", "json",
                 str(root)], root)
    # 0: none found, 1: vulnerabilities found.
    if proc.returncode not in (0, 1):
        raise CheckerBroken(f"osv-scanner exited with {proc.returncode}:\n"
                            + proc.stderr[-4000:])
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise CheckerBroken(f"osv-scanner: bad JSON ({e}):\n{proc.stdout[:2000]}")


def run_zizmor(root) -> list:
    # Offline: the online audits (known-vulnerable-actions, impostor-commit)
    # need a GitHub token.
    proc = _run(["zizmor", "--offline", "--format", "json", ".github/workflows"], root)
    if not zizmor_ok(proc.returncode):
        raise CheckerBroken(f"zizmor exited with {proc.returncode}:\n"
                            + proc.stderr[-4000:])
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise CheckerBroken(f"zizmor: bad JSON ({e}):\n{proc.stdout[:2000]}")


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="fork_check audit")
    ap.add_argument("--win-repo", required=True)
    ap.add_argument("--commit", required=True)
    return ap.parse_args(argv)


def _run_audit(args: argparse.Namespace) -> int:
    missing = [exe for exe in INSTALL if shutil.which(exe) is None]
    if missing:
        raise CheckerBroken("missing tools:\n" + "\n".join(
            f"  {exe}: {INSTALL[exe]}" for exe in missing))
    root = snapshot.prepare(args.win_repo, args.commit, args.commit)
    status = parse_submodule_status(snapshot.git(root, "submodule", "status"))
    unscanned = fetch_missing_submodules(root, status)
    status = parse_submodule_status(snapshot.git(root, "submodule", "status"))
    sections = submodule_lines((root / ".gitmodules").read_text())

    findings, skipped = parse_osv(run_osv(root), str(root), sections,
                                  {p: sha for p, (sha, _) in status.items()})
    findings += parse_zizmor(run_zizmor(root))

    if unscanned:
        print(f"note: not checked out, so not scanned: {', '.join(unscanned)}",
              file=sys.stderr)
    print(f"note: osv-scanner matches commits by file contents; check each hit "
          f"against the pinned commit. Skipped {skipped} nested package(s) "
          f"(dev lockfiles, nested submodules).", file=sys.stderr)
    print(f"audited {len(status)} submodule(s) and .github/workflows at "
          f"{args.commit[:10]}", file=sys.stderr)
    selected = select(findings, "full", {})
    print(format_report(selected))
    return EXIT_FINDINGS if selected else EXIT_CLEAN


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        with snapshot.exclusive():
            return _run_audit(args)
    except CheckerBroken as e:
        print(f"checker broken: {e}", file=sys.stderr)
        return EXIT_BROKEN
    except Exception:  # A crash must never read as exit 1, "findings".
        traceback.print_exc()
        print("checker broken: unexpected error (traceback above)", file=sys.stderr)
        return EXIT_BROKEN
