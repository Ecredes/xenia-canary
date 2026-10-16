# Fork check: local pre-flight analysis for upstream-bound branches

Date: 2026-10-15
Tracking issue: Ecredes/xenia-canary#10 (lane A; lane C reuses it)

## Goal

Before a topic branch goes upstream as a PR, run static analysis on the lines
it changes and report anything that Codacy would flag, plus the bugs and
style-guide violations that nothing upstream checks. Fork-only and local-only:
nothing is added to topic branches, and no GitHub configuration changes.

Later, the same tool scans the whole codebase (`-Full`) to produce a baseline
report for triage into fork issues (lane C).

## Background

- **Upstream enforces only clang-format** (`Lint.yml` runs `xb lint --all`).
  `docs/style_guide.md` names cpplint as "the source of truth" for the Google
  style, but cpplint was never wired up. Rules clang-format can't check: Google
  naming, TODO attribution (`TODO(name): foo.`), include order across groups
  (`.clang-format` has `IncludeBlocks: Preserve`), comment punctuation.
- **`xb tidy` is broken**: it passes `-p build/llvm_tools/debug_<platform>`, a
  premake-era path, and the CMake configure doesn't export
  `compile_commands.json`. Out of scope here.
- **Codacy** scans every upstream PR. Gate policy "Codacy Gate Policy" has
  `issueThreshold: 0`, so any new issue fails the check. Enabled C/C++ tools,
  all on default patterns: Cppcheck, Flawfinder, Opengrep, Lizard (complexity,
  not gated). Clang-Tidy is disabled (client-side only). `.codacy.yaml`
  excludes `tools/**`, `third_party/**`, `docs/**`, `android/**`, `**.md`,
  `src/xenia/gpu/shaders/**`. Source: Codacy public API v3, 2026-10-15
  (`/analysis/organizations/gh/xenia-canary/repositories/xenia-canary/tools`
  and `/issues/overview`). Per-tool pattern lists need authentication.

## Where it lives

- Orphan branch `tooling` on the fork, with no shared history with upstream.
  Pushed to `origin` as a backup; not the default branch.
- Checked out as a worktree at `C:\Users\Adam\AGENTS\xenia-canary-tooling`.
- Contents: the checker, its configs, its tests, this spec, a README.

## Interface

Run from the xenia-canary repo, on any branch:

```
..\xenia-canary-tooling\check.ps1 [-Base REF] [-AllLines | -Full] [-Tools cppcheck,flawfinder,clang-tidy,cpplint]
```

- `check.ps1` is a thin wrapper that runs `fork_check.py` in the
  `Ubuntu-24.04` WSL distro through a login shell, so the toolchain matches
  upstream CI (clang-20).
- **Base**: default `merge-base(upstream/canary_experimental, HEAD)`.
  `-Base` overrides it.
- **Scope**:
  - default: findings on lines added or modified since the base.
  - `-AllLines`: all findings in files touched since the base.
  - `-Full`: all findings in all scanned files (lane C).
- **Output**: one list sorted by file and line:
  `path:line: [tool:rule] message`, then a per-tool count.
- **Exit codes**: 0 = clean, 1 = findings, 2 = checker broken (missing tool,
  configure failure, tool crash, unparseable output).

## How it works

1. **Snapshot.** In WSL, a dedicated scratch clone (`~/src/xenia-fork-check`,
   never the normal `~/src/xenia-canary`) fetches the commit from the Windows
   repo (`/mnt/c/...`) and checks it out detached. Submodules are synced and
   updated there. Uncommitted changes are not analyzed; the tool says so if
   the Windows working tree is dirty.
2. **Changed lines.** `git diff -U0 <base> <commit>` gives, per file, the
   ranges of added/modified lines in the new version. Deleted files and
   non-C/C++ files are dropped. Paths excluded by upstream's `.codacy.yaml`
   are also dropped from every tool, so the scan set matches Codacy's.
3. **Compile database.** `cmake -G "Ninja Multi-Config"
   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` with clang-20, Debug config. Configure
   only, no build. Re-run only when a CMake file changed since the last run.
   Headers are analyzed through the `.cc` files that include them (clang-tidy
   `HeaderFilterRegex` restricted to `src/xenia/`).
4. **Tools**, each run on the touched files (or all files with `-Full`):

   | Lane | Tool | Configuration |
   |---|---|---|
   | Codacy mirror | cppcheck | `--enable=warning,style,performance`, no include paths (as Codacy runs it), `--template` with file, line, id, message |
   | Codacy mirror | Flawfinder | defaults, `--dataonly --singleline` |
   | Bugs | clang-tidy 20 | `bugprone-*`, `clang-analyzer-*` |
   | Style guide | clang-tidy 20 | `google-readability-todo`, `readability-identifier-naming` set to Google naming |
   | Style guide | cpplint | only categories clang-format doesn't cover (`build/include_order`, `build/include_what_you_use`, `readability/todo`, `runtime/*`, `build/namespaces`); line length, whitespace and indentation filtered out |

   Checks that turn out noisy during calibration are disabled in the config
   files with a comment giving the reason.
5. **Filter and report.** Each tool's output is parsed into
   `(path, line, tool, rule, message)`, paths are normalized to repo-relative,
   findings outside the scope are dropped, duplicates (same path, line and
   rule) are merged, and the rest is printed.

## Error handling

- Missing tool: print the exact install command (apt.llvm.org `clang-tidy-20`,
  `apt install cppcheck`, `pipx install cpplint flawfinder`) and exit 2. A tool
  is never skipped silently.
- Configure failure, tool crash, or output that can't be parsed: exit 2 with
  the tool's stderr. Never reported as a clean pass.
- clang-tidy compiler errors (`clang-diagnostic-error`) on a file mean the
  compile database is wrong for that file: reported as checker broken, not as
  findings.

## Testing

- **Unit tests** (`unittest`, no extra deps): diff → line ranges (adds,
  modifications, pure deletions, renames, new files); each tool's output parser
  against captured sample output; scope filtering; `.codacy.yaml` exclusions.
- **End-to-end**: a throwaway commit planting one known issue per tool (a
  cppcheck `nullPointer`, a Flawfinder `strcpy`, a clang-tidy `bugprone-*` hit,
  an unattributed TODO, a misnamed member) must flag each; the same run on an
  unchanged branch reports zero.
- **Calibration**: run on `gpu/release-trace-writer` and
  `testing/main-thread-asserts`, both of which passed Codacy and review.
  Anything firing on them is a candidate for tuning, with the decision
  recorded in the config.

## Out of scope

Fixing `xb tidy` upstream; updating the stale parts of `docs/style_guide.md`;
running in CI (possible later through a `workflow_run` workflow on a default
branch); a pre-push hook; Opengrep (its C rules mostly duplicate Flawfinder on
existing Codacy findings); Lizard; sanitizer and fuzzing lanes from #10.
