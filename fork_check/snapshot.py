"""The WSL scratch clone the tools run against."""

from __future__ import annotations

import glob
import hashlib
import os
import pathlib
import subprocess

from fork_check.findings import CheckerBroken

SCRATCH = pathlib.Path.home() / "src" / "xenia-fork-check"
REFERENCE_CLONE = pathlib.Path.home() / "src" / "xenia-canary"
KEY_FILE = ".fork-check-key"
SOURCE_GLOBS = ("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hpp", "*.inl")


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


def prepare(win_repo: str, commit: str, base: str) -> pathlib.Path:
    root = SCRATCH
    if not (root / ".git").exists():
        root.parent.mkdir(parents=True, exist_ok=True)
        proc = _run(["git", "clone", "--no-checkout", win_repo, str(root)])
        if proc.returncode != 0:
            raise CheckerBroken(f"cloning {win_repo} failed:\n{proc.stderr.strip()}")
    # Only branch heads: the Windows repo is a partial clone, so upstream refs
    # can point at objects it doesn't have. The base is the merge-base, an
    # ancestor of the branch, so it arrives with the heads.
    git(root, "fetch", "--no-tags", "--force", win_repo,
        "+refs/heads/*:refs/remotes/win/*")
    for sha in (commit, base):
        if _run(["git", "-C", str(root), "cat-file", "-e", f"{sha}^{{commit}}"]).returncode:
            raise CheckerBroken(f"commit {sha} not found after fetching {win_repo}")
    git(root, "checkout", "--force", "--detach", commit)
    # Keep the build dir and generated shader headers between runs.
    git(root, "clean", "-ffdxq", "-e", "build/", "-e", "src/xenia/gpu/shaders/bytecode/")
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


def _current_key(root: pathlib.Path) -> str:
    # Sources are part of the key because CMake collects them with file(GLOB).
    return cache_key(
        git(root, "ls-files", "-s", "--", "*CMakeLists.txt", "*.cmake"),
        git(root, "ls-files", "--", *SOURCE_GLOBS),
        git(root, "submodule", "status", "--recursive"))


def _tool_env() -> dict:
    env = dict(os.environ)
    if not env.get("VULKAN_SDK"):
        sdks = sorted(glob.glob(str(pathlib.Path.home() / "vulkan-sdk" / "*" / "x86_64")))
        if sdks:
            env["VULKAN_SDK"] = sdks[-1]
            env["PATH"] = f"{sdks[-1]}/bin:{env.get('PATH', '')}"
    return env


def configure(root: pathlib.Path) -> pathlib.Path:
    build = root / "build"
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
    targets = _run(["ninja", "-C", str(build), "-t", "targets", "all"], env=env)
    names = shader_targets(targets.stdout)
    if names:
        proc = _run(["ninja", "-C", str(build), *names], env=env)
        if proc.returncode != 0:
            raise CheckerBroken("generating shader headers failed:\n"
                                + (proc.stdout + proc.stderr)[-4000:])
    if stale:
        key_path.write_text(key + "\n")
    return build
