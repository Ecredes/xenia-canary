# Telemetry

Xenia can write a stream of JSON lines describing what the emulator is doing:
its state, frame pacing, and (as the format grows) compatibility events such as
missing imports or failed file opens. Tools read this file to tell whether a
title boots, runs, stalls or crashes without scraping the log.

Telemetry is **output only** and **off by default**. Xenia never reads from the
telemetry file and there is no input or control channel. With no file set, the
hooks cost one relaxed atomic load each.

## Settings

The settings are in the `Telemetry` section of the config, or on the command
line:

| cvar | Default | Meaning |
|---|---|---|
| `telemetry_file` | empty (off) | Path of the JSONL file to write. Truncated when opened. Other processes can read it while it grows. |
| `telemetry_window` | `60` | Vblanks per `frame_stats` event. |
| `telemetry_exports` | `stub` | Exports whose first call is reported by `export_first_call`: `stub` (stubbed and sketchy exports) or `all`. |

Example:

```
xenia_canary.exe --telemetry_file=C:\temp\run.jsonl path\to\title.iso
```

## Envelope

One JSON object per line, UTF-8, `\n` terminated. Every line starts with these
fields, in this order:

| Field | Type | Meaning |
|---|---|---|
| `type` | string | Event type, below |
| `vblank` | int | Guest vblank count when the event was recorded |
| `swap` | int | Guest swap (presented frame) count at that moment |
| `ns` | int | Host `std::chrono::steady_clock` time in ns |
| `state` | string | Emulator state when the event was recorded (see below) |

Event fields follow the envelope.

## Emulator state

Every line carries the emulator state it was written in, so a reader that starts
mid-stream (a late reader, or a file cut short by a crash) knows the state from
any single line.

```
starting ──► loading ──► running ◄──► paused
               │           │  ▲
               │           ▼  │
               │        system_ui
               ▼           ▼
            crashed      exited
```

| State | Entered when |
|---|---|
| `starting` | telemetry opened, no title yet |
| `loading` | a title launched, no swap since |
| `running` | first swap after `loading`, resume from `paused`, or a system UI closed |
| `system_ui` | a system (XAM) UI is shown; left when the UI closes |
| `paused` | the emulator is paused (save/restore state, or a debugger break); resume returns to the state it left |
| `crashed` | guest crash (terminal) |
| `exited` | telemetry closing (terminal) |

Transition rules (the `reason` written with each transition is in parentheses):

- `crashed` accepts only exit; `exited` accepts nothing. Anything else is
  ignored and writes no line.
- A title launch moves any non-terminal state to `loading` (`title`),
  including a launch from another title. If a pause is still in effect (a
  debugger break at start), `pause` follows at once.
- The first swap moves `loading` to `running` (`first_swap`).
- Pause moves any non-terminal, non-paused state to `paused` (`pause`); resume
  returns to the state it left (`resume`). The emulator (save/restore state)
  and the debugger pause independently: the state stays `paused` until both
  have resumed.
- A system UI moves `loading` or `running` to `system_ui` (`xam_ui`); closing
  it returns to the state it left (`xam_ui_closed`). While `paused`, a UI
  opening or closing only changes the state that resume returns to.
- A guest crash moves any non-terminal state to `crashed` (`guest_crash`).
- Exit moves any state but `exited` to `exited`. Its reason is the exit
  reason: `shutdown`, `title_exit` (the title was closed from the menu and no
  title launched since) or `crash` (a guest crash; closing the crashed title
  keeps `crash`).

Xenia reports only states it enters directly. It never writes thresholds or
judgments such as "hung" or "no picture"; those are policy, left to readers.

## Header and privacy

The first line is always `session_start`. It describes the build, the host and
the settings that affect results, so a file can be compared with others.

It contains no host name, user name, serials or MAC addresses, and no
path-valued settings, because telemetry files are meant to be shared in
reports.

## Events

