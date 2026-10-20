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

clang-tidy analyzes a touched header through a compiled file that includes
it, directly or through other headers; headers only Windows, D3D12, a64 or
Android code includes can't be reached from the Linux build and get a note.
It runs one job per CPU, capped at one per 1.5 GiB of available memory.

The configure re-runs only when CMake files, the list of tracked source files
(CMake uses `file(GLOB)`) or submodule commits change. Typical timings: first
run ~4 min (submodule clone), later runs ~10–30 s; `-Full` ~30 min
(914 files).

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
(objects missing); the scratch clone fetches only branch heads and `HEAD`
(so a detached HEAD works), which is enough because the base is an ancestor
of HEAD. A `-Base` outside that history exits 2.

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

## Dependency and workflow audit

A whole-repo check, separate from the per-branch fork check:

```powershell
..\xenia-canary-tooling\audit.ps1                                # at HEAD
..\xenia-canary-tooling\audit.ps1 -Ref upstream/canary_experimental
```

Same report format and exit codes. It checks out the commit in the scratch
clone, including the submodules the Linux build skips (shallow, not
recursive), then runs:

| Tool | Checks | Reported at |
|---|---|---|
| osv-scanner 2.6.0 | known vulnerabilities (OSV database) in the top-level `third_party` submodules | the submodule's `[submodule ...]` line in `.gitmodules` |
| zizmor 1.30.1 (`--offline`) | GitHub Actions problems: unpinned actions, template injection, permissions, inherited secrets | the workflow line |

Reading the osv results:
- The submodule clones are shallow, so osv-scanner estimates the closest known
  commit from file contents. Each message gives that `matched` commit and the
  `pinned` one. When they differ, check whether the fix is already in the
  pinned commit; rapidjson matched its 2016 v1.1.0 release though the pinned
  commit already has the CVE-2024-38517 fix.
- A CVE in a component xenia doesn't build is not reachable. Canary compiles a
  small part of FFmpeg (see the FFmpeg section of `third_party/CMakeLists.txt`).
