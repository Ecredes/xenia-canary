"""Upstream's Codacy exclusions and which files the tools analyze."""

from __future__ import annotations

import re

SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl")

_ITEM_RE = re.compile(r"""^\s*-\s*['"]?([^'"]+?)['"]?\s*$""")


def load_codacy_excludes(yaml_text: str) -> list[str]:
    """Reads the `exclude_paths` list from a `.codacy.yaml`.

    Only the flat list form upstream uses is supported, so no YAML library
    is needed.
    """
    patterns: list[str] = []
    in_list = False
    for line in yaml_text.splitlines():
        if line.strip().startswith("exclude_paths:"):
            in_list = True
            continue
        if in_list:
            match = _ITEM_RE.match(line)
            if match:
                patterns.append(match.group(1))
            elif line.strip() and not line.startswith((" ", "\t")):
                in_list = False
    return patterns


def _glob_to_regex(pattern: str) -> re.Pattern:
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def is_excluded(path: str, patterns: list[str]) -> bool:
    return any(_glob_to_regex(p).match(path) for p in patterns)


def analyzable(path: str, patterns: list[str]) -> bool:
    return path.endswith(SOURCE_EXTS) and not is_excluded(path, patterns)
