# Fork Check Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A local command that runs cppcheck, Flawfinder, clang-tidy and cpplint on the lines a xenia-canary topic branch changes, in WSL, and reports findings in one list.

**Architecture:** `check.ps1` (Windows) resolves the commit, base and dirty state with Windows git and calls `python3 -m fork_check` in the `Ubuntu-24.04` WSL distro. The Python package keeps a dedicated scratch clone (`~/src/xenia-fork-check`), checks the commit out there, configures CMake for a compile database, runs the tools, parses their output into `Finding`s, filters them to the changed lines and prints a report. Pure logic (diff parsing, exclusions, output parsers, filtering) is unit-tested; process-running code is verified end to end.

**Tech Stack:** Python 3.12 stdlib only (WSL); `unittest`; PowerShell 7; clang-tidy-20, cppcheck 2.13 (apt), cpplint and flawfinder (pipx); CMake 3.28 + Ninja + clang-20 in WSL.

**Spec:** `docs/superpowers/specs/2026-10-15-fork-check-design.md` (same branch).

## Global Constraints

- Everything lives on the orphan branch `tooling`, worktree `C:\Users\Adam\AGENTS\xenia-canary-tooling`. Nothing is added to xenia-canary topic branches.
- Runs in WSL distro `Ubuntu-24.04` with clang-20 (matches upstream CI). Never touches `~/src/xenia-canary`; the scratch clone is `~/src/xenia-fork-check`.
- Exit codes: 0 = clean, 1 = findings, 2 = checker broken. A missing tool, configure failure, tool crash or unparseable output is exit 2, never a clean pass.
- Report line format: `path:line: [tool:rule] message`, paths repo-relative with `/`, sorted by path then line, followed by a per-tool count line.
- Paths excluded by upstream's `.codacy.yaml` are excluded for every tool. Analyzed extensions: `.c .cc .cpp .cxx .h .hpp .inl`.
- Default base: `merge-base(upstream/canary_experimental, HEAD)`. Only committed state is analyzed; a dirty working tree gives a warning.
- Python: stdlib only, `from __future__ import annotations`, no `match` statements (Windows has Python 3.9; tests should also run there).
- Commits: subject starts with `[Tools]`, LF-only messages, no Claude attribution lines (the repo's hooks reject them). On Windows, commit with `printf '...' | git commit -F -` from Git Bash.
- All tracked files use LF line endings (`.gitattributes`), because WSL runs them.

## Review Focus

1. **A branch that adds a new `.cc` file.** CMake collects sources with `file(GLOB)`, so the compile database must be regenerated. Expect the configure cache key to include the list of tracked source files (Task 5 test `test_cache_key_changes_when_source_added`).
2. **A branch that only changes a header.** clang-tidy only runs on `.cc` files, so the header must be analyzed through a `.cc` that includes it. Expect findings on the header's changed lines (Task 6 test `test_header_without_touched_source_uses_includer`).
3. **A branch that is behind upstream, or whose upstream moved on.** Expect only the branch's own lines reported: the diff is `base..commit` with base = merge-base, never `upstream..commit` (Task 7 wrapper computes the merge-base; Task 8 Step 1's second run with `-Base HEAD~1` pins that only lines after the base are reported).
4. **Binary files, pure renames and deletions in the diff.** Expect no crash and no changed lines for them (Task 2 tests).
5. **clang-tidy findings in system or third-party headers, or absolute paths outside the scratch root.** Expect them dropped, not reported and not treated as a crash (Task 3 tests `test_rel_outside_root_is_none`, `test_clang_tidy_drops_outside_root`).

---

## File Structure

```
xenia-canary-tooling/
  .gitattributes                 # LF everywhere
  README.md                      # usage, install, exit codes, calibration notes
  check.ps1                      # Windows entry point -> WSL
  config/clang-tidy.yaml         # clang-tidy checks + Google naming options
  fork_check/__init__.py
  fork_check/__main__.py         # python3 -m fork_check -> cli.main()
  fork_check/findings.py         # Finding, CheckerBroken, scope filtering, report formatting
  fork_check/diffparse.py        # git diff -U0 text -> {path: set(changed lines)}
  fork_check/excludes.py         # .codacy.yaml exclusions, analyzable() predicate
  fork_check/parsers.py          # tool output text -> list[Finding], one function per tool
  fork_check/snapshot.py         # scratch clone, checkout, submodules, configure cache, shader headers
  fork_check/tidy_sources.py     # pick .cc files for clang-tidy (incl. includers of touched headers)
  fork_check/runners.py          # run each tool, require() installed tools
  fork_check/cli.py              # argument parsing and orchestration
  tests/data/*.txt               # real tool output captured in Task 1
  tests/test_findings.py
  tests/test_diffparse.py
  tests/test_excludes.py
  tests/test_parsers.py
  tests/test_snapshot.py
  tests/test_tidy_sources.py
  tests/test_cli.py
  docs/superpowers/specs/...     # existing
  docs/superpowers/plans/...     # this file
```

Run unit tests from the worktree root: `wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling && python3 -m unittest discover -s tests -v"`. Below this is abbreviated to **`UT`** with an optional test filter, e.g. `UT tests.test_findings`.

---

### Task 1: Environment, scaffold and real tool output samples

Installs the tools, proves the WSL scratch-clone and configure approach works, and captures real output from each tool for the parser tests in Task 3. No product code yet beyond the scaffold.

**Files:**
- Create: `.gitattributes`, `README.md`, `fork_check/__init__.py`, `fork_check/__main__.py`, `tests/__init__.py`
- Create: `tests/data/planted.cc`, `tests/data/cppcheck.txt`, `tests/data/flawfinder.txt`, `tests/data/clang_tidy.txt`, `tests/data/cpplint.txt`

**Interfaces:**
- Produces: the four `tests/data/*.txt` samples (Task 3 parses them); the verified scratch-clone commands (Task 5 encodes them).

- [ ] **Step 1: Write the scaffold**

`.gitattributes`:
```
* text=auto eol=lf
*.ps1 text eol=lf
```

`fork_check/__init__.py`:
```python
"""Local pre-flight analysis for upstream-bound xenia-canary branches."""
```

`fork_check/__main__.py`:
```python
import sys

from fork_check.cli import main

sys.exit(main())
```

`tests/__init__.py`: empty file.

`README.md`:
```markdown
# fork check

Local pre-flight analysis for xenia-canary topic branches before they go
upstream. Fork-only; see `docs/superpowers/specs/2026-10-15-fork-check-design.md`.

Usage, install and calibration notes are filled in by the implementation plan.
```

- [ ] **Step 2: Install the tools in WSL**

```bash
wsl -d Ubuntu-24.04 -u root -- bash -c "wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc && echo 'deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main' > /etc/apt/sources.list.d/llvm-20.list && apt-get update -q && apt-get install -y -q clang-tidy-20 pipx"
wsl -d Ubuntu-24.04 -- bash -lc "pipx install cpplint && pipx install flawfinder && pipx ensurepath"
wsl -d Ubuntu-24.04 -- bash -lc "clang-tidy-20 --version; cppcheck --version; cpplint --version; flawfinder --version"
```
Expected: four version banners. If `clang-tidy-20` is already provided by an existing apt.llvm.org source, skip adding the list file (check `/etc/apt/sources.list.d/` first; do not add a duplicate source).

- [ ] **Step 3: Verify the WSL login environment**

```bash
wsl -d Ubuntu-24.04 -- bash -lc 'echo "CC=$CC CXX=$CXX VULKAN_SDK=$VULKAN_SDK"; ls -d ~/vulkan-sdk/*/x86_64'
```
Record the result in the README's "Environment" section (Task 8 writes it). If `VULKAN_SDK` is empty in `bash -lc`, Task 5's `configure()` sets it from the newest `~/vulkan-sdk/*/x86_64`; that code is already in Task 5.

