import unittest

from fork_check.diffparse import parse_changed_lines

MODIFY = """\
diff --git a/src/a.cc b/src/a.cc
index 1111111..2222222 100644
--- a/src/a.cc
+++ b/src/a.cc
@@ -10,0 +11,2 @@ void F() {
+  int x = 1;
+  int y = 2;
@@ -20 +22 @@ void G() {
-  old();
+  new_call();
"""

NEW_FILE = """\
diff --git a/src/new.h b/src/new.h
new file mode 100644
index 0000000..3333333
--- /dev/null
+++ b/src/new.h
@@ -0,0 +1,3 @@
+#pragma once
+
+int F();
"""

DELETED = """\
diff --git a/src/gone.cc b/src/gone.cc
deleted file mode 100644
index 4444444..0000000
--- a/src/gone.cc
+++ /dev/null
@@ -1,2 +0,0 @@
-int a;
-int b;
"""

PURE_DELETION_HUNK = """\
diff --git a/src/a.cc b/src/a.cc
index 1111111..2222222 100644
--- a/src/a.cc
+++ b/src/a.cc
@@ -5,2 +4,0 @@ void F() {
-  a();
-  b();
"""

RENAME_ONLY = """\
diff --git a/src/old.cc b/src/renamed.cc
similarity index 100%
rename from src/old.cc
rename to src/renamed.cc
"""

RENAME_WITH_EDIT = """\
diff --git a/src/old.cc b/src/renamed.cc
similarity index 90%
rename from src/old.cc
rename to src/renamed.cc
index 1111111..2222222 100644
--- a/src/old.cc
+++ b/src/renamed.cc
@@ -3 +3 @@
-int a;
+int b;
"""

BINARY = """\
diff --git a/assets/icon.png b/assets/icon.png
index 1111111..2222222 100644
Binary files a/assets/icon.png and b/assets/icon.png differ
"""


class ParseChangedLinesTest(unittest.TestCase):
    def test_modified_file(self):
        self.assertEqual(parse_changed_lines(MODIFY), {"src/a.cc": {11, 12, 22}})

    def test_new_file(self):
        self.assertEqual(parse_changed_lines(NEW_FILE), {"src/new.h": {1, 2, 3}})

    def test_deleted_file_absent(self):
        self.assertEqual(parse_changed_lines(DELETED), {})

    def test_pure_deletion_hunk_adds_no_lines(self):
        self.assertEqual(parse_changed_lines(PURE_DELETION_HUNK), {"src/a.cc": set()})

    def test_pure_rename_absent(self):
        self.assertEqual(parse_changed_lines(RENAME_ONLY), {})

    def test_rename_with_edit_uses_new_path(self):
        self.assertEqual(parse_changed_lines(RENAME_WITH_EDIT), {"src/renamed.cc": {3}})

    def test_binary_file_absent(self):
        self.assertEqual(parse_changed_lines(BINARY), {})

    def test_multiple_files(self):
        out = parse_changed_lines(MODIFY + NEW_FILE + DELETED + BINARY)
        self.assertEqual(set(out), {"src/a.cc", "src/new.h"})

    def test_empty_diff(self):
        self.assertEqual(parse_changed_lines(""), {})

    def test_path_with_space_drops_trailing_tab(self):
        # Real git output: a path containing a space gets a trailing tab.
        diff = "+++ b/src/a b/c d.cc\t\n@@ -0,0 +1 @@\n+x\n"
        self.assertEqual(parse_changed_lines(diff), {"src/a b/c d.cc": {1}})

    def test_c_quoted_path_unquoted(self):
        # git quotes paths with `"`, `\` or control characters, even with
        # core.quotePath=false.
        diff = r'+++ "b/src/q\"t\\u.cc"' + "\n@@ -0,0 +1 @@\n+x\n"
        self.assertEqual(parse_changed_lines(diff), {r'src/q"t\u.cc': {1}})
