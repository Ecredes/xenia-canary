"""The WSL scratch clone the tools run against."""

from __future__ import annotations

import contextlib
import fcntl
import glob
import hashlib
import os
import pathlib
import subprocess
import sys
from collections.abc import Iterator

from fork_check.findings import CheckerBroken

SCRATCH = pathlib.Path.home() / "src" / "xenia-fork-check"
# Next to the clone, not in it: `git clean` would delete it.
LOCK_FILE = SCRATCH.parent / "xenia-fork-check.lock"
REFERENCE_CLONE = pathlib.Path.home() / "src" / "xenia-canary"
KEY_FILE = ".fork-check-key"
SOURCE_GLOBS = ("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hpp", "*.inl")
# Shader sources: XeniaHelpers.cmake collects them with file(GLOB) as well.
SHADER_GLOBS = ("*.xesl", "*.xesli", "*.glsl", "*.glsli", "*.hlsl", "*.hlsli")


def _run(cmd: list[str], cwd: pathlib.Path | None = None,
         env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True,
                          text=True, errors="replace")


def git(root: pathlib.Path, *args: str, check: bool = True) -> str:
    proc = _run(["git", "-C", str(root), *args])
    if check and proc.returncode != 0:
        raise CheckerBroken(f"git {' '.join(args)} failed:\n{proc.stderr.strip()}")
    return proc.stdout


def _submodule_paths() -> list[str]:
    """Submodules initialized in the normal WSL clone, which builds fine."""
    if not (REFERENCE_CLONE / ".git").exists():
        return []  # Empty means all.
    status = git(REFERENCE_CLONE, "submodule", "status")
    return [line.split()[1] for line in status.splitlines()
            if line and not line.startswith("-")]


def clean(root: pathlib.Path) -> None:
    """Removes untracked files but keeps the build and generated headers.

    Every `shaders/bytecode/` dir (gpu and ui) must survive: ninja's stamps in
    build/ say they are up to date, so deleted headers would never come back.
    """
    git(root, "clean", "-ffdxq", "-e", "/build/", "-e", "**/shaders/bytecode/")


@contextlib.contextmanager
def exclusive(lock_file: pathlib.Path | None = None) -> Iterator[None]:
    """Holds the scratch clone for a whole run.

    Every run checks out and cleans the one clone, so two concurrent runs
    would analyze each other's files. The lock goes with the process, even
    when it is killed.
    """
    lock_file = lock_file or LOCK_FILE
    lock_file.parent.mkdir(parents=True, exist_ok=True)
    with open(lock_file, "w") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("waiting for another fork check to finish with "
                  f"{SCRATCH}", file=sys.stderr, flush=True)
            fcntl.flock(f, fcntl.LOCK_EX)
        yield


def prepare(win_repo: str, commit: str, base: str) -> pathlib.Path:
    root = SCRATCH
    if not (root / ".git").exists():
        root.parent.mkdir(parents=True, exist_ok=True)
        proc = _run(["git", "clone", "--no-checkout", win_repo, str(root)])
        if proc.returncode != 0:
            raise CheckerBroken(f"cloning {win_repo} failed:\n{proc.stderr.strip()}")
    # Only branch heads and HEAD (which may be detached): the Windows repo is
    # a partial clone, so upstream refs can point at objects it doesn't have.
    # The base is the merge-base, an ancestor of HEAD, so it arrives too.
    git(root, "fetch", "--no-tags", "--force", win_repo,
        "+refs/heads/*:refs/remotes/win/*", "+HEAD:refs/remotes/win-HEAD")
    for sha, what in ((commit, "HEAD"), (base, "base")):
        if _run(["git", "-C", str(root), "cat-file", "-e", f"{sha}^{{commit}}"]).returncode:
            hint = ("; -Base must be an ancestor of HEAD or of a local branch"
                    if what == "base" else "")
            raise CheckerBroken(f"{what} commit {sha} not found after fetching "
                                f"{win_repo}{hint}")
    git(root, "checkout", "--force", "--detach", commit)
    clean(root)
    git(root, "submodule", "sync", "--recursive")
    paths = _submodule_paths()
    args = ["-c", "protocol.file.allow=always", "submodule", "update", "--init",
            "--recursive", "--jobs", "8"]
    # Shallow first; some pinned commits can't be fetched at depth 1.
    if _run(["git", "-C", str(root), *args, "--depth", "1", "--", *paths]).returncode:
        git(root, *args, "--", *paths)
    return root