| type | When | Fields |
|---|---|---|
| `session_start` | telemetry opened; always the first line | `format` (int, currently 1), `xenia` (object: `branch`, `commit`, `commit_short`, `date`, `config` `debug`/`checked`/`release`), `host` (object, below), `cvars` (object: `gpu`, `vsync`, `framerate_limit`, `draw_resolution_scale_x`, `draw_resolution_scale_y`, `telemetry_window`), `wall_time` (ISO 8601, UTC) |
| `state` | the emulator state changes | `from`, `to`, `reason` (see above). The envelope's `state` is already `to` |
| `title` | a title launches (including a launch from another title) | `title_id` (hex string), `media_id` (hex string), `version`, `disc` (`number/count`), `module` (executable file name). `media_id` and `disc` are omitted when the executable has no execution info |
| `export_first_call` | the first call of a kernel export selected by `telemetry_exports` | `library` (`xboxkrnl`, `xam`, `xbdm`), `ordinal`, `name`, `tag` (`stub`, `sketchy` or `implemented`; stub wins over sketchy), `caller` (hex guest address of the call site), `thread` (guest thread id). At most one per export per session |
| `import_missing` | a title's import table names a kernel export Xenia does not implement: an unknown ordinal, a known function with no implementation, or a known variable with no value. Written when the module is loaded, which is before the `title` line (state `starting`) | `module` (the importing module), `library` (the import library, e.g. `xboxkrnl.exe`), `ordinal`, `name` (the export name when known, else a generated `__<library>_<ordinal in hex>` name). At most one per module, library and ordinal |
| `extern_undefined_call` | a guest call reaches a function with no implementation behind it (usually a missing import) | `address` (hex guest address), `name`. At most one per address. Written at run time, on the calling thread. With `ignore_undefined_externs=false` Xenia then stops with a fatal error: this line is flushed first, and the file ends with exit reason `shutdown` and no `crash` line |
| `unimplemented_instruction` | the JIT meets a PowerPC instruction Xenia does not implement | `address` (hex), `opcode` (hex instruction word), `name` (disassembly mnemonic). At most one per address. Written at JIT translation time, which can be before the instruction ever runs, or without it running at all |
| `frame_stats` | every `telemetry_window` vblanks | see below |
| `exit` | telemetry closing, followed by `state` → `exited` | `reason` (`shutdown`, `title_exit`, `crash`), `vblank_total`, `swap_total`, `export_calls` (object mapping `library::name` to the number of calls in the session, for every export called at least once) |

`host` in `session_start`: `os` (`windows`, `linux`), `os_version` (for
example `10.0.26200`, or the kernel release on Linux), `arch` (`x86_64`,
`aarch64`), `cpu` (brand string from CPUID, or `/proc/cpuinfo` on aarch64),
`cpu_cores` (physical), `cpu_threads` (logical), `ram_mb` (physical total).

`frame_stats` fields:

| Field | Meaning |
|---|---|
| `window_vblanks` | vblanks in this window (`telemetry_window`) |
| `swaps` | swaps in this window |
| `swap_ms` | swap-to-swap intervals in this window, ms, 2 decimals. An interval belongs to the window of its later swap. The first swap after start or a pause has no interval |
| `shader_compiles`, `pipeline_compiles` | compiled in this window |
| `file_read_bytes` | guest file bytes read in this window |
| `input_polls` | guest controller polls in this window |
| `guest_threads` | guest threads alive now (threads running guest code; emulator-internal host threads are not counted) |
| `blocked_threads` | guest threads inside a kernel wait now |
| `sleeping_threads` | guest threads inside a delay (sleep) now |
| `audio_frames` | audio frames submitted to the host driver in this window |
| `audio_dropped` | audio frames dropped because the host queue was full |
| `audio_peak_db` | peak absolute sample in the window as dBFS, 1 decimal; `null` when silent |

The thread gauges are current values, not per-window counts. `blocked_threads`
never exceeds `guest_threads`. A title that makes no progress (`swaps` stays 0
in the `running` state) with `blocked_threads` equal to `guest_threads` and
`sleeping_threads` 0 has every guest thread waiting on another, which usually
means a deadlock. Threads in a delay are counted in `sleeping_threads`, not in
`blocked_threads`.

Counters that are not hooked up yet report 0. Guest memory use is not reported
in this version.

`crash` and `exit` lines, and the `state` lines into `crashed` and `exited`,
are written and flushed before Xenia continues, since the process may be about
to end. Other lines are flushed at least every 100 ms.

## Compatibility rules

- Readers must ignore fields and event types they do not know. Adding fields
  or event types does not change `format`.
- Renaming or removing a field or event type bumps `format` in
  `session_start`.

## Clock

`ns` is `std::chrono::steady_clock`. On Windows that reads
QueryPerformanceCounter, on Linux `CLOCK_MONOTONIC`, the same clocks Python's
`time.perf_counter_ns()` reads, so a tool on the same machine can line events
up with its own recordings without estimating an offset. A unit test checks
this on Windows.

## Planned events

The format is growing. These events are planned and not written yet:

- `gpu`: the graphics backend and adapter.
- `input_first_poll`: the first controller poll per user index.
- `file_open_failed`, `file_open`: guest file opens.
- `xam_ui`: a system UI is shown.
- `shader_failed`: a shader that failed to translate or compile.
- `xma_error`, `audio_client`: audio decoding and host audio driver events.
- `gpu_error`: command stream errors.
- `crash`: guest crash details, followed by `state` → `crashed`.
