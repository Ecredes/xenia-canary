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
