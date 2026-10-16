"""Chooses which translation units clang-tidy runs on."""

from __future__ import annotations

import json
import posixpath
import re
from typing import Callable, Iterable

from fork_check.parsers import rel

HEADER_EXTS = (".h", ".hpp", ".inl")


def load_compile_db_files(db_path: str, root: str) -> list[str]:
    with open(db_path, encoding="utf-8") as f:
        entries = json.load(f)
    files: list[str] = []
    seen = set()
    for entry in entries:
        path = entry["file"]
        if not path.startswith("/"):
            path = posixpath.join(entry["directory"], path)
        path = rel(path, root)
        if path is not None and path not in seen:
            seen.add(path)
            files.append(path)
    return files


_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def _resolved_includes(source: str, text: str) -> set[str]:
    """Repo paths a quoted include in `source` may refer to.

    xenia includes headers by their path under src/ ("xenia/base/a.h") or
    relative to the including file ("pm4_impl.h", "../pm4_impl.h"); both
    readings are kept.
    """
    source_dir = posixpath.dirname(source)
    paths = set()
    for name in _INCLUDE_RE.findall(text):
        paths.add(posixpath.normpath(posixpath.join("src", name)))
        paths.add(posixpath.normpath(posixpath.join(source_dir, name)))
    return paths


def sources_not_in_db(touched: list[str], db_files: list[str]) -> list[str]:
    """Touched non-header files the Linux build doesn't compile (d3d12, win32)."""
    db_set = set(db_files)
    return [p for p in touched if not p.endswith(HEADER_EXTS) and p not in db_set]


def choose_tidy_sources(touched: list[str], db_files: list[str],
                        read_text: Callable[[str], str],
                        headers: Iterable[str] = ()
                        ) -> tuple[list[str], list[str]]:
    """Returns (sources to run, touched headers no compiled file includes).

    A touched header is analyzed through a touched source that includes it,
    directly or through the tracked `headers`; else through the compiled
    file nearest to it in the include chain (first in `db_files` order).
    """
    includes_cache: dict[str, set[str]] = {}

    def includes(path: str) -> set[str]:
        if path not in includes_cache:
            includes_cache[path] = _resolved_includes(path, read_text(path))
        return includes_cache[path]

    included_by: dict[str, list[str]] = {}
    for h in headers:
        for target in includes(h):
            included_by.setdefault(target, []).append(h)

    db_set = set(db_files)
    sources = [p for p in touched if p in db_set]
    no_includer: list[str] = []
    for header in (p for p in touched if p.endswith(HEADER_EXTS)):
        # Breadth-first up the include graph: levels[n] is n headers away.
        levels = [[header]]
        seen = {header}
        while levels[-1]:
            nxt = [h for target in levels[-1] for h in included_by.get(target, ())
                   if h not in seen]
            seen.update(nxt)
            levels.append(list(dict.fromkeys(nxt)))
        if any(includes(s) & seen for s in sources):
            continue
        includer = next((s for level in levels for s in db_files
                         if includes(s) & set(level)), None)
        if includer is None:
            no_includer.append(header)
        elif includer not in sources:
            sources.append(includer)
    return sources, no_includer
