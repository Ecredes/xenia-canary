#!/usr/bin/env python3
"""Replays GPU traces through a trace dump tool and saves the frames.

Every .xtr file under the trace path is played back with one of the
xenia-gpu-*-trace-dump executables. The captured guest output of each trace is
written to <output_path>/<trace path relative to the trace root>.png, next to
the tool log of that trace, and results.json in the output path records the
outcome of every trace.

Run this once with a baseline build and once with a test build, then compare
the two output directories with gpu_trace_check.py.

Example:
  gpu_trace_run.py -x build/bin/Windows/Release/xenia-gpu-vulkan-trace-dump.exe
      -t traces -o build/gputrace/test
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys
import time


def find_traces(trace_path):
    trace_path = pathlib.Path(trace_path)
    if trace_path.is_file():
        return trace_path.parent, [trace_path]
    return trace_path, sorted(trace_path.rglob("*.xtr"))


def run_trace(executable, trace_root, trace, output_path, timeout, extra_args):
    relative = trace.relative_to(trace_root)
    name = relative.with_suffix("").as_posix()
    output_dir = output_path / relative.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    png_path = output_dir / (trace.stem + ".png")
    log_path = output_dir / (trace.stem + ".log")
    for path in (png_path, log_path):
        if path.exists():
            path.unlink()

    args = [
        str(executable),
        f"--target_trace_file={trace}",
        f"--trace_dump_path={output_dir}",
        f"--log_file={log_path}",
    ] + extra_args

    creationflags = 0
    if os.name == "nt":
        creationflags = subprocess.CREATE_NO_WINDOW
    start = time.monotonic()
    try:
        exit_code = subprocess.run(
            args,
            cwd=executable.parent,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=creationflags,
            timeout=timeout).returncode
        timed_out = False
    except subprocess.TimeoutExpired:
        exit_code = None
        timed_out = True
    seconds = time.monotonic() - start

    if timed_out:
        status = "timeout"
    elif exit_code == 0 and png_path.exists():
        status = "ok"
    else:
        status = "failed"
    return name, {
        "status": status,
        "exit_code": exit_code,
        "seconds": round(seconds, 2),
        "png": png_path.relative_to(output_path).as_posix()
        if png_path.exists() else None,
        "log": log_path.relative_to(output_path).as_posix(),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Replay GPU traces and save the captured frames.",
        epilog="Arguments after -- are passed to the trace dump tool.")
    parser.add_argument("-x", "--executable", required=True,
                        help="xenia-gpu-*-trace-dump executable to use.")
    parser.add_argument("-t", "--trace_path", required=True,
                        help="Trace file, or directory searched for .xtr "
                        "files recursively.")
    parser.add_argument("-o", "--output_path", required=True,
                        help="Directory to write frames, logs and "
                        "results.json to.")
    parser.add_argument("-j", "--jobs", type=int,
                        default=max(1, (os.cpu_count() or 2) // 2),
                        help="Number of traces to replay at once.")
    parser.add_argument("--timeout", type=float, default=120,
                        help="Seconds before a replay is abandoned.")
    args, extra_args = parser.parse_known_args()
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]

    executable = pathlib.Path(args.executable).resolve()
    if not executable.is_file():
        print(f"ERROR: {executable} not found, ensure it is built.")
        return 1
    trace_root, traces = find_traces(pathlib.Path(args.trace_path).resolve())
    if not traces:
        print(f"ERROR: no .xtr traces found in {args.trace_path}.")
        return 1
    output_path = pathlib.Path(args.output_path).resolve()
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"Replaying {len(traces)} trace(s) with {executable.name} "
          f"({args.jobs} at a time)...")
    results = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [
            pool.submit(run_trace, executable, trace_root, trace, output_path,
                        args.timeout, extra_args) for trace in traces
        ]
        for future in concurrent.futures.as_completed(futures):
            name, result = future.result()
            results[name] = result
            print(f"  {result['status']:7} {result['seconds']:6.2f}s  {name}")

    with open(output_path / "results.json", "w", encoding="utf-8") as f:
        json.dump(
            {
                "executable": str(executable),
                "extra_args": extra_args,
                "traces": dict(sorted(results.items())),
            },
            f,
            indent=2)

    failed = [name for name, r in results.items() if r["status"] != "ok"]
    print(f"{len(results) - len(failed)}/{len(results)} trace(s) replayed.")
    for name in sorted(failed):
        print(f"  {results[name]['status']}: {name} "
              f"(see {results[name]['log']})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
