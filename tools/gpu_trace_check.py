#!/usr/bin/env python3
"""Compares two sets of frames produced by gpu_trace_run.py.

The baseline and test directories are usually the output of a baseline build
and a test build replaying the same traces, but they can also be two backends
replaying the same traces with the same build.

Frames are compared by their pixels if Pillow is installed (and a difference
image is saved for each changed frame), otherwise by the bytes of the PNG
files. A report.html showing changed frames side by side is written to the
test directory unless --report is given. The exit code is 0 if every frame
matches, and 1 otherwise.

Example:
  gpu_trace_check.py build/gputrace/baseline build/gputrace/test
"""

import argparse
import hashlib
import html
import json
import os
import pathlib
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    Image = None


def load_results(path):
    results_path = path / "results.json"
    if not results_path.is_file():
        raise SystemExit(f"ERROR: {results_path} not found; run "
                         "gpu_trace_run.py first.")
    with open(results_path, encoding="utf-8") as f:
        return json.load(f)["traces"]


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_frames(baseline_png, test_png, diff_png):
    """Returns None if the frames match, or a description of the difference."""
    if Image is None:
        if file_hash(baseline_png) == file_hash(test_png):
            return None
        return "file contents differ"
    with Image.open(baseline_png) as baseline, Image.open(test_png) as test:
        baseline = baseline.convert("RGB")
        test = test.convert("RGB")
        if baseline.size != test.size:
            return (f"size {baseline.size[0]}x{baseline.size[1]} -> "
                    f"{test.size[0]}x{test.size[1]}")
        diff = ImageChops.difference(baseline, test)
        if diff.getbbox() is None:
            return None
        changed = sum(1 for pixel in diff.getdata() if pixel != (0, 0, 0))
        max_delta = max(max(band) for band in diff.getextrema())
        # Make small differences visible.
        diff.point(lambda value: 255 if value else 0).save(diff_png)
        total = baseline.size[0] * baseline.size[1]
        return (f"{changed} of {total} pixels ({100 * changed / total:.2g}%), "
                f"max channel difference {max_delta}")


def main():
    parser = argparse.ArgumentParser(
        description="Compare frames produced by gpu_trace_run.py.")
    parser.add_argument("baseline", help="Baseline output directory.")
    parser.add_argument("test", help="Test output directory.")
    parser.add_argument("--report",
                        help="HTML report path (default: <test>/report.html).")
    args = parser.parse_args()

    baseline_path = pathlib.Path(args.baseline).resolve()
    test_path = pathlib.Path(args.test).resolve()
    report_path = pathlib.Path(args.report or test_path /
                               "report.html").resolve()
    diff_path = test_path / "diff"
    baseline_results = load_results(baseline_path)
    test_results = load_results(test_path)
    if Image is None:
        print("Pillow not found; comparing PNG files byte by byte without "
              "difference images (install with: python -m pip install "
              "Pillow).")

    rows = []
    matched = 0
    for name in sorted(set(baseline_results) | set(test_results)):
        baseline = baseline_results.get(name)
        test = test_results.get(name)
        baseline_png = (baseline_path / baseline["png"]
                        if baseline and baseline["png"] else None)
        test_png = test_path / test["png"] if test and test["png"] else None
        diff_png = None
        if baseline is None:
            problem = "only in test"
        elif test is None:
            problem = "only in baseline"
        elif baseline_png is None or test_png is None:
            problem = (f"replay status {baseline['status']} -> "
                       f"{test['status']}")
        else:
            diff_png = diff_path / (name + ".png")
            diff_png.parent.mkdir(parents=True, exist_ok=True)
            if diff_png.exists():
                diff_png.unlink()
            problem = compare_frames(baseline_png, test_png, diff_png)
            if problem is None:
                matched += 1
                continue
            if not diff_png.exists():
                diff_png = None
        print(f"  DIFF  {name}: {problem}")
        rows.append((name, problem, baseline_png, test_png, diff_png))

    total = matched + len(rows)
    print(f"{matched}/{total} frame(s) match.")

    def image_cell(path):
        if path is None:
            return "<td></td>"
        src = html.escape(
            pathlib.Path(os.path.relpath(path, report_path.parent)).as_posix())
        return f'<td><a href="{src}"><img src="{src}"></a></td>'

    with open(report_path, "w", encoding="utf-8") as f:
        f.write("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
                "<title>GPU trace comparison</title>\n<style>\n"
                "body { font-family: sans-serif; }\n"
                "img { width: 480px; }\n"
                "td { vertical-align: top; }\n"
                "</style></head><body>\n")
        f.write(f"<p>Baseline: {html.escape(str(baseline_path))}<br>\n"
                f"Test: {html.escape(str(test_path))}<br>\n"
                f"{matched} of {total} frame(s) match.</p>\n")
        if rows:
            f.write("<table>\n<tr><th>Baseline</th><th>Test</th>"
                    "<th>Changed pixels</th></tr>\n")
            for name, problem, baseline_png, test_png, diff_png in rows:
                f.write(f"<tr><td colspan=\"3\"><b>{html.escape(name)}</b>: "
                        f"{html.escape(problem)}</td></tr>\n<tr>")
                for path in (baseline_png, test_png, diff_png):
                    f.write(image_cell(path))
                f.write("</tr>\n")
            f.write("</table>\n")
        f.write("</body></html>\n")
    print(f"Report: {report_path}")
    return 1 if rows else 0


if __name__ == "__main__":
    sys.exit(main())