- [ ] **Step 4: Prove the scratch clone works**

```bash
wsl -d Ubuntu-24.04 -- bash -lc '
set -e
W=/mnt/c/Users/Adam/AGENTS/xenia-canary
S=~/src/xenia-fork-check
git clone --no-checkout "$W" "$S"
cd "$S"
git fetch --no-tags "$W" "+refs/heads/*:refs/remotes/win/*" "+refs/remotes/upstream/*:refs/remotes/upstream/*"
git checkout --force --detach win/gpu/release-trace-writer
git -C ~/src/xenia-canary submodule status | grep -v "^-" | awk "{print \$2}" > /tmp/subs.txt
cat /tmp/subs.txt | xargs git -c protocol.file.allow=always submodule update --init --recursive --depth 1 --jobs 8 --
'
```
Expected: clone and submodule update finish. If the clone fails with "detected dubious ownership", run `wsl -d Ubuntu-24.04 -- git config --global --add safe.directory /mnt/c/Users/Adam/AGENTS/xenia-canary` and retry, and record that one-time step in the README install section (Task 8). If a submodule fails at `--depth 1` with "unadvertised object", drop `--depth 1` for that path and note it (Task 5 Step 3 then uses the same fallback).

- [ ] **Step 5: Prove a configure-only compile database works**

```bash
wsl -d Ubuntu-24.04 -- bash -lc '
cd ~/src/xenia-fork-check
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DXENIA_BUILD_TESTS=ON -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 > /tmp/cfg.log 2>&1; echo "configure exit $?"
python3 -c "import json;d=json.load(open(\"build/compile_commands.json\"));print(len(d),\"entries\")"
ninja -C build -t targets all | grep -E "^[^:]*-shaders:" | cut -d: -f1 | tee /tmp/shader_targets.txt
xargs ninja -C build < /tmp/shader_targets.txt > /tmp/shaders.log 2>&1; echo "shaders exit $?"
'
```
Expected: `configure exit 0`, several hundred entries or more, at least one `*-shaders` target, `shaders exit 0`. If configure fails, read `/tmp/cfg.log` and fix the cause before continuing (e.g. missing `VULKAN_SDK`); Task 5 must encode whatever was needed.

- [ ] **Step 6: Write the planted sample file**

`tests/data/planted.cc` (sample input only, never compiled into xenia):
```cpp
#include <cstring>
#include <vector>
#include <string>

namespace Bad_Namespace {

// TODO: unattributed todo for cpplint and clang-tidy.
class lower_class {
 public:
  int Value() { return value; }

 private:
  int value;
};

int NullDeref() {
  int* p = nullptr;
  return *p;
}

void Copy(char* dst, const char* src) { strcpy(dst, src); }

void Swap(int a, int b) {
  if (a = b) {
    return;
  }
}

}  // namespace Bad_Namespace
```

- [ ] **Step 7: Capture real output from each tool**

Copy the file into the scratch clone so clang-tidy has a compile command (reuse the flags of an existing base file), then capture each tool's raw output.

```bash
wsl -d Ubuntu-24.04 -- bash -lc '
set -e
T=/mnt/c/Users/Adam/AGENTS/xenia-canary-tooling
cd ~/src/xenia-fork-check
cp $T/tests/data/planted.cc src/xenia/base/planted.cc
cppcheck --quiet --enable=warning,style,performance "--template={file}:{line}:{column}: {severity}:{id}: {message}" src/xenia/base/planted.cc 2> $T/tests/data/cppcheck.txt || true
flawfinder --dataonly --quiet --singleline src/xenia/base/planted.cc > $T/tests/data/flawfinder.txt || true
cpplint --quiet --output=emacs --filter=-,+build/include_order,+build/include_what_you_use,+readability/todo,+runtime,+build/namespaces src/xenia/base/planted.cc 2> $T/tests/data/cpplint.txt || true
python3 - <<EOF
import json, os
d = json.load(open("build/compile_commands.json"))
e = dict(next(e for e in d if e["file"].endswith("src/xenia/base/string.cc")))
for k in ("command", "file"):
    if k in e:
        e[k] = e[k].replace("string.cc", "planted.cc")
if "arguments" in e:
    e["arguments"] = [a.replace("string.cc", "planted.cc") for a in e["arguments"]]
e.pop("output", None)
os.makedirs("/tmp/planted_db", exist_ok=True)
json.dump([e], open("/tmp/planted_db/compile_commands.json", "w"))
EOF
clang-tidy-20 -p /tmp/planted_db --quiet "--checks=-*,bugprone-*,clang-analyzer-*,google-readability-todo,readability-identifier-naming" --config="{CheckOptions: {readability-identifier-naming.ClassCase: CamelCase, readability-identifier-naming.NamespaceCase: lower_case, readability-identifier-naming.PrivateMemberSuffix: _}}" src/xenia/base/planted.cc > $T/tests/data/clang_tidy.txt 2>&1 || true
rm src/xenia/base/planted.cc
'
```
If `string.cc` isn't in `src/xenia/base`, pick any `src/xenia/base/*.cc` entry from the database. Expected: each `.txt` file is non-empty. Open each and confirm it contains the expected hits:
- `cppcheck.txt`: a `nullPointer` line and a `knownConditionTrueFalse` or `assignmentInCondition`-style line.
- `flawfinder.txt`: a `strcpy` line.
- `cpplint.txt`: a `readability/todo` line.
- `clang_tidy.txt`: `[clang-analyzer-core.NullDereference]`, a `readability-identifier-naming` warning, `[google-readability-todo]`, and `bugprone-assignment-in-if-condition`.

The captured paths are relative (`src/xenia/base/planted.cc`) for cppcheck, flawfinder and cpplint and absolute (`/home/<user>/src/xenia-fork-check/src/xenia/base/planted.cc`) for clang-tidy. Leave them as captured; Task 3 tests use root `/home/<user>/src/xenia-fork-check` read from the sample itself.

- [ ] **Step 8: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add -A
printf '[Tools] Scaffold fork check and capture tool output samples\n' | git commit -q -F -
```

---

### Task 2: Findings model and diff parsing

**Files:**
- Create: `fork_check/findings.py`, `fork_check/diffparse.py`
- Test: `tests/test_findings.py`, `tests/test_diffparse.py`

**Interfaces:**
- Produces:
  - `findings.Finding(path: str, line: int, tool: str, rule: str, message: str)` — frozen dataclass.
  - `findings.CheckerBroken(Exception)`.
  - `findings.EXIT_CLEAN = 0`, `EXIT_FINDINGS = 1`, `EXIT_BROKEN = 2`.
  - `findings.select(findings: Iterable[Finding], scope: str, changed: dict[str, set[int]]) -> list[Finding]` — scope is `"changed"`, `"files"` or `"full"`; dedupes on `(path, line, tool, rule)`; sorted by `(path, line, tool, rule)`.
  - `findings.format_report(findings: list[Finding]) -> str`.
  - `diffparse.parse_changed_lines(diff_text: str) -> dict[str, set[int]]` — keys are new-side repo-relative paths of every file with a `+++` side; values are line numbers added or modified.

- [ ] **Step 1: Write the failing tests**

`tests/test_findings.py`:
```python
import unittest

from fork_check.findings import Finding, format_report, select


def f(path="a.cc", line=1, tool="cppcheck", rule="r", message="m"):
    return Finding(path, line, tool, rule, message)


