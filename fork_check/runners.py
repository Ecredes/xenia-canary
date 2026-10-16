"""Runs each analysis tool and parses its output."""

from __future__ import annotations

import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import threading
from typing import Callable, Iterable, TypeVar

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
T = TypeVar("T")


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

    headers = [p for p in _run(["git", "ls-files", "--", "*.h", "*.hpp", "*.inl"],
                               root).stdout.splitlines() if p.startswith("src/")]
    sources, no_includer = choose_tidy_sources(files, db_files, read_text, headers)
    notes = [f"note: {h} is not included by any compiled file, even through other "
             "headers; clang-tidy skipped it" for h in no_includer]
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

    jobs = tidy_jobs(os.cpu_count() or 1, _mem_available_kb())
    return run_parallel(tidy, sources, jobs), notes


# A heavy xenia translation unit peaks near 1 GB in clang-tidy (measured on
# command_processor.cc, 2026-10-16); WSL gets half the host's RAM by default.
_TIDY_JOB_KB = 1536 * 1024


def tidy_jobs(cpus: int, mem_available_kb: int | None) -> int:
    if mem_available_kb is None:
        return max(1, cpus)
    return max(1, min(cpus, mem_available_kb // _TIDY_JOB_KB))


def _mem_available_kb() -> int | None:
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def run_parallel(fn: Callable[[T], list], items: list[T], jobs: int) -> list:
    """Runs `fn` on each item; the first failure stops what hasn't started."""
    failed = threading.Event()

    def guarded(item: T) -> list:
        if failed.is_set():
            return []
        try:
            return fn(item)
        except BaseException:
            failed.set()
            raise

    results: list = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(guarded, item) for item in items]
        try:
            for future in futures:
                results += future.result()
        except BaseException:
            pool.shutdown(cancel_futures=True)
            raise
    return results
