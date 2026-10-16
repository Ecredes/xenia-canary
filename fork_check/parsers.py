"""Parses each tool's text output into Findings."""

from __future__ import annotations

import posixpath
import re

from fork_check.findings import CheckerBroken, Finding


def rel(path: str, root: str) -> str | None:
    """Returns `path` relative to `root`, or None if it lies outside it."""
    path = path.replace("\\", "/")
    if path.startswith("/"):
        prefix = root.rstrip("/") + "/"
        if not path.startswith(prefix):
            return None
        path = path[len(prefix):]
    path = posixpath.normpath(path)
    if path.startswith("../") or path == "..":
        return None
    return path


# Template: {file}:{line}:{column}: {severity}:{id}: {message}
_CPPCHECK_RE = re.compile(r"^(.+?):(\d+):(\d+): (\w+):(\w+): (.*)$")
# syntaxError: without include paths cppcheck can't parse macros such as
# Catch2's TEST_CASE, and Codacy (which runs it the same way) reports no
# syntaxError issues, so it drops them too.
_CPPCHECK_IGNORED_IDS = {"missingInclude", "missingIncludeSystem",
                         "checkersReport", "unmatchedSuppression", "syntaxError"}


def parse_cppcheck(text: str, root: str) -> list[Finding]:
    findings = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = _CPPCHECK_RE.match(line)
        if match is None:
            raise CheckerBroken(f"cppcheck: unexpected output: {line}")
        path, line_no, _, severity, rule, message = match.groups()
        if severity == "information" or rule in _CPPCHECK_IGNORED_IDS:
            continue
        path = rel(path, root)
        if path is not None:
            findings.append(Finding(path, int(line_no), "cppcheck", rule, message))
    return findings


# --dataonly --singleline: path:line:  [level] (category) function:message
# (flawfinder 2.0 prints no column; one is accepted in case a version does).
_FLAWFINDER_RE = re.compile(
    r"^(.+?):(\d+):(?:\d+:)?\s+\[(\d)\] \((\w+)\) (\w+):(.*)$")


def parse_flawfinder(text: str, root: str) -> list[Finding]:
    findings = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = _FLAWFINDER_RE.match(line)
        if match is None:
            raise CheckerBroken(f"flawfinder: unexpected output: {line}")
        path, line_no, level, category, function, message = match.groups()
        path = rel(path, root)
        if path is not None:
            findings.append(Finding(path, int(line_no), "flawfinder", function,
                                    f"[{level}] ({category}) {message.strip()}"))
    return findings


_TIDY_RE = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*) \[([\w.,-]+)\]$")


def parse_clang_tidy(text: str, root: str) -> list[Finding]:
    """Keeps check warnings; compile errors mean the database is wrong."""
    findings = []
    for line in text.splitlines():
        if line.startswith("Error while processing"):
            raise CheckerBroken(f"clang-tidy: {line}")
        match = _TIDY_RE.match(line)
        if match is None:
            continue  # Notes, source excerpts, carets and summaries.
        path, line_no, _, kind, message, checks = match.groups()
        names = checks.split(",")
        if kind == "error" and any(n.startswith("clang-diagnostic") for n in names):
            raise CheckerBroken(f"clang-tidy could not compile: {line}")
        path = rel(path, root)
        if path is None:
            continue
        for name in names:
            if name.startswith("clang-diagnostic"):
                continue  # Compiler warnings; the build covers those.
            findings.append(Finding(path, int(line_no), "clang-tidy", name, message))
    return findings


# --output=emacs: path:line:  message  [category] [confidence]
_CPPLINT_RE = re.compile(r"^(.+?):(\d+):\s+(.*?)\s+\[([\w/+-]+)\] \[(\d)\]$")


def parse_cpplint(text: str, root: str) -> list[Finding]:
    findings = []
    for line in text.splitlines():
        if not line.strip() or line.startswith(("Done processing", "Total errors found")):
            continue
        match = _CPPLINT_RE.match(line)
        if match is None:
            raise CheckerBroken(f"cpplint: unexpected output: {line}")
        path, line_no, message, rule, _ = match.groups()
        path = rel(path, root)
        if path is not None:
            findings.append(Finding(path, int(line_no), "cpplint", rule, message))
    return findings