class SelectTest(unittest.TestCase):
    def test_changed_scope_keeps_only_changed_lines(self):
        changed = {"a.cc": {2, 3}}
        out = select([f(line=1), f(line=2), f(path="b.cc", line=2)], "changed", changed)
        self.assertEqual(out, [f(line=2)])

    def test_files_scope_keeps_all_lines_of_touched_files(self):
        changed = {"a.cc": {2}}
        out = select([f(line=1), f(line=9), f(path="b.cc")], "files", changed)
        self.assertEqual([x.line for x in out], [1, 9])

    def test_files_scope_includes_touched_file_with_no_added_lines(self):
        out = select([f(line=5)], "files", {"a.cc": set()})
        self.assertEqual(len(out), 1)

    def test_full_scope_keeps_everything(self):
        out = select([f(path="z.cc"), f(path="b.cc")], "full", {})
        self.assertEqual([x.path for x in out], ["b.cc", "z.cc"])

    def test_dedupes_same_path_line_tool_rule(self):
        out = select([f(message="x"), f(message="y")], "full", {})
        self.assertEqual(len(out), 1)

    def test_same_line_different_rule_kept(self):
        out = select([f(rule="a"), f(rule="b")], "full", {})
        self.assertEqual(len(out), 2)

    def test_sorted_by_path_then_line(self):
        out = select([f(path="b.cc", line=1), f(path="a.cc", line=10),
                      f(path="a.cc", line=2)], "full", {})
        self.assertEqual([(x.path, x.line) for x in out],
                         [("a.cc", 2), ("a.cc", 10), ("b.cc", 1)])

    def test_unknown_scope_raises(self):
        with self.assertRaises(ValueError):
            select([f()], "bogus", {})


class FormatTest(unittest.TestCase):
    def test_report_lines_and_counts(self):
        text = format_report([f(line=3, rule="nullPointer", message="Null"),
                              f(path="b.cc", tool="cpplint", rule="readability/todo",
                                message="Missing username")])
        self.assertEqual(text.splitlines(), [
            "a.cc:3: [cppcheck:nullPointer] Null",
            "b.cc:1: [cpplint:readability/todo] Missing username",
            "-- 2 finding(s) (cppcheck: 1, cpplint: 1)",
        ])

    def test_empty_report(self):
        self.assertEqual(format_report([]), "-- no findings")
```

`tests/test_diffparse.py`:
```python
import unittest

from fork_check.diffparse import parse_changed_lines

MODIFY = """\
diff --git a/src/a.cc b/src/a.cc
index 1111111..2222222 100644
--- a/src/a.cc
+++ b/src/a.cc
@@ -10,0 +11,2 @@ void F() {
+  int x = 1;
+  int y = 2;
@@ -20 +22 @@ void G() {
-  old();
+  new_call();
"""

NEW_FILE = """\
diff --git a/src/new.h b/src/new.h
new file mode 100644
index 0000000..3333333
--- /dev/null
+++ b/src/new.h
@@ -0,0 +1,3 @@
+#pragma once
+
+int F();
"""

DELETED = """\
diff --git a/src/gone.cc b/src/gone.cc
deleted file mode 100644
index 4444444..0000000
--- a/src/gone.cc
+++ /dev/null
@@ -1,2 +0,0 @@
-int a;
-int b;
"""

PURE_DELETION_HUNK = """\
diff --git a/src/a.cc b/src/a.cc
index 1111111..2222222 100644
--- a/src/a.cc
+++ b/src/a.cc
@@ -5,2 +4,0 @@ void F() {
-  a();
-  b();
"""

RENAME_ONLY = """\
diff --git a/src/old.cc b/src/renamed.cc
similarity index 100%
rename from src/old.cc
rename to src/renamed.cc
"""

RENAME_WITH_EDIT = """\
diff --git a/src/old.cc b/src/renamed.cc
similarity index 90%
rename from src/old.cc
rename to src/renamed.cc
index 1111111..2222222 100644
--- a/src/old.cc
+++ b/src/renamed.cc
@@ -3 +3 @@
-int a;
+int b;
"""

BINARY = """\
diff --git a/assets/icon.png b/assets/icon.png
index 1111111..2222222 100644
Binary files a/assets/icon.png and b/assets/icon.png differ
"""


class ParseChangedLinesTest(unittest.TestCase):
    def test_modified_file(self):
        self.assertEqual(parse_changed_lines(MODIFY), {"src/a.cc": {11, 12, 22}})

    def test_new_file(self):
        self.assertEqual(parse_changed_lines(NEW_FILE), {"src/new.h": {1, 2, 3}})

    def test_deleted_file_absent(self):
        self.assertEqual(parse_changed_lines(DELETED), {})

    def test_pure_deletion_hunk_adds_no_lines(self):
        self.assertEqual(parse_changed_lines(PURE_DELETION_HUNK), {"src/a.cc": set()})

    def test_pure_rename_absent(self):
        self.assertEqual(parse_changed_lines(RENAME_ONLY), {})

    def test_rename_with_edit_uses_new_path(self):
        self.assertEqual(parse_changed_lines(RENAME_WITH_EDIT), {"src/renamed.cc": {3}})

    def test_binary_file_absent(self):
        self.assertEqual(parse_changed_lines(BINARY), {})

    def test_multiple_files(self):
        out = parse_changed_lines(MODIFY + NEW_FILE + DELETED + BINARY)
        self.assertEqual(set(out), {"src/a.cc", "src/new.h"})

    def test_empty_diff(self):
        self.assertEqual(parse_changed_lines(""), {})
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `UT tests.test_findings tests.test_diffparse`
Expected: errors `ModuleNotFoundError: No module named 'fork_check.findings'` and `'fork_check.diffparse'`.

- [ ] **Step 3: Implement**

`fork_check/findings.py`:
```python
"""Findings, the checker-broken error and report formatting."""

from __future__ import annotations

import collections
import dataclasses
from typing import Iterable

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_BROKEN = 2

SCOPES = ("changed", "files", "full")


@dataclasses.dataclass(frozen=True)
class Finding:
    path: str  # Repo-relative, forward slashes.
    line: int
    tool: str
    rule: str
    message: str


class CheckerBroken(Exception):
    """The checker could not produce a trustworthy result (exit code 2)."""


def _in_scope(finding: Finding, scope: str,
              changed: dict[str, set[int]]) -> bool:
    if scope == "full":
        return True
    lines = changed.get(finding.path)
    if lines is None:
        return False
    return scope == "files" or finding.line in lines


def select(findings: Iterable[Finding], scope: str,
           changed: dict[str, set[int]]) -> list[Finding]:
    """Keeps the findings in scope, without duplicates, sorted."""
    if scope not in SCOPES:
        raise ValueError(f"unknown scope {scope!r}")
    kept: dict[tuple, Finding] = {}
    for finding in findings:
        if _in_scope(finding, scope, changed):
            key = (finding.path, finding.line, finding.tool, finding.rule)
            kept.setdefault(key, finding)
    return sorted(kept.values(),
                  key=lambda x: (x.path, x.line, x.tool, x.rule))


def format_report(findings: list[Finding]) -> str:
    if not findings:
        return "-- no findings"
    lines = [f"{x.path}:{x.line}: [{x.tool}:{x.rule}] {x.message}"
             for x in findings]
    counts = collections.Counter(x.tool for x in findings)
    summary = ", ".join(f"{tool}: {n}" for tool, n in sorted(counts.items()))
    lines.append(f"-- {len(findings)} finding(s) ({summary})")
    return "\n".join(lines)
```

`fork_check/diffparse.py`:
```python
"""Turns `git diff -U0` output into the line numbers each file adds."""

from __future__ import annotations

import re

_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


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
            target = line[4:]
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `UT tests.test_findings tests.test_diffparse`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/findings.py fork_check/diffparse.py tests/test_findings.py tests/test_diffparse.py
printf '[Tools] Add findings model and diff line parsing\n' | git commit -q -F -
```

---

### Task 3: Codacy exclusions and tool output parsers

**Files:**
- Create: `fork_check/excludes.py`, `fork_check/parsers.py`
- Test: `tests/test_excludes.py`, `tests/test_parsers.py`

