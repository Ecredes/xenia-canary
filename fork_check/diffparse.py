"""Turns `git diff -U0` output into the line numbers each file adds."""

from __future__ import annotations

import re

_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
_ESCAPE_RE = re.compile(rb'\\([0-7]{3}|.)')
_ESCAPES = {b"a": b"\a", b"b": b"\b", b"t": b"\t", b"n": b"\n", b"v": b"\v",
            b"f": b"\f", b"r": b"\r"}


def _unquote(path: str) -> str:
    """Undoes git's C-style quoting ("b/a\\"b.cc", octal UTF-8 bytes)."""
    def unescape(match: re.Match) -> bytes:
        code = match.group(1)
        if len(code) == 3:
            return bytes([int(code, 8)])
        return _ESCAPES.get(code, code)  # `\"` and `\\` stand for themselves.
    raw = _ESCAPE_RE.sub(unescape, path[1:-1].encode("utf-8"))
    return raw.decode("utf-8", errors="replace")


def parse_changed_lines(diff_text: str) -> dict[str, set[int]]:
    """Maps each file with new content to its added or modified lines.

    Deleted files, pure renames and binary files have no `+++` side and are
    left out. A file whose hunks only delete lines maps to an empty set.
    """
    result: dict[str, set[int]] = {}
    current: str | None = None
    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            current = None
        elif line.startswith("+++ "):
            # git ends a path containing a space with a tab.
            target = line[4:].rstrip("\t")
            if target.startswith('"') and target.endswith('"'):
                target = _unquote(target)
            if target == "/dev/null":
                current = None
            else:
                current = target[2:] if target.startswith("b/") else target
                result.setdefault(current, set())
        elif line.startswith("@@") and current is not None:
            match = _HUNK_RE.match(line)
            if match is None:
                continue
            start = int(match.group(1))
            count = int(match.group(2)) if match.group(2) is not None else 1
            result[current].update(range(start, start + count))
    return result
