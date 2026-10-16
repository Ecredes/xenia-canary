# fork check

Local pre-flight analysis for xenia-canary topic branches before they go
upstream: the Codacy tools upstream runs on every PR (cppcheck, Flawfinder),
plus clang-tidy and cpplint for bugs and `docs/style_guide.md` rules that
nothing upstream checks. Reports only the lines a branch changes. Fork-only
(Ecredes/xenia-canary#10); design in
`docs/superpowers/specs/2026-10-15-fork-check-design.md`.

## Usage

From the xenia-canary repo, on the branch to check:

```powershell
..\xenia-canary-tooling\check.ps1                  # lines changed since merge-base with upstream
..\xenia-canary-tooling\check.ps1 -AllLines        # everything in the touched files
..\xenia-canary-tooling\check.ps1 -Full            # whole codebase (slow; baseline report)
..\xenia-canary-tooling\check.ps1 -Base HEAD~3     # explicit base
..\xenia-canary-tooling\check.ps1 -Tools cppcheck,flawfinder
```

Output is `path:line: [tool:rule] message`, sorted, then a count line.

| Exit | Meaning |
|---|---|
| 0 | no findings |
| 1 | findings |
| 2 | checker broken (missing tool, configure failure, tool crash, unexpected output) — never a pass |

Only committed state is analyzed; with uncommitted changes it warns.

## How it works

`check.ps1` resolves HEAD, the base (`merge-base upstream/canary_experimental
HEAD`) and the dirty state with Windows git, then runs `python3 -m fork_check`
in the `Ubuntu-24.04` WSL distro. That keeps a scratch clone at
`~/src/xenia-fork-check` (never the normal `~/src/xenia-canary`), fetches the
Windows repo's branch heads into it, checks the commit out, configures CMake
(Ninja, Debug, clang-20, tests on) for `compile_commands.json`, generates the
SPIR-V shader headers, and runs the tools on the touched C/C++ files.
Paths excluded by upstream's `.codacy.yaml` are skipped by every tool.

The configure re-runs only when CMake files, the list of tracked source files
(CMake uses `file(GLOB)`) or submodule commits change. Typical timings: first
run ~4 min (submodule clone), later runs ~10–30 s.

## What it checks

| Lane | Tool | Rules |
|---|---|---|
| Codacy mirror | cppcheck 2.13 | `--enable=warning,style,performance`, no include paths, `.h` parsed as C++ |
| Codacy mirror | Flawfinder 2.0 | defaults |
| Bugs | clang-tidy 20 | `bugprone-*`, `clang-analyzer-*` (see `config/clang-tidy.yaml`) |
| Style guide | clang-tidy 20 | `google-readability-todo`, `readability-identifier-naming` (Google naming) |
| Style guide | cpplint 2.0 | `build/include_order`, `readability/todo`, `runtime/*` |

## Install (one time)

```bash
# clang-tidy (the apt.llvm.org noble llvm-toolchain-noble-20 source is already configured)
wsl -d Ubuntu-24.04 -u root -- apt-get install -y clang-tidy-20 pipx
wsl -d Ubuntu-24.04 -- bash -lc "pipx install cpplint && pipx install flawfinder && pipx ensurepath"
```

cppcheck 2.13 comes from Ubuntu (`apt install cppcheck`). No `safe.directory`
setup was needed. The WSL login shell already sets `CC`, `CXX` and
`VULKAN_SDK` (`~/.profile`); if `VULKAN_SDK` is missing the tool uses the
newest `~/vulkan-sdk/*/x86_64`.

The Windows repo is a partial clone, so upstream refs can't be fetched from it
(objects missing); the scratch clone fetches only branch heads, which is
enough because the base is an ancestor of the branch.

## Calibration notes (2026-10-16, on #1299 and #1300)

Both PRs passed Codacy and review. `-AllLines` went from 89 findings to 47
after tuning; the default scope reports 1 and 0.

Turned off as noise (all hits were deliberate code):
- clang-tidy `bugprone-implicit-widening-of-multiplication-result` (7, guest
  32-bit size math), `bugprone-switch-missing-default-case` (2) and
  `bugprone-branch-clone` (1) (register switch tables),
  `clang-analyzer-optin.performance.Padding` (1).
- Naming: no rule for function-local `constexpr`, and `k`-prefixed locals are
  allowed (both optional in Google style).
- cpplint `build/include_what_you_use` (14) and `build/namespaces` (8): xenia
  relies on transitive includes and `using namespace` throughout.
  `runtime/references`: Google style allows non-const reference parameters
  since 2020.

Made to match Codacy (evidence: Codacy's issue list for the repo):
- Flawfinder `memcpy` dropped: 300+ calls in `src/xenia`, 0 Codacy issues.
- cppcheck `syntaxError` dropped: every `*_test.cc` trips it on Catch2's
  `TEST_CASE`, 0 Codacy issues.
- cppcheck parses `.h` as C++ (`--language=c++`); as C it reported
  `syntaxError` on every `namespace xe {`.

Known difference from Codacy:
- cppcheck `unusedStructMember` on a header member used only from other files
  (`CommandProcessor::trace_frame_numbers_` in #1299, set in
  `command_processor.cc`, read in `pm4_command_processor_implement.h`). Codacy
  passed that commit, so it must run cppcheck differently there. Treat
  `unusedStructMember` on class members as a hint and check with grep.

## Tests

```bash
wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling && python3 -m unittest discover -s tests -t ."
```