**Interfaces:**
- Consumes: `findings.Finding`, `findings.CheckerBroken` (Task 2); `tests/data/*.txt` (Task 1).
- Produces:
  - `excludes.SOURCE_EXTS: tuple[str, ...]`
  - `excludes.load_codacy_excludes(yaml_text: str) -> list[str]`
  - `excludes.is_excluded(path: str, patterns: list[str]) -> bool`
  - `excludes.analyzable(path: str, patterns: list[str]) -> bool` — has a source extension and is not excluded.
  - `parsers.rel(path: str, root: str) -> str | None`
  - `parsers.parse_cppcheck(text: str, root: str) -> list[Finding]`
  - `parsers.parse_flawfinder(text: str, root: str) -> list[Finding]`
  - `parsers.parse_clang_tidy(text: str, root: str) -> list[Finding]`
  - `parsers.parse_cpplint(text: str, root: str) -> list[Finding]`
  - Tool names used in findings: `"cppcheck"`, `"flawfinder"`, `"clang-tidy"`, `"cpplint"`.

- [ ] **Step 1: Write the failing tests**

`tests/test_excludes.py`:
```python
import unittest

from fork_check.excludes import analyzable, is_excluded, load_codacy_excludes

CODACY = """\
---
exclude_paths:
  - 'tools/**'
  - 'third_party/**'
  - "docs/**"
  - android/**
  - '**.md'
  - 'src/xenia/gpu/shaders/**'
"""


class ExcludesTest(unittest.TestCase):
    def setUp(self):
        self.patterns = load_codacy_excludes(CODACY)

    def test_loads_all_quote_styles(self):
        self.assertEqual(self.patterns, [
            "tools/**", "third_party/**", "docs/**", "android/**", "**.md",
            "src/xenia/gpu/shaders/**"])

    def test_prefix_patterns(self):
        self.assertTrue(is_excluded("third_party/fmt/x.h", self.patterns))
        self.assertTrue(is_excluded("src/xenia/gpu/shaders/bytecode/a.h", self.patterns))
        self.assertFalse(is_excluded("src/xenia/gpu/shader.cc", self.patterns))

    def test_double_star_suffix(self):
        self.assertTrue(is_excluded("src/README.md", self.patterns))

    def test_no_partial_directory_match(self):
        self.assertFalse(is_excluded("src/tools/a.cc", self.patterns))

    def test_analyzable(self):
        self.assertTrue(analyzable("src/xenia/base/a.cc", self.patterns))
        self.assertTrue(analyzable("src/xenia/base/a.inl", self.patterns))
        self.assertFalse(analyzable("src/xenia/base/a.py", self.patterns))
        self.assertFalse(analyzable("third_party/a.cc", self.patterns))

    def test_empty_config(self):
        self.assertEqual(load_codacy_excludes(""), [])
```

`tests/test_parsers.py` (sample-driven; asserts the hits Task 1 Step 7 confirmed are present):
```python
import os
import re
import unittest

from fork_check.findings import CheckerBroken
from fork_check.parsers import (parse_clang_tidy, parse_cppcheck, parse_cpplint,
                                parse_flawfinder, rel)

DATA = os.path.join(os.path.dirname(__file__), "data")
PLANTED = "src/xenia/base/planted.cc"


def sample(name):
    with open(os.path.join(DATA, name), encoding="utf-8") as f:
        return f.read()


def tidy_root():
    """The scratch root the clang-tidy sample was captured under."""
    m = re.search(r"^(/\S+?)/src/xenia/base/planted\.cc:", sample("clang_tidy.txt"), re.M)
    return m.group(1)


class RelTest(unittest.TestCase):
    def test_relative_path_kept(self):
        self.assertEqual(rel("src/a.cc", "/root"), "src/a.cc")

    def test_dot_slash_stripped(self):
        self.assertEqual(rel("./src/a.cc", "/root"), "src/a.cc")

    def test_absolute_under_root(self):
        self.assertEqual(rel("/root/src/a.cc", "/root"), "src/a.cc")

    def test_rel_outside_root_is_none(self):
        self.assertIsNone(rel("/usr/include/c++/13/vector", "/root"))

    def test_root_prefix_is_not_a_directory_match(self):
        self.assertIsNone(rel("/root2/src/a.cc", "/root"))

    def test_dotdot_normalized(self):
        self.assertEqual(rel("/root/src/x/../a.cc", "/root"), "src/a.cc")


class CppcheckTest(unittest.TestCase):
    def test_sample(self):
        out = parse_cppcheck(sample("cppcheck.txt"), "/unused")
        self.assertTrue(out)
        self.assertTrue(all(x.path == PLANTED and x.tool == "cppcheck" for x in out))
        self.assertIn("nullPointer", {x.rule for x in out})

    def test_information_lines_ignored(self):
        text = ("nofile:0:0: information:checkersReport: Active checkers: 1\n"
                "src/a.cc:3:1: information:missingIncludeSystem: Include not found\n")
        self.assertEqual(parse_cppcheck(text, "/r"), [])

    def test_line_fields(self):
        out = parse_cppcheck("src/a.cc:12:5: warning:nullPointer: Null pointer dereference: p\n", "/r")
        self.assertEqual(len(out), 1)
        self.assertEqual((out[0].path, out[0].line, out[0].rule, out[0].message),
                         ("src/a.cc", 12, "nullPointer", "Null pointer dereference: p"))

    def test_unparseable_line_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_cppcheck("cppcheck: error: unrecognized command line option\n", "/r")


class FlawfinderTest(unittest.TestCase):
    def test_sample(self):
        out = parse_flawfinder(sample("flawfinder.txt"), "/unused")
        self.assertIn("strcpy", {x.rule for x in out})
        self.assertTrue(all(x.path == PLANTED for x in out))

    def test_level_in_message(self):
        line = ("src/a.cc:7:3:  [4] (buffer) strcpy:Does not check for buffer "
                "overflows when copying to destination (CWE-120).\n")
        out = parse_flawfinder(line, "/r")
        self.assertEqual((out[0].line, out[0].rule), (7, "strcpy"))
        self.assertTrue(out[0].message.startswith("[4] (buffer) Does not check"))

    def test_unparseable_line_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_flawfinder("Traceback (most recent call last):\n", "/r")


class ClangTidyTest(unittest.TestCase):
    def test_sample(self):
        out = parse_clang_tidy(sample("clang_tidy.txt"), tidy_root())
        rules = {x.rule for x in out}
        self.assertIn("clang-analyzer-core.NullDereference", rules)
        self.assertIn("readability-identifier-naming", rules)
        self.assertIn("google-readability-todo", rules)
        self.assertTrue(all(x.path == PLANTED and x.tool == "clang-tidy" for x in out))

    def test_notes_and_noise_ignored(self):
        text = ("/r/src/a.cc:5:3: note: Null pointer value stored to 'p'\n"
                "   5 |   int* p = nullptr;\n"
                "      |   ^\n"
                "12 warnings generated.\n"
                "Suppressed 11 warnings (11 in non-user code).\n")
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_multiple_checks_on_one_warning(self):
        text = "/r/src/a.cc:5:3: warning: thing [bugprone-a,bugprone-b]\n"
        self.assertEqual({x.rule for x in parse_clang_tidy(text, "/r")},
                         {"bugprone-a", "bugprone-b"})

    def test_clang_tidy_drops_outside_root(self):
        text = "/usr/include/c++/13/bits/a.h:5:3: warning: thing [bugprone-a]\n"
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_compiler_warnings_dropped(self):
        text = "/r/src/a.cc:5:3: warning: unused variable 'x' [clang-diagnostic-unused-variable]\n"
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_compile_error_is_broken(self):
        text = "/r/src/a.cc:1:10: error: 'missing.h' file not found [clang-diagnostic-error]\n"
        with self.assertRaises(CheckerBroken):
            parse_clang_tidy(text, "/r")

    def test_error_without_check_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_clang_tidy("Error while processing /r/src/a.cc.\n", "/r")


class CpplintTest(unittest.TestCase):
    def test_sample(self):
        out = parse_cpplint(sample("cpplint.txt"), "/unused")
        self.assertIn("readability/todo", {x.rule for x in out})
        self.assertTrue(all(x.path == PLANTED for x in out))

    def test_line_fields(self):
        text = "src/a.cc:7:  Missing username in TODO; it should look like \"// TODO(my_username): Stuff.\"  [readability/todo] [2]\n"
        out = parse_cpplint(text, "/r")
        self.assertEqual((out[0].line, out[0].rule), (7, "readability/todo"))
        self.assertTrue(out[0].message.startswith("Missing username in TODO"))

    def test_summary_lines_ignored(self):
        text = "Done processing src/a.cc\nTotal errors found: 0\n"
        self.assertEqual(parse_cpplint(text, "/r"), [])

    def test_skipped_input_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_cpplint("Skipping input 'src/a.cc': Can't open for reading\n", "/r")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `UT tests.test_excludes tests.test_parsers`
Expected: `ModuleNotFoundError` for `fork_check.excludes` and `fork_check.parsers`.

- [ ] **Step 3: Implement**

`fork_check/excludes.py`:
```python
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
```

`fork_check/parsers.py`:
```python
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
_CPPCHECK_IGNORED_IDS = {"missingInclude", "missingIncludeSystem",
                         "checkersReport", "unmatchedSuppression"}


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


