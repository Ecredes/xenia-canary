"""Chooses which translation units clang-tidy runs on."""

from __future__ import annotations

import json
import posixpath
import re
from typing import Callable

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


def _includes(source: str, text: str, header: str) -> bool:
    """Whether `source` includes `header` with a quoted include.

    xenia includes headers by their path under src/ ("xenia/base/a.h") or
    relative to the including file ("pm4_impl.h", "../pm4_impl.h").
    """
    if posixpath.basename(header) not in text:
        return False  # Cheap pre-filter; most files don't mention it.
    source_dir = posixpath.dirname(source)
    for name in _INCLUDE_RE.findall(text):
        if (posixpath.normpath(posixpath.join("src", name)) == header
                or posixpath.normpath(posixpath.join(source_dir, name)) == header):
            return True
    return False


def choose_tidy_sources(touched: list[str], db_files: list[str],
                        read_text: Callable[[str], str]
                        ) -> tuple[list[str], list[str]]:
    """Returns (sources to run, touched headers no compiled file includes).

    A touched header is analyzed through a touched source that includes it,
    or else through the first compiled file that does.
    """
    db_set = set(db_files)
    sources = [p for p in touched if p in db_set]
    no_includer: list[str] = []
    for header in (p for p in touched if p.endswith(HEADER_EXTS)):
        if any(_includes(s, read_text(s), header) for s in sources):
            continue
        includer = next((s for s in db_files
                         if _includes(s, read_text(s), header)), None)
        if includer is None:
            no_includer.append(header)
        elif includer not in sources:
            sources.append(includer)
    return sources, no_includer
