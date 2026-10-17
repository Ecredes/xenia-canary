import json
import os
import unittest

from fork_check.audit import (parse_osv, parse_submodule_status, parse_zizmor,
                              submodule_lines, zizmor_ok)
from fork_check.findings import CheckerBroken

DATA = os.path.join(os.path.dirname(__file__), "data")

GITMODULES = """\
[submodule "third_party/FFmpeg"]
\tpath = third_party/FFmpeg
\turl = https://github.com/has207/FFmpeg.git
[submodule "third_party/libusb"]
\tpath = third_party/libusb
\turl = https://github.com/libusb/libusb.git
[submodule "third_party/snappy"]
\tpath = third_party/snappy
\turl = https://github.com/google/snappy.git
"""

STATUS = """\
 d980192e175e6ff95bcd287af77e16fcb6597974 third_party/FFmpeg (d980192e17)
+a61afe5f75d969c4561a1d0ad753aa23cee6329a third_party/libusb (v1.0.29)
-6af9287fbdb913f0794d0148c6aa43b58e63c8e3 third_party/snappy
"""


def load(name):
    with open(os.path.join(DATA, name), encoding="utf-8") as f:
        return json.load(f)


class SubmoduleLinesTest(unittest.TestCase):
    def test_section_line_by_path(self):
        self.assertEqual(submodule_lines(GITMODULES), {
            "third_party/FFmpeg": 1, "third_party/libusb": 4,
            "third_party/snappy": 7})

    def test_path_differs_from_name(self):
        text = '[submodule "ffmpeg"]\n\tpath = third_party/FFmpeg\n'
        self.assertEqual(submodule_lines(text), {"third_party/FFmpeg": 1})


class SubmoduleStatusTest(unittest.TestCase):
    def test_sha_by_path_with_state_prefixes(self):
        status = parse_submodule_status(STATUS)
        self.assertEqual(status["third_party/FFmpeg"][0][:10], "d980192e17")
        self.assertEqual(status["third_party/libusb"][0][:10], "a61afe5f75")
        self.assertEqual(status["third_party/snappy"], (
            "6af9287fbdb913f0794d0148c6aa43b58e63c8e3", False))
        self.assertTrue(status["third_party/FFmpeg"][1])

    def test_unexpected_line_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_submodule_status("garbage\n")


class OsvTest(unittest.TestCase):
    def setUp(self):
        pinned = {p: sha for p, (sha, _) in parse_submodule_status(STATUS).items()}
        self.findings, self.skipped = parse_osv(
            load("osv.json"), "/scratch", submodule_lines(GITMODULES), pinned)

    def test_only_top_level_submodules_reported(self):
        self.assertEqual({x.message.split(":")[0] for x in self.findings},
                         {"third_party/FFmpeg", "third_party/libusb"})

    def test_nested_lockfiles_and_submodules_skipped(self):
        # SPIRV-Tools' yarn.lock and snappy's nested benchmark submodule.
        self.assertEqual(self.skipped, 2)

    def test_reported_on_gitmodules_section_line(self):
        lines = {x.message.split(":")[0]: x.line for x in self.findings}
        self.assertEqual(lines, {"third_party/FFmpeg": 1, "third_party/libusb": 4})
        self.assertTrue(all(x.path == ".gitmodules" and x.tool == "osv"
                            for x in self.findings))

    def test_one_finding_per_alias_group(self):
        libusb = sorted(x.rule for x in self.findings
                        if x.message.startswith("third_party/libusb"))
        self.assertEqual(libusb, ["CVE-2026-23679", "CVE-2026-47104"])

    def test_message_has_severity_and_both_commits(self):
        x = next(x for x in self.findings if x.rule == "CVE-2026-23679")
        self.assertIn("CVSS 6.9", x.message)
        self.assertIn("NULL Pointer Dereference", x.message)
        self.assertIn("pinned a61afe5f75", x.message)
        ffmpeg = next(x for x in self.findings
                      if x.message.startswith("third_party/FFmpeg"))
        self.assertIn("matched c1263a3dd9", ffmpeg.message)
        self.assertIn("pinned d980192e17", ffmpeg.message)

    def test_results_missing_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_osv({}, "/scratch", {}, {})


class ZizmorTest(unittest.TestCase):
    def test_primary_location_one_based(self):
        out = parse_zizmor(load("zizmor.json"))
        got = {(x.path, x.line, x.rule) for x in out}
        self.assertEqual(got, {
            (".github/workflows/Check_commit_message.yml", 12, "unpinned-uses"),
            (".github/workflows/Orchestrator.yml", 90, "secrets-inherit"),
            (".github/workflows/Create_release.yml", 34, "template-injection"),
        })

    def test_message_has_severity_and_annotation(self):
        x = next(x for x in parse_zizmor(load("zizmor.json"))
                 if x.rule == "unpinned-uses")
        self.assertTrue(x.message.startswith("High/High: "))
        self.assertIn("not pinned to a hash", x.message)

    def test_ignored_skipped(self):
        data = load("zizmor.json")
        data[0]["ignored"] = True
        self.assertEqual(len(parse_zizmor(data)), 2)

    def test_no_primary_location_is_broken(self):
        data = load("zizmor.json")
        for loc in data[0]["locations"]:
            loc["symbolic"]["kind"] = "Related"
        with self.assertRaises(CheckerBroken):
            parse_zizmor(data)

    def test_exit_codes(self):
        # 0: clean; 10-14: findings by highest severity; others: errors.
        self.assertTrue(all(zizmor_ok(c) for c in (0, 10, 11, 12, 13, 14)))
        self.assertFalse(any(zizmor_ok(c) for c in (1, 2, 3, 15)))


if __name__ == "__main__":
    unittest.main()