# --dataonly --singleline: path:line:col:  [level] (category) function:message
_FLAWFINDER_RE = re.compile(r"^(.+?):(\d+):(\d+):\s+\[(\d)\] \((\w+)\) (\w+):(.*)$")


def parse_flawfinder(text: str, root: str) -> list[Finding]:
    findings = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = _FLAWFINDER_RE.match(line)
        if match is None:
            raise CheckerBroken(f"flawfinder: unexpected output: {line}")
        path, line_no, _, level, category, function, message = match.groups()
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `UT tests.test_excludes tests.test_parsers`
Expected: all pass. If a `test_sample` fails, compare the regex with the real line in `tests/data/` and fix the regex (the samples are the source of truth), then re-run all of `tests.test_parsers`.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/excludes.py fork_check/parsers.py tests/test_excludes.py tests/test_parsers.py
printf '[Tools] Add Codacy exclusions and tool output parsers\n' | git commit -q -F -
```

---

### Task 4: clang-tidy source selection

**Files:**
- Create: `fork_check/tidy_sources.py`
- Test: `tests/test_tidy_sources.py`

**Interfaces:**
- Produces:
  - `tidy_sources.load_compile_db_files(db_path: str, root: str) -> list[str]` — repo-relative paths of every entry in `compile_commands.json`, deduplicated, in file order.
  - `tidy_sources.choose_tidy_sources(touched: list[str], db_files: list[str], read_text: Callable[[str], str]) -> tuple[list[str], list[str]]` — returns `(sources_to_run, headers_without_includer)`. `touched` are repo-relative analyzable paths; `read_text(path)` returns a repo-relative file's text.

Rules: touched files present in `db_files` run directly. For each touched header (`.h .hpp .inl`) not included by any touched source in the run list, the first `db_files` entry whose text includes it is added. A header's include string is its path with a leading `src/` removed (xenia includes as `"xenia/base/foo.h"`). Headers with no includer are returned in the second list.

- [ ] **Step 1: Write the failing tests**

`tests/test_tidy_sources.py`:
```python
import json
import os
import tempfile
import unittest

from fork_check.tidy_sources import choose_tidy_sources, load_compile_db_files

FILES = {
    "src/xenia/base/a.cc": '#include "xenia/base/a.h"\n',
    "src/xenia/base/b.cc": '#include "xenia/base/shared.h"\n',
    "src/xenia/base/c.cc": '#include "xenia/base/shared.h"\n',
    "src/xenia/base/a.h": "",
    "src/xenia/base/shared.h": "",
    "src/xenia/base/orphan.h": "",
}
DB = ["src/xenia/base/a.cc", "src/xenia/base/b.cc", "src/xenia/base/c.cc"]


class ChooseTest(unittest.TestCase):
    def choose(self, touched):
        return choose_tidy_sources(touched, DB, FILES.__getitem__)

    def test_touched_sources_run_directly(self):
        self.assertEqual(self.choose(["src/xenia/base/b.cc"]), (["src/xenia/base/b.cc"], []))

    def test_header_covered_by_touched_source(self):
        self.assertEqual(self.choose(["src/xenia/base/a.cc", "src/xenia/base/a.h"]),
                         (["src/xenia/base/a.cc"], []))

    def test_header_without_touched_source_uses_includer(self):
        self.assertEqual(self.choose(["src/xenia/base/shared.h"]),
                         (["src/xenia/base/b.cc"], []))

    def test_header_with_no_includer_reported(self):
        self.assertEqual(self.choose(["src/xenia/base/orphan.h"]),
                         ([], ["src/xenia/base/orphan.h"]))

    def test_source_missing_from_db_skipped(self):
        self.assertEqual(self.choose(["src/xenia/base/new.cc"]), ([], []))

    def test_no_duplicates(self):
        sources, _ = self.choose(["src/xenia/base/b.cc", "src/xenia/base/shared.h"])
        self.assertEqual(sources, ["src/xenia/base/b.cc"])


class LoadDbTest(unittest.TestCase):
    def test_relative_dedup_and_outside_root_dropped(self):
        with tempfile.TemporaryDirectory() as d:
            db = os.path.join(d, "compile_commands.json")
            with open(db, "w") as f:
                json.dump([
                    {"directory": "/r/build", "file": "/r/src/a.cc", "command": "x"},
                    {"directory": "/r/build", "file": "/r/src/a.cc", "command": "y"},
                    {"directory": "/r/build", "file": "../src/b.cc", "command": "z"},
                    {"directory": "/r/build", "file": "/elsewhere/c.cc", "command": "w"},
                ], f)
            self.assertEqual(load_compile_db_files(db, "/r"), ["src/a.cc", "src/b.cc"])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `UT tests.test_tidy_sources`
Expected: `ModuleNotFoundError: No module named 'fork_check.tidy_sources'`.

- [ ] **Step 3: Implement**

`fork_check/tidy_sources.py`:
```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `UT tests.test_tidy_sources`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/tidy_sources.py tests/test_tidy_sources.py
printf '[Tools] Pick clang-tidy sources, including includers of touched headers\n' | git commit -q -F -
```

---

### Task 5: Scratch clone, configure cache and shader headers

**Files:**
- Create: `fork_check/snapshot.py`
- Test: `tests/test_snapshot.py`

**Interfaces:**
- Consumes: `findings.CheckerBroken`.
- Produces:
  - `snapshot.SCRATCH: pathlib.Path` = `~/src/xenia-fork-check`
  - `snapshot.git(root: Path, *args: str, check: bool = True) -> str` — runs git, returns stdout; raises `CheckerBroken` on failure when `check`.
  - `snapshot.prepare(win_repo: str, commit: str, base: str) -> Path` — clone if needed, fetch, verify both commits exist, check out `commit`, update submodules; returns the scratch root.
  - `snapshot.cache_key(cmake_files_listing: str, source_listing: str, submodules: str) -> str` — pure; sha256 hex.
  - `snapshot.configure(root: Path) -> Path` — returns the build dir containing `compile_commands.json`; re-configures only when the cache key changed; always brings shader headers up to date.

- [ ] **Step 1: Write the failing test for the pure part**

