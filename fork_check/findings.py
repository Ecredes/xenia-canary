"""Findings, the checker-broken error and report formatting."""

from __future__ import annotations

import collections
import dataclasses
from typing import Iterable

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_BROKEN = 2

SCOPES = ("changed", "files", "full")


@dataclasses.dataclass(frozen=True)
class Finding:
    path: str  # Repo-relative, forward slashes.
    line: int
    tool: str
    rule: str
    message: str


class CheckerBroken(Exception):
    """The checker could not produce a trustworthy result (exit code 2)."""


def _in_scope(finding: Finding, scope: str,
              changed: dict[str, set[int]]) -> bool:
    if scope == "full":
        return True
    lines = changed.get(finding.path)
    if lines is None:
        return False
    return scope == "files" or finding.line in lines


def select(findings: Iterable[Finding], scope: str,
           changed: dict[str, set[int]]) -> list[Finding]:
    """Keeps the findings in scope, without duplicates, sorted."""
    if scope not in SCOPES:
        raise ValueError(f"unknown scope {scope!r}")
    kept: dict[tuple, Finding] = {}
    for finding in findings:
        if _in_scope(finding, scope, changed):
            key = (finding.path, finding.line, finding.tool, finding.rule)
            kept.setdefault(key, finding)
    return sorted(kept.values(),
                  key=lambda x: (x.path, x.line, x.tool, x.rule))


def format_report(findings: list[Finding]) -> str:
    if not findings:
        return "-- no findings"
    lines = [f"{x.path}:{x.line}: [{x.tool}:{x.rule}] {x.message}"
             for x in findings]
    counts = collections.Counter(x.tool for x in findings)
    summary = ", ".join(f"{tool}: {n}" for tool, n in sorted(counts.items()))
    lines.append(f"-- {len(findings)} finding(s) ({summary})")
    return "\n".join(lines)
