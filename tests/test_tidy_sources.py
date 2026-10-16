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

    def test_relative_includes_resolve_against_the_source_dir(self):
        # Seen in upstream: command_processor.cc includes
        # "pm4_command_processor_implement.h" and d3d12/... includes "../...".
        files = {
            "src/xenia/gpu/d3d12/d3d12_cp.cc": '#include "../pm4_impl.h"\n',
            "src/xenia/gpu/cp.cc": '#include "pm4_impl.h"\n',
            "src/xenia/gpu/other.cc": '#include "unrelated/pm4_impl.h"\n',
        }
        db = ["src/xenia/gpu/other.cc", "src/xenia/gpu/d3d12/d3d12_cp.cc",
              "src/xenia/gpu/cp.cc"]
        self.assertEqual(
            choose_tidy_sources(["src/xenia/gpu/pm4_impl.h"], db, files.__getitem__),
            (["src/xenia/gpu/d3d12/d3d12_cp.cc"], []))

    def test_touched_source_with_relative_include_covers_header(self):
        files = {"src/xenia/gpu/cp.cc": '#include "pm4_impl.h"\n'}
        self.assertEqual(
            choose_tidy_sources(["src/xenia/gpu/cp.cc", "src/xenia/gpu/pm4_impl.h"],
                                ["src/xenia/gpu/cp.cc"], files.__getitem__),
            (["src/xenia/gpu/cp.cc"], []))

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