`tests/test_snapshot.py`:
```python
import unittest

from fork_check.snapshot import cache_key

CMAKE = "100644 aaa 0\tCMakeLists.txt\n100644 bbb 0\tcmake/XeniaHelpers.cmake\n"
SOURCES = "src/xenia/base/a.cc\nsrc/xenia/base/b.cc\n"
SUBS = " d980192 third_party/FFmpeg (d980192)\n"


class CacheKeyTest(unittest.TestCase):
    def test_stable(self):
        self.assertEqual(cache_key(CMAKE, SOURCES, SUBS), cache_key(CMAKE, SOURCES, SUBS))

    def test_cache_key_changes_when_cmake_changes(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE.replace("aaa", "ccc"), SOURCES, SUBS))

    def test_cache_key_changes_when_source_added(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE, SOURCES + "src/xenia/base/c.cc\n", SUBS))

    def test_cache_key_changes_when_submodule_moves(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE, SOURCES, SUBS.replace("d980192", "e000000")))

    def test_fields_do_not_run_together(self):
        self.assertNotEqual(cache_key("ab", "c", ""), cache_key("a", "bc", ""))
```

- [ ] **Step 2: Run it to verify it fails**

Run: `UT tests.test_snapshot`
Expected: `ModuleNotFoundError: No module named 'fork_check.snapshot'`.

- [ ] **Step 3: Implement**

`fork_check/snapshot.py`:
```python
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
    git(root, "fetch", "--no-tags", "--force", win_repo,
        "+refs/heads/*:refs/remotes/win/*",
        "+refs/remotes/upstream/*:refs/remotes/upstream/*")
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


def _current_key(root: pathlib.Path) -> str:
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
    shader_targets = [line.split(":")[0] for line in targets.stdout.splitlines()
                      if line.split(":")[0].endswith("-shaders")]
    if shader_targets:
        proc = _run(["ninja", "-C", str(build), *shader_targets], env=env)
        if proc.returncode != 0:
            raise CheckerBroken("generating shader headers failed:\n"
                                + (proc.stdout + proc.stderr)[-4000:])
    if stale:
        key_path.write_text(key + "\n")
    return build
```

- [ ] **Step 4: Run the unit test, then verify against the real clone**

Run: `UT tests.test_snapshot` — expected: all pass.

Then (the scratch clone from Task 1 already exists):
```bash
wsl -d Ubuntu-24.04 -- bash -lc '
cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling
python3 - <<EOF
import time
from fork_check import snapshot
W="/mnt/c/Users/Adam/AGENTS/xenia-canary"
c=snapshot.git(snapshot.SCRATCH,"rev-parse","win/gpu/release-trace-writer").strip()
b=snapshot.git(snapshot.SCRATCH,"merge-base","upstream/canary_experimental",c).strip()
t=time.time(); root=snapshot.prepare(W,c,b); print("prepare", round(time.time()-t,1),"s")
t=time.time(); build=snapshot.configure(root); print("configure #1", round(time.time()-t,1),"s")
t=time.time(); build=snapshot.configure(root); print("configure #2", round(time.time()-t,1),"s")
print((build/"compile_commands.json").exists())
EOF'
```
Expected: prints `True`; `configure #2` is much faster than `#1` (cache hit, only the shader no-op). Also check `prepare` with a bogus commit raises `CheckerBroken` mentioning "not found":
```bash
wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling && python3 -c "from fork_check import snapshot; snapshot.prepare(\"/mnt/c/Users/Adam/AGENTS/xenia-canary\", \"0\"*40, \"0\"*40)"'
```
Expected: traceback ending in `CheckerBroken: commit 0000... not found after fetching ...`.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/snapshot.py tests/test_snapshot.py
printf '[Tools] Add the scratch clone and cached configure\n' | git commit -q -F -
```

---

### Task 6: Tool runners and configs

**Files:**
- Create: `fork_check/runners.py`, `config/clang-tidy.yaml`

**Interfaces:**
- Consumes: parsers (Task 3), `choose_tidy_sources`, `load_compile_db_files` (Task 4), `CheckerBroken`.
- Produces:
  - `runners.TOOLS: tuple[str, ...] = ("cppcheck", "flawfinder", "clang-tidy", "cpplint")`
  - `runners.require(tools: Iterable[str]) -> None` — raises `CheckerBroken` naming every missing executable and its install command.
  - `runners.run_cppcheck(root: Path, files: list[str]) -> list[Finding]`
  - `runners.run_flawfinder(root: Path, files: list[str]) -> list[Finding]`
  - `runners.run_cpplint(root: Path, files: list[str]) -> list[Finding]`
  - `runners.run_clang_tidy(root: Path, build: Path, files: list[str]) -> tuple[list[Finding], list[str]]` — second value: notes to print (headers with no includer).
  - `runners.CONFIG_DIR: Path` — `<tooling>/config`.

- [ ] **Step 1: Write the clang-tidy config**

`config/clang-tidy.yaml`:
```yaml
# clang-tidy settings for the fork check. Naming follows the Google C++ style
# guide, which docs/style_guide.md says xenia follows. Calibration changes go
# here with a comment giving the reason.
Checks: >
  -*,
  bugprone-*,
  clang-analyzer-*,
  google-readability-todo,
  readability-identifier-naming,
  -bugprone-easily-swappable-parameters
HeaderFilterRegex: '.*/src/xenia/.*'
CheckOptions:
  readability-identifier-naming.NamespaceCase: lower_case
  readability-identifier-naming.ClassCase: CamelCase
  readability-identifier-naming.StructCase: CamelCase
  # Guest-layout structs mirror the console's names (X_KTHREAD and so on).
  readability-identifier-naming.StructIgnoredRegexp: '^X_[A-Z0-9_]+$'
  readability-identifier-naming.EnumCase: CamelCase
  readability-identifier-naming.EnumConstantCase: CamelCase
  readability-identifier-naming.EnumConstantPrefix: k
  readability-identifier-naming.EnumConstantIgnoredRegexp: '^X_[A-Z0-9_]+$'
  readability-identifier-naming.TypeAliasCase: CamelCase
  readability-identifier-naming.TypedefCase: CamelCase
  readability-identifier-naming.FunctionCase: CamelCase
  readability-identifier-naming.MethodCase: CamelCase
  # Google allows accessors and mutators named like variables: count(), set_count().
  readability-identifier-naming.MethodIgnoredRegexp: '^[a-z][a-z0-9_]*$'
  readability-identifier-naming.VariableCase: lower_case
  readability-identifier-naming.ParameterCase: lower_case
  readability-identifier-naming.MemberCase: lower_case
  readability-identifier-naming.PrivateMemberSuffix: _
  readability-identifier-naming.ProtectedMemberSuffix: _
  readability-identifier-naming.GlobalConstantCase: CamelCase
  readability-identifier-naming.GlobalConstantPrefix: k
  readability-identifier-naming.StaticConstantCase: CamelCase
  readability-identifier-naming.StaticConstantPrefix: k
  readability-identifier-naming.ConstexprVariableCase: CamelCase
  readability-identifier-naming.ConstexprVariablePrefix: k
```

- [ ] **Step 2: Implement the runners**

`fork_check/runners.py`:
```python
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
from fork_check.tidy_sources import choose_tidy_sources, load_compile_db_files

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
# Only what clang-format and `xb lint` don't already enforce.
CPPLINT_FILTER = ("-,+build/include_order,+build/include_what_you_use,"
                  "+readability/todo,+runtime,+build/namespaces")
CPPCHECK_TEMPLATE = "{file}:{line}:{column}: {severity}:{id}: {message}"
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


