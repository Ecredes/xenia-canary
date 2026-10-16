"""Chooses which translation units clang-tidy runs on."""

from __future__ import annotations

import json
import posixpath
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


def _include_string(header: str) -> str:
    return header[len("src/"):] if header.startswith("src/") else header


def choose_tidy_sources(touched: list[str], db_files: list[str],
                        read_text: Callable[[str], str]
                        ) -> tuple[list[str], list[str]]:
    """Returns (sources to run, touched headers no compiled file includes).

    xenia includes headers by their path under src/, e.g. "xenia/base/a.h".
    A touched header is analyzed through a touched source that includes it,
    or else through the first compiled file that does.
    """
    db_set = set(db_files)
    sources = [p for p in touched if p in db_set]
    no_includer: list[str] = []
    for header in (p for p in touched if p.endswith(HEADER_EXTS)):
        needle = f'"{_include_string(header)}"'
        if any(needle in read_text(s) for s in sources):
            continue
        includer = next((s for s in db_files if needle in read_text(s)), None)
        if includer is None:
            no_includer.append(header)
        elif includer not in sources:
            sources.append(includer)
    return sources, no_includer
