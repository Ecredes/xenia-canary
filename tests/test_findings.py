import unittest

from fork_check.findings import Finding, format_report, select


def f(path="a.cc", line=1, tool="cppcheck", rule="r", message="m"):
    return Finding(path, line, tool, rule, message)


class SelectTest(unittest.TestCase):
    def test_changed_scope_keeps_only_changed_lines(self):
        changed = {"a.cc": {2, 3}}
        out = select([f(line=1), f(line=2), f(path="b.cc", line=2)], "changed", changed)
        self.assertEqual(out, [f(line=2)])

    def test_files_scope_keeps_all_lines_of_touched_files(self):
        changed = {"a.cc": {2}}
        out = select([f(line=1), f(line=9), f(path="b.cc")], "files", changed)
        self.assertEqual([x.line for x in out], [1, 9])

    def test_files_scope_includes_touched_file_with_no_added_lines(self):
        out = select([f(line=5)], "files", {"a.cc": set()})
        self.assertEqual(len(out), 1)

    def test_full_scope_keeps_everything(self):
        out = select([f(path="z.cc"), f(path="b.cc")], "full", {})
        self.assertEqual([x.path for x in out], ["b.cc", "z.cc"])

    def test_dedupes_same_path_line_tool_rule(self):
        out = select([f(message="x"), f(message="y")], "full", {})
        self.assertEqual(len(out), 1)

    def test_same_line_different_rule_kept(self):
        out = select([f(rule="a"), f(rule="b")], "full", {})
        self.assertEqual(len(out), 2)

    def test_sorted_by_path_then_line(self):
        out = select([f(path="b.cc", line=1), f(path="a.cc", line=10),
                      f(path="a.cc", line=2)], "full", {})
        self.assertEqual([(x.path, x.line) for x in out],
                         [("a.cc", 2), ("a.cc", 10), ("b.cc", 1)])

    def test_unknown_scope_raises(self):
        with self.assertRaises(ValueError):
            select([f()], "bogus", {})


class FormatTest(unittest.TestCase):
    def test_report_lines_and_counts(self):
        text = format_report([f(line=3, rule="nullPointer", message="Null"),
                              f(path="b.cc", tool="cpplint", rule="readability/todo",
                                message="Missing username")])
        self.assertEqual(text.splitlines(), [
            "a.cc:3: [cppcheck:nullPointer] Null",
            "b.cc:1: [cpplint:readability/todo] Missing username",
            "-- 2 finding(s) (cppcheck: 1, cpplint: 1)",
        ])

    def test_empty_report(self):
        self.assertEqual(format_report([]), "-- no findings")
