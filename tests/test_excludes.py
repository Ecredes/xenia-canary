import unittest

from fork_check.excludes import analyzable, is_excluded, load_codacy_excludes

CODACY = """\
---
exclude_paths:
  - 'tools/**'
  - 'third_party/**'
  - "docs/**"
  - android/**
  - '**.md'
  - 'src/xenia/gpu/shaders/**'
"""


class ExcludesTest(unittest.TestCase):
    def setUp(self):
        self.patterns = load_codacy_excludes(CODACY)

    def test_loads_all_quote_styles(self):
        self.assertEqual(self.patterns, [
            "tools/**", "third_party/**", "docs/**", "android/**", "**.md",
            "src/xenia/gpu/shaders/**"])

    def test_prefix_patterns(self):
        self.assertTrue(is_excluded("third_party/fmt/x.h", self.patterns))
        self.assertTrue(is_excluded("src/xenia/gpu/shaders/bytecode/a.h", self.patterns))
        self.assertFalse(is_excluded("src/xenia/gpu/shader.cc", self.patterns))

    def test_double_star_suffix(self):
        self.assertTrue(is_excluded("src/README.md", self.patterns))

    def test_no_partial_directory_match(self):
        self.assertFalse(is_excluded("src/tools/a.cc", self.patterns))

    def test_analyzable(self):
        self.assertTrue(analyzable("src/xenia/base/a.cc", self.patterns))
        self.assertTrue(analyzable("src/xenia/base/a.inl", self.patterns))
        self.assertFalse(analyzable("src/xenia/base/a.py", self.patterns))
        self.assertFalse(analyzable("third_party/a.cc", self.patterns))

    def test_empty_config(self):
        self.assertEqual(load_codacy_excludes(""), [])

    def test_list_ends_at_next_key(self):
        text = "exclude_paths:\n  - 'a/**'\nengines:\n  - 'not/a/path'\n"
        self.assertEqual(load_codacy_excludes(text), ["a/**"])