def run_cppcheck(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for batch in _batches(files):
        proc = _run(["cppcheck", "--quiet", "--enable=warning,style,performance",
                     f"--template={CPPCHECK_TEMPLATE}", f"-j{os.cpu_count() or 1}",
                     *batch], root)
        findings += parsers.parse_cppcheck(proc.stderr, str(root))
    return findings


def run_flawfinder(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for batch in _batches(files):
        proc = _run(["flawfinder", "--dataonly", "--quiet", "--singleline", *batch], root)
        findings += parsers.parse_flawfinder(proc.stdout, str(root))
    return findings


def run_cpplint(root: pathlib.Path, files: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    for batch in _batches(files):
        # cpplint exits 1 when it reports anything.
        proc = _run(["cpplint", "--quiet", "--output=emacs",
                     f"--filter={CPPLINT_FILTER}", *batch], root, ok_codes=(0, 1))
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
```

- [ ] **Step 3: Verify each runner against the scratch clone**

Plant the sample file in the scratch clone (new `.cc` in `src/xenia/base` is picked up by the GLOB after reconfigure; the key changes only if the file is tracked, so commit it there on a throwaway detached commit):
```bash
wsl -d Ubuntu-24.04 -- bash -lc '
set -e
T=/mnt/c/Users/Adam/AGENTS/xenia-canary-tooling
cd ~/src/xenia-fork-check
cp $T/tests/data/planted.cc src/xenia/base/planted.cc
cp $T/tests/data/planted.cc src/xenia/base/planted.h
git add src/xenia/base/planted.cc src/xenia/base/planted.h
git -c user.name=t -c user.email=t@t commit -qm planted
cd $T
python3 - <<EOF
from fork_check import runners, snapshot
root = snapshot.SCRATCH
runners.require(runners.TOOLS)
build = snapshot.configure(root)
files = ["src/xenia/base/planted.cc"]
for name, fn in [("cppcheck", runners.run_cppcheck), ("flawfinder", runners.run_flawfinder), ("cpplint", runners.run_cpplint)]:
    print(name, sorted({x.rule for x in fn(root, files)}))
f, notes = runners.run_clang_tidy(root, build, files)
print("clang-tidy", sorted({x.rule for x in f}))
f, notes = runners.run_clang_tidy(root, build, ["src/xenia/base/planted.h"])
print("header", notes)
EOF'
```
Expected: cppcheck includes `nullPointer`; flawfinder includes `strcpy`; cpplint includes `readability/todo`; clang-tidy includes `clang-analyzer-core.NullDereference`, `google-readability-todo` and `readability-identifier-naming`; `header` prints a note that `planted.h` has no includer. The next `prepare()` checkout resets the scratch clone, so the throwaway commit needs no cleanup.

Verify `require()` names a missing tool:
```bash
wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling && PATH=/usr/bin/nonexistent:/bin python3 -c "from fork_check import runners; runners.require(runners.TOOLS)"'
```
Expected: `CheckerBroken: missing tools:` listing `cppcheck`, `flawfinder`, `clang-tidy-20`, `cpplint` with install commands.

- [ ] **Step 4: Run all unit tests**

Run: `UT`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/runners.py config/clang-tidy.yaml
printf '[Tools] Add tool runners and the clang-tidy config\n' | git commit -q -F -
```

---

### Task 7: CLI and Windows wrapper

**Files:**
- Create: `fork_check/cli.py`, `check.ps1`
- Test: `tests/test_cli.py`
- Modify: `docs/superpowers/specs/2026-10-15-fork-check-design.md` (Interface section: PowerShell-style flags)

**Interfaces:**
- Consumes: everything above.
- Produces: `cli.main(argv: list[str] | None = None) -> int`; `check.ps1 [-Base REF] [-AllLines | -Full] [-Tools a,b]`.

CLI arguments: `--win-repo PATH` (WSL path, required), `--commit SHA` (required), `--base SHA` (required), `--scope {changed,files,full}` (default `changed`), `--tools LIST` (comma-separated subset of `TOOLS`, default all), `--dirty` (flag: warn that uncommitted changes are not analyzed).

- [ ] **Step 1: Write the failing test for argument handling**

`tests/test_cli.py`:
```python
import io
import unittest
from contextlib import redirect_stderr
from unittest import mock

from fork_check import cli


class CliArgsTest(unittest.TestCase):
    def test_unknown_tool_is_usage_error(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
            cli.parse_args(["--win-repo", "/w", "--commit", "c", "--base", "b",
                            "--tools", "cppcheck,bogus"])
        self.assertEqual(cm.exception.code, 2)

    def test_defaults(self):
        args = cli.parse_args(["--win-repo", "/w", "--commit", "c", "--base", "b"])
        self.assertEqual(args.scope, "changed")
        self.assertEqual(args.tools, ["cppcheck", "flawfinder", "clang-tidy", "cpplint"])

    def test_broken_returns_2(self):
        with mock.patch.object(cli.runners, "require",
                               side_effect=cli.CheckerBroken("missing tools")), \
             redirect_stderr(io.StringIO()) as err:
            code = cli.main(["--win-repo", "/w", "--commit", "c", "--base", "b"])
        self.assertEqual(code, 2)
        self.assertIn("checker broken: missing tools", err.getvalue())
```

- [ ] **Step 2: Run it to verify it fails**

Run: `UT tests.test_cli`
Expected: `ModuleNotFoundError: No module named 'fork_check.cli'`.

- [ ] **Step 3: Implement the CLI**

`fork_check/cli.py`:
```python
"""Command line entry point (run inside WSL by check.ps1)."""

from __future__ import annotations

import argparse
import sys

from fork_check import runners, snapshot
from fork_check.diffparse import parse_changed_lines
from fork_check.excludes import analyzable, load_codacy_excludes
from fork_check.findings import (EXIT_BROKEN, EXIT_CLEAN, EXIT_FINDINGS,
                                 CheckerBroken, format_report, select)


def _tool_list(text: str) -> list[str]:
    tools = [t.strip() for t in text.split(",") if t.strip()]
    unknown = [t for t in tools if t not in runners.TOOLS]
    if unknown or not tools:
        raise argparse.ArgumentTypeError(
            f"unknown tool(s) {unknown}; choose from {', '.join(runners.TOOLS)}")
    return tools


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="fork_check")
    ap.add_argument("--win-repo", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--scope", choices=("changed", "files", "full"), default="changed")
    ap.add_argument("--tools", type=_tool_list, default=list(runners.TOOLS))
    ap.add_argument("--dirty", action="store_true")
    return ap.parse_args(argv)


def _run(args: argparse.Namespace) -> int:
    runners.require(args.tools)
    if args.dirty:
        print("warning: uncommitted changes are not analyzed", file=sys.stderr)
    root = snapshot.prepare(args.win_repo, args.commit, args.base)
    changed = parse_changed_lines(snapshot.git(
        root, "-c", "core.quotePath=false", "diff", "-U0", "--no-color",
        "--no-ext-diff", "-M", args.base, args.commit))
    codacy = root / ".codacy.yaml"
    excludes = load_codacy_excludes(codacy.read_text()) if codacy.exists() else []
    if args.scope == "full":
        candidates = snapshot.git(root, "ls-files").splitlines()
    else:
        candidates = list(changed)
    files = sorted(p for p in candidates if analyzable(p, excludes))
    if not files:
        print("-- no C/C++ files to check")
        return EXIT_CLEAN

    findings = []
    notes: list[str] = []
    if "cppcheck" in args.tools:
        findings += runners.run_cppcheck(root, files)
    if "flawfinder" in args.tools:
        findings += runners.run_flawfinder(root, files)
    if "cpplint" in args.tools:
        findings += runners.run_cpplint(root, files)
    if "clang-tidy" in args.tools:
        build = snapshot.configure(root)
        tidy_findings, notes = runners.run_clang_tidy(root, build, files)
        findings += tidy_findings

    selected = [x for x in select(findings, args.scope, changed)
                if analyzable(x.path, excludes)]
    for note in notes:
        print(note, file=sys.stderr)
    print(f"checked {len(files)} file(s), {args.base[:10]}..{args.commit[:10]}, "
          f"scope {args.scope}", file=sys.stderr)
    print(format_report(selected))
    return EXIT_FINDINGS if selected else EXIT_CLEAN


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return _run(args)
    except CheckerBroken as e:
        print(f"checker broken: {e}", file=sys.stderr)
        return EXIT_BROKEN
```

Note: `cli.CheckerBroken` is imported into `cli`'s namespace above, which the test relies on.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `UT`
Expected: all pass.

- [ ] **Step 5: Write the wrapper**

`check.ps1`:
```powershell
# Runs the fork check on the current branch of the repo in the current
# directory. See README.md.
param(
  [string]$Base,
  [switch]$AllLines,
  [switch]$Full,
  [string]$Tools
)
$ErrorActionPreference = 'Stop'
if ($AllLines -and $Full) { throw '-AllLines and -Full are mutually exclusive.' }

$repo = (git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE) { throw 'Run this from inside the xenia-canary repo.' }
$commit = (git rev-parse HEAD).Trim()
if ($Base) {
  $baseSha = (git rev-parse --verify "$Base^{commit}").Trim()
} else {
  $baseSha = (git merge-base upstream/canary_experimental HEAD).Trim()
}
if ($LASTEXITCODE -or -not $baseSha) { throw 'Could not resolve the base commit.' }
$dirty = [bool](git status --porcelain --untracked-files=no)

$scope = if ($Full) { 'full' } elseif ($AllLines) { 'files' } else { 'changed' }
$wslRepo = (wsl -d Ubuntu-24.04 -- wslpath -a "$repo").Trim()
$wslTooling = (wsl -d Ubuntu-24.04 -- wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()

$cliArgs = @('--win-repo', "'$wslRepo'", '--commit', $commit, '--base', $baseSha,
             '--scope', $scope)
if ($Tools) { $cliArgs += @('--tools', $Tools) }
if ($dirty) { $cliArgs += '--dirty' }

wsl -d Ubuntu-24.04 -- bash -lc "cd '$wslTooling' && python3 -m fork_check $($cliArgs -join ' ')"
exit $LASTEXITCODE
```

- [ ] **Step 6: Update the spec's Interface section**

In `docs/superpowers/specs/2026-10-15-fork-check-design.md`, replace
```
..\xenia-canary-tooling\check.ps1 [--base REF] [--all-lines | --full]
```
with
```
..\xenia-canary-tooling\check.ps1 [-Base REF] [-AllLines | -Full] [-Tools cppcheck,flawfinder,clang-tidy,cpplint]
```
and in the bullets below it rename `--base`, `--all-lines` and `--full` to `-Base`, `-AllLines` and `-Full`.

- [ ] **Step 7: Smoke-run on a real branch**

From the xenia-canary repo on `gpu/release-trace-writer`:
```powershell
cd C:\Users\Adam\AGENTS\xenia-canary
..\xenia-canary-tooling\check.ps1; "exit $LASTEXITCODE"
```
Expected: a `checked N file(s)` line, a report, and `exit 0` or `exit 1` (never 2). Save the report text for Task 8 calibration.

Then run with an explicit base equal to HEAD:
```powershell
..\xenia-canary-tooling\check.ps1 -Base HEAD; "exit $LASTEXITCODE"
```
Expected: `-- no C/C++ files to check`, `exit 0`.

- [ ] **Step 8: Commit**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add fork_check/cli.py tests/test_cli.py check.ps1 docs/superpowers/specs/2026-10-15-fork-check-design.md
printf '[Tools] Add the fork check CLI and Windows wrapper\n' | git commit -q -F -
```

---

### Task 8: End-to-end check, calibration, README, push

**Files:**
- Modify: `config/clang-tidy.yaml`, `fork_check/runners.py` (only `CPPLINT_FILTER` / cppcheck args, if calibration says so), `README.md`

- [ ] **Step 1: End-to-end with planted issues**

In the xenia-canary repo, on a throwaway branch:
```bash
cd /c/Users/Adam/AGENTS/xenia-canary
git switch -c scratch/fork-check-e2e upstream/canary_experimental
cp ../xenia-canary-tooling/tests/data/planted.cc src/xenia/base/fork_check_planted.cc
git add src/xenia/base/fork_check_planted.cc
git commit -qm "[Base] e2e planted issues (throwaway)"
pwsh -NoProfile -File ../xenia-canary-tooling/check.ps1; echo "exit $?"
```
Expected: exit 1, with at least one finding from each tool: `[cppcheck:nullPointer]`, `[flawfinder:strcpy]`, `[cpplint:readability/todo]`, `[clang-tidy:clang-analyzer-core.NullDereference]`, `[clang-tidy:readability-identifier-naming]` (for `lower_class`, `Bad_Namespace` or `value`), `[clang-tidy:google-readability-todo]`.

Then add one clean comment line in a second commit and run with `-Base HEAD~1`, so only that line is in scope:
```bash
printf '\n// Fork check end-to-end: clean change.\n' >> src/xenia/base/fork_check_planted.cc
git commit -qam "[Base] e2e clean line (throwaway)"
pwsh -NoProfile -File ../xenia-canary-tooling/check.ps1 -Base HEAD~1; echo "exit $?"
```
Expected: exit 0 and `-- no findings` (the planted issues are outside the changed lines).

Clean up:
```bash
git switch gpu/release-trace-writer
git branch -D scratch/fork-check-e2e
```

- [ ] **Step 2: Calibrate on accepted code**

Run on both branches that passed Codacy and review, with `-AllLines` for a bigger sample:
```powershell
cd C:\Users\Adam\AGENTS\xenia-canary
git switch gpu/release-trace-writer;     ..\xenia-canary-tooling\check.ps1 -AllLines > $env:TEMP\fc-1299.txt
git switch testing/main-thread-asserts;  ..\xenia-canary-tooling\check.ps1 -AllLines > $env:TEMP\fc-1300.txt
git switch gpu/release-trace-writer
```
For each `[tool:rule]` in the two reports, look at every hit and classify it as **real** (a bug or a genuine style-guide violation) or **noise**. Then, per rule:
- All noise, and the pattern is common in xenia (e.g. naming of guest structs, `constexpr` locals without `k`): disable or narrow it in `config/clang-tidy.yaml` (or remove the category from `CPPLINT_FILTER`, or add `--suppress=<id>` to the cppcheck command), with a one-line comment giving the reason and the count seen.
- Mixed: keep it.
- Exception: never disable a cppcheck or Flawfinder rule, because those mirror Codacy. If one fires on a line Codacy already passed in #1299/#1300, our setup differs from Codacy's; record the rule and the line under "Calibration notes" in the README instead.

Re-run both commands after tuning; repeat until every remaining rule has at least one real hit or is a Codacy-mirror rule.

- [ ] **Step 3: Run all unit tests**

Run: `UT`
Expected: all pass (config changes must not break the parsers' sample tests; if the clang-tidy sample test needs a rule that calibration disabled, keep the sample as-is — it tests parsing, not config).

- [ ] **Step 4: Write the README**

Replace `README.md` with: purpose (one paragraph, link to the spec); **Install** (the Task 1 Step 2 commands, plus the `safe.directory` step if Task 1 needed it); **Environment** (what Task 1 Step 3 found about `VULKAN_SDK`); **Usage** (`check.ps1` forms from the updated spec, what each scope means, exit codes 0/1/2); **What it checks** (the tool table from the spec); **Calibration notes** (every rule disabled or narrowed in Step 2 and why, and any Codacy-mirror discrepancy); **Tests** (the `UT` command). Keep it under 120 lines.

- [ ] **Step 5: Commit and push**

```bash
cd /c/Users/Adam/AGENTS/xenia-canary-tooling
git add -A
printf '[Tools] Calibrate fork check rules and document usage\n' | git commit -q -F -
git push -u origin tooling
```
Expected: push succeeds (the pre-push hook checks for CR and attribution lines). Then post a short comment on Ecredes/xenia-canary#10 linking the `tooling` branch and summarizing which checklist items it covers (prerequisite `compile_commands.json`, clang-tidy, cppcheck, cpplint for the style guide) — only after asking the user.
