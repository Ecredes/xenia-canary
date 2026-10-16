import os
import re
import unittest

from fork_check.findings import CheckerBroken
from fork_check.parsers import (parse_clang_tidy, parse_cppcheck, parse_cpplint,
                                parse_flawfinder, rel)

DATA = os.path.join(os.path.dirname(__file__), "data")
PLANTED = "src/xenia/base/planted.cc"


def sample(name):
    with open(os.path.join(DATA, name), encoding="utf-8") as f:
        return f.read()


def tidy_root():
    """The scratch root the clang-tidy sample was captured under."""
    m = re.search(r"^(/\S+?)/src/xenia/base/planted\.cc:", sample("clang_tidy.txt"), re.M)
    return m.group(1)


class RelTest(unittest.TestCase):
    def test_relative_path_kept(self):
        self.assertEqual(rel("src/a.cc", "/root"), "src/a.cc")

    def test_dot_slash_stripped(self):
        self.assertEqual(rel("./src/a.cc", "/root"), "src/a.cc")

    def test_absolute_under_root(self):
        self.assertEqual(rel("/root/src/a.cc", "/root"), "src/a.cc")

    def test_rel_outside_root_is_none(self):
        self.assertIsNone(rel("/usr/include/c++/13/vector", "/root"))

    def test_root_prefix_is_not_a_directory_match(self):
        self.assertIsNone(rel("/root2/src/a.cc", "/root"))

    def test_dotdot_normalized(self):
        self.assertEqual(rel("/root/src/x/../a.cc", "/root"), "src/a.cc")


class CppcheckTest(unittest.TestCase):
    def test_sample(self):
        out = parse_cppcheck(sample("cppcheck.txt"), "/unused")
        self.assertTrue(out)
        self.assertTrue(all(x.path == PLANTED and x.tool == "cppcheck" for x in out))
        self.assertIn("nullPointer", {x.rule for x in out})

    def test_information_lines_ignored(self):
        text = ("nofile:0:0: information:checkersReport: Active checkers: 1\n"
                "src/a.cc:3:1: information:missingIncludeSystem: Include not found\n")
        self.assertEqual(parse_cppcheck(text, "/r"), [])

    def test_line_fields(self):
        out = parse_cppcheck("src/a.cc:12:5: warning:nullPointer: Null pointer dereference: p\n", "/r")
        self.assertEqual(len(out), 1)
        self.assertEqual((out[0].path, out[0].line, out[0].rule, out[0].message),
                         ("src/a.cc", 12, "nullPointer", "Null pointer dereference: p"))

    def test_unparseable_line_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_cppcheck("cppcheck: error: unrecognized command line option\n", "/r")


class FlawfinderTest(unittest.TestCase):
    def test_sample(self):
        out = parse_flawfinder(sample("flawfinder.txt"), "/unused")
        self.assertIn("strcpy", {x.rule for x in out})
        self.assertTrue(all(x.path == PLANTED for x in out))

    def test_level_in_message(self):
        line = ("src/a.cc:7:  [4] (buffer) strcpy:Does not check for buffer "
                "overflows when copying to destination (CWE-120). \n")
        out = parse_flawfinder(line, "/r")
        self.assertEqual((out[0].line, out[0].rule), (7, "strcpy"))
        self.assertEqual(out[0].message,
                         "[4] (buffer) Does not check for buffer overflows when "
                         "copying to destination (CWE-120).")

    def test_column_accepted(self):
        out = parse_flawfinder("src/a.cc:7:3:  [1] (buffer) strlen:Does not handle\n", "/r")
        self.assertEqual((out[0].line, out[0].rule), (7, "strlen"))

    def test_unparseable_line_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_flawfinder("Traceback (most recent call last):\n", "/r")


class ClangTidyTest(unittest.TestCase):
    def test_sample(self):
        out = parse_clang_tidy(sample("clang_tidy.txt"), tidy_root())
        rules = {x.rule for x in out}
        self.assertIn("clang-analyzer-core.NullDereference", rules)
        self.assertIn("readability-identifier-naming", rules)
        self.assertIn("google-readability-todo", rules)
        self.assertIn("bugprone-assignment-in-if-condition", rules)
        self.assertTrue(all(x.path == PLANTED and x.tool == "clang-tidy" for x in out))

    def test_notes_and_noise_ignored(self):
        text = ("/r/src/a.cc:5:3: note: Null pointer value stored to 'p'\n"
                "   5 |   int* p = nullptr;\n"
                "      |   ^\n"
                "12 warnings generated.\n"
                "Suppressed 11 warnings (11 in non-user code).\n")
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_multiple_checks_on_one_warning(self):
        text = "/r/src/a.cc:5:3: warning: thing [bugprone-a,bugprone-b]\n"
        self.assertEqual({x.rule for x in parse_clang_tidy(text, "/r")},
                         {"bugprone-a", "bugprone-b"})

    def test_clang_tidy_drops_outside_root(self):
        text = "/usr/include/c++/13/bits/a.h:5:3: warning: thing [bugprone-a]\n"
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_compiler_warnings_dropped(self):
        text = "/r/src/a.cc:5:3: warning: unused variable 'x' [clang-diagnostic-unused-variable]\n"
        self.assertEqual(parse_clang_tidy(text, "/r"), [])

    def test_compile_error_is_broken(self):
        text = "/r/src/a.cc:1:10: error: 'missing.h' file not found [clang-diagnostic-error]\n"
        with self.assertRaises(CheckerBroken):
            parse_clang_tidy(text, "/r")

    def test_error_without_check_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_clang_tidy("Error while processing /r/src/a.cc.\n", "/r")


class CpplintTest(unittest.TestCase):
    def test_sample(self):
        out = parse_cpplint(sample("cpplint.txt"), "/unused")
        self.assertIn("readability/todo", {x.rule for x in out})
        self.assertTrue(all(x.path == PLANTED for x in out))

    def test_line_fields(self):
        text = "src/a.cc:7:  Missing username in TODO; it should look like \"// TODO(my_username): Stuff.\"  [readability/todo] [2]\n"
        out = parse_cpplint(text, "/r")
        self.assertEqual((out[0].line, out[0].rule), (7, "readability/todo"))
        self.assertTrue(out[0].message.startswith("Missing username in TODO"))

    def test_summary_lines_ignored(self):
        text = "Done processing src/a.cc\nTotal errors found: 0\n"
        self.assertEqual(parse_cpplint(text, "/r"), [])

    def test_skipped_input_is_broken(self):
        with self.assertRaises(CheckerBroken):
            parse_cpplint("Skipping input 'src/a.cc': Can't open for reading\n", "/r")
