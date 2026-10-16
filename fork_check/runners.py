"""Runs each analysis tool and parses its output."""

from __future__ import annotations

import concurrent.futures
import os
import pathlib
import shutil
import subprocess
from typing import Iterable

from fork_check import parsers
from fork_check.findings import CheckerBroken, Finding
from fork_check.tidy_sources import (choose_tidy_sources, load_compile_db_files,
                                     sources_not_in_db)

CONFIG_DIR = pathlib.Path(__file__).resolve().parent.parent / "config"

TOOLS = ("cppcheck", "flawfinder", "clang-tidy", "cpplint")
EXECUTABLES = {"cppcheck": "cppcheck", "flawfinder": "flawfinder",
               "clang-tidy": "clang-tidy-20", "cpplint": "cpplint"}
INSTALL = {
    "cppcheck": "sudo apt install cppcheck",
    "flawfinder": "pipx install flawfinder",
    "clang-tidy-20": "sudo apt install clang-tidy-20 (apt.llvm.org, llvm-toolchain-noble-20)",
    "cpplint": "pipx install cpplint",
}
# Only what clang-format and `xb lint` don't already enforce. Calibrated on
# #1299/#1300 (2026-10-16): dropped build/include_what_you_use (14 hits;
# xenia relies on transitive includes throughout) and build/namespaces (8 hits;
# `using namespace` is normal in xenia .cc and test files), and
# runtime/references (Google style allows non-const reference parameters
# since 2020).
CPPLINT_FILTER = ("-,+build/include_order,+readability/todo,+runtime,"
                  "-runtime/references")
CPPCHECK_TEMPLATE = "{file}:{line}:{column}: {severity}:{id}: {message}"
# Codacy reports no memcpy issues although src/xenia has 300+ memcpy calls, so
# its Flawfinder patterns leave memcpy out; mirror that.
FLAWFINDER_IGNORED = {"memcpy"}
_BATCH = 200  # Keeps command lines short in --full runs.


def require(tools: Iterable[str]) -> None:
    missing = [EXECUTABLES[t] for t in tools if shutil.which(EXECUTABLES[t]) is None]
    if missing:
        raise CheckerBroken("missing tools:\n" + "\n".join(
            f"  {exe}: {INSTALL[exe]}" for exe in missing))


def _run(cmd: list[str], cwd: pathlib.Path, ok_codes=(0,)) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
    if proc.returncode not in ok_codes:
        raise CheckerBroken(f"{cmd[0]} exited with {proc.returncode}:\n"
                            + (proc.stdout + proc.stderr)[-4000:])
    return proc


def _batches(files: list[str]):
    for i in range(0, len(files), _BATCH):
        yield files[i:i + _BATCH]


def cppcheck_groups(files: list[str]) -> list[tuple[list[str], list[str]]]:
    """Splits files into (extra args, files) runs: .c as C, the rest as C++.

    cppcheck guesses C for .h files and then reports syntaxError on every
    namespace; Codacy reports no such errors, so it must parse them as C++.
    """
    c_files = [p for p in files if p.endswith(".c")]
    cpp_files = [p for p in files if not p.endswith(".c")]
    groups = [([], c_files), (["--language=c++"], cpp_files)]
    return [(args, group) for args, group in groups if group]


def run_cppcheck(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for extra, group in cppcheck_groups(files):
        for batch in _batches(group):
            proc = _run(["cppcheck", "--quiet", "--enable=warning,style,performance",
                         f"--template={CPPCHECK_TEMPLATE}", f"-j{os.cpu_count() or 1}",
                         *extra, *batch], root)
            findings += parsers.parse_cppcheck(proc.stderr, str(root))
    return findings


def drop_flawfinder_ignored(findings: list[Finding]) -> list[Finding]:
    return [x for x in findings if x.rule not in FLAWFINDER_IGNORED]


def run_flawfinder(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for batch in _batches(files):
        proc = _run(["flawfinder", "--dataonly", "--quiet", "--singleline", *batch], root)
        findings += parsers.parse_flawfinder(proc.stdout, str(root))
    return drop_flawfinder_ignored(findings)


def cpplint_command(batch: list[str]) -> list[str]:
    # cpplint 2.0 skips .inl ("not a valid file name") unless it is listed.
    return ["cpplint", "--quiet", "--output=emacs", "--extensions=c,cc,cpp,cxx,h,hpp,inl",
            f"--filter={CPPLINT_FILTER}", *batch]


def run_cpplint(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for batch in _batches(files):
        # cpplint exits 1 when it reports anything.
        proc = _run(cpplint_command(batch), root, ok_codes=(0, 1))
        findings += parsers.parse_cpplint(proc.stderr, str(root))
    return findings


def run_clang_tidy(root: pathlib.Path, build: pathlib.Path,
                   files: list[str]) -> tuple[list[Finding], list[str]]:
    db_files = load_compile_db_files(str(build / "compile_commands.json"), str(root))
    cache: dict[str, str] = {}

    def read_text(path: str) -> str:
        if path not in cache:
            cache[path] = (root / path).read_text(errors="replace")
        return cache[path]

    sources, no_includer = choose_tidy_sources(files, db_files, read_text)
    notes = [f"note: {h} is not included by any compiled file; clang-tidy skipped it"
             for h in no_includer]
    notes += [f"note: {s} is not in the Linux compile database (platform-specific?); "
              "clang-tidy skipped it" for s in sources_not_in_db(files, db_files)]
    config = str(CONFIG_DIR / "clang-tidy.yaml")

    def tidy(path: str) -> list[Finding]:
        proc = subprocess.run(["clang-tidy-20", "-p", str(build),
                               f"--config-file={config}", "--quiet", path],
                              cwd=root, capture_output=True, text=True, errors="replace")
        out = proc.stdout + proc.stderr
        found = parsers.parse_clang_tidy(out, str(root))  # Raises on compile errors.
        if proc.returncode != 0:
            raise CheckerBroken(f"clang-tidy failed on {path}:\n{out[-4000:]}")
        return found

    findings: list[Finding] = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 1) as pool:
        for result in pool.map(tidy, sources):
            findings += result
    return findings, notes