def cache_key(cmake_files_listing: str, source_listing: str, submodules: str) -> str:
    digest = hashlib.sha256()
    for part in (cmake_files_listing, source_listing, submodules):
        data = part.encode()
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()


def shader_targets(ninja_targets_output: str) -> list[str]:
    """Bare `*-shaders` target names from `ninja -t targets all`."""
    names = (line.split(":")[0] for line in ninja_targets_output.splitlines())
    return sorted({n for n in names if n.endswith("-shaders") and "/" not in n})


def list_shader_targets(build: pathlib.Path, env: dict) -> list[str]:
    proc = _run(["ninja", "-C", str(build), "-t", "targets", "all"], env=env)
    if proc.returncode != 0:
        raise CheckerBroken("listing ninja targets failed:\n"
                            + (proc.stdout + proc.stderr)[-4000:])
    return shader_targets(proc.stdout)


def _current_key(root: pathlib.Path) -> str:
    # Sources and shaders are part of the key because CMake collects them
    # with file(GLOB).
    return cache_key(
        git(root, "ls-files", "-s", "--", "*CMakeLists.txt", "*.cmake"),
        git(root, "ls-files", "--", *SOURCE_GLOBS, *SHADER_GLOBS),
        git(root, "submodule", "status", "--recursive"))


def _tool_env() -> dict:
    env = dict(os.environ)
    if not env.get("VULKAN_SDK"):
        sdks = sorted(glob.glob(str(pathlib.Path.home() / "vulkan-sdk" / "*" / "x86_64")))
        if sdks:
            env["VULKAN_SDK"] = sdks[-1]
            env["PATH"] = f"{sdks[-1]}/bin:{env.get('PATH', '')}"
    return env


_VERSION_H = """\
// Placeholder written by the fork check; `xb` normally generates this file.
#ifndef GENERATED_VERSION_H_
#define GENERATED_VERSION_H_
#define XE_BUILD_BRANCH "fork-check"
#define XE_BUILD_COMMIT "fork-check"
#define XE_BUILD_COMMIT_SHORT "fork-check"
#define XE_BUILD_DATE __DATE__
#endif  // GENERATED_VERSION_H_
"""


def ensure_version_h(build: pathlib.Path) -> None:
    """Writes build/version.h if missing (xenia-build.py makes it, not CMake)."""
    path = build / "version.h"
    if not path.exists():
        build.mkdir(parents=True, exist_ok=True)
        path.write_text(_VERSION_H)


def configure(root: pathlib.Path) -> pathlib.Path:
    build = root / "build"
    ensure_version_h(build)
    key_path = build / KEY_FILE
    key = _current_key(root)
    db = build / "compile_commands.json"
    env = _tool_env()
    stale = not (db.exists() and key_path.exists()
                 and key_path.read_text().strip() == key)
    if stale:
        proc = _run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Debug",
                     "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                     "-DXENIA_BUILD_TESTS=ON",
                     "-DCMAKE_C_COMPILER=clang-20",
                     "-DCMAKE_CXX_COMPILER=clang++-20"], env=env)
        if proc.returncode != 0 or not db.exists():
            raise CheckerBroken("cmake configure failed:\n"
                                + (proc.stdout + proc.stderr)[-4000:])
    names = list_shader_targets(build, env)
    if names:
        proc = _run(["ninja", "-C", str(build), *names], env=env)
        if proc.returncode != 0:
            raise CheckerBroken("generating shader headers failed:\n"
                                + (proc.stdout + proc.stderr)[-4000:])
    if stale:
        key_path.write_text(key + "\n")
    return build
