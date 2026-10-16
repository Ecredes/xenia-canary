import unittest

from fork_check.findings import Finding
from fork_check.runners import cppcheck_groups, drop_flawfinder_ignored


class CppcheckGroupsTest(unittest.TestCase):
    def test_headers_are_parsed_as_cpp(self):
        # Without --language=c++ cppcheck parses .h as C and reports
        # syntaxError on every `namespace xe {`.
        groups = cppcheck_groups(["src/a.h", "src/b.cc", "src/c.c", "src/d.inl"])
        self.assertEqual(groups, [([], ["src/c.c"]),
                                  (["--language=c++"], ["src/a.h", "src/b.cc", "src/d.inl"])])

    def test_empty_groups_dropped(self):
        self.assertEqual(cppcheck_groups(["src/b.cc"]), [(["--language=c++"], ["src/b.cc"])])


class FlawfinderIgnoredTest(unittest.TestCase):
    def test_memcpy_dropped(self):
        keep = Finding("a.cc", 1, "flawfinder", "strcpy", "m")
        drop = Finding("a.cc", 2, "flawfinder", "memcpy", "m")
        self.assertEqual(drop_flawfinder_ignored([keep, drop]), [keep])