- Developer lockfiles and nested submodules inside a submodule (yarn.lock,
  requirements.txt, snappy's benchmark) are skipped and counted in a note.

zizmor's online audits (known-vulnerable-actions, impostor-commit) need a
GitHub token and are not run.

Install (one time, in Ubuntu-24.04). Both are pinned to releases at least two
weeks old when added:

```bash
v=v2.6.0; d=$(mktemp -d); cd "$d"
curl -sSfLO "https://github.com/google/osv-scanner/releases/download/$v/osv-scanner_linux_amd64"
curl -sSfLO "https://github.com/google/osv-scanner/releases/download/$v/osv-scanner_SHA256SUMS"
grep ' osv-scanner_linux_amd64$' osv-scanner_SHA256SUMS | sha256sum -c -
install -m755 osv-scanner_linux_amd64 ~/.local/bin/osv-scanner
pipx install zizmor==1.30.1
```

Run these in a WSL shell, not through `wsl -- bash -c "..."` from Windows,
which expands `$`.

## A/B test builds

Builds a fix branch and its upstream base side by side, each with
`testing/telemetry` merged in, so an A/B run in game-mcp gets the same live
signals (state, fps, problems with vblank stamps) on both sides
(Ecredes/xenia-canary#31). From the xenia-canary repo, in PowerShell:

```powershell
..\xenia-canary-tooling\ab-build.ps1 gpu/vulkan-edram-compute-modified     # prepare, check, build both
..\xenia-canary-tooling\ab-build.ps1 <fix> -NoBuild                        # prepare and check only
..\xenia-canary-tooling\ab-build.ps1 <fix> -Reuse                          # after resolving a merge conflict
..\xenia-canary-tooling\ab-build.ps1 <fix> -Clean                          # remove the pair
```

1. The base is `git merge-base <fix> upstream/canary_experimental` (or `-Base`).
2. Worktrees `.claude/worktrees/ab-<name>-{base,fix}` on local branches
   `ab/<name>/{base,fix}`, each with a `--no-ff` merge of the telemetry branch
   (`-Telemetry`, default `origin/testing/telemetry`), with `rerere` on so a
   conflict resolved on one side resolves the same way on the other.
3. Isolation check: the diff between the two A/B branches must equal the fix's
   own diff (hunk positions ignored, since telemetry can shift them). If
   telemetry edits the fix's hunks, an A/B difference can't be attributed to
   the fix alone; exit 1 unless `-AllowOverlap`.
4. `xb setup` and `xb build --config=release --target=xenia-app` in each,
   with identical options (the second side mostly hits ccache), then checks
   that each exe has `--telemetry_file`.

It prints both exe paths for `game_launch(plugin="xenia", exe=..., path=...)`;
game-mcp turns telemetry on by itself. The `ab/*` branches are throwaway:
never push them, and never use them for a PR (the PR is the plain fix
branch). Native input needs `hid/remote-input` in the merge too, which
conflicts with telemetry until #23 lands; use keyboard or ViGEm meanwhile.

| Exit | Meaning |
|---|---|
| 0 | pair ready (or `-NoBuild` and the check passed) |
| 1 | telemetry overlaps the fix's diff |
| 2 | broken (merge conflict, build failure, git error) |

## Integration build

One build with every pending fork branch: `integration` is regenerated from
the latest `upstream/canary_experimental` with each branch in
`config/integration.txt` merged in, in order. Use it to play and explore
titles past bugs we've already fixed, to catch fixes that conflict with each
other before upstream does, and to run the regression set across all of our
work. From the xenia-canary repo, in PowerShell:

```powershell
..\xenia-canary-tooling\integrate.ps1               # fetch, regenerate, build
..\xenia-canary-tooling\integrate.ps1 -NoBuild      # merge only
..\xenia-canary-tooling\integrate.ps1 -Resume       # after resolving a conflict
..\xenia-canary-tooling\integrate.ps1 -Push         # also force-push it for a Fork CI build
..\xenia-canary-tooling\integrate.ps1 -Clean        # remove the worktree and branch
```

1. Fetches the remotes named in the list, then resets the worktree
   `.claude/worktrees/integration` (branch `integration`) to upstream.
2. Merges each listed ref with `--no-ff`. A branch whose commits are all
   upstream already is skipped and reported: drop it from the list. A branch
   inside one listed earlier (a stack step) is reported as already in.
3. On a conflict it stops (exit 1). Resolve and `git add` the files in the
   worktree, without committing, and rerun with `-Resume`: it commits the
   merge with rerere on and carries on. Every later regeneration replays the
   recorded resolution by itself ("conflicts replayed by rerere").
4. Builds `xenia-app` (release) and prints the exe path for `game_launch`.

Rules:

- **Never cut a branch from `integration`** or base a PR on it. A fix built
  there can silently depend on another unmerged fix, so it won't stand alone
  upstream. Fixes start from `canary_experimental` as always.
- **A/B stays against upstream** (`ab-build.ps1`). The integration build is
  for finding bugs and checking combinations, not for attributing a change.
- **Fix bugs on topic branches**, not in conflict resolutions. A resolution
  only combines two branches; if one of them is wrong, fix it there and
  regenerate (rerere still replays the rest).
- A topic branch that changes where a recorded resolution applies makes the
  merge conflict again; resolve it once more.

Resolutions live in the main checkout's `.git/rr-cache` (kept 60 days
unused, 15 days unresolved by default; `git config gc.rerereResolved` to
change). If it's lost, the next regeneration stops at each conflict again.

| Exit | Meaning |
|---|---|
| 0 | ready (built, or merged with `-NoBuild`) |
| 1 | merge conflict to resolve, then `-Resume` |
| 2 | broken (git error, unfinished resolution, build failure) |

## Tests

```bash
wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Users/Adam/AGENTS/xenia-canary-tooling && python3 -m unittest discover -s tests -t ."
```

## Debugging game compatibility

`docs/compat-debugging.md` is the workflow for turning a compat finding into
an upstream fix: verified routes, stall classification, Ghidra on the
caller, edge comparison, minimal fixes. Ghidra scripts are in `re/ghidra`
(`FixSaveRestGpr`, `DecompileAt`, `DecompileCallers`); `re/tools` has an
XDVDFS extractor and Capstone helpers.
