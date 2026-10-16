import unittest

from fork_check.snapshot import cache_key, shader_targets

CMAKE = "100644 aaa 0\tCMakeLists.txt\n100644 bbb 0\tcmake/XeniaHelpers.cmake\n"
SOURCES = "src/xenia/base/a.cc\nsrc/xenia/base/b.cc\n"
SUBS = " d980192 third_party/FFmpeg (d980192)\n"

# Real `ninja -t targets all` lines from the scratch build (one target is
# listed under three spellings).
NINJA_TARGETS = """\
src/xenia/gpu/vulkan/xenia-gpu-vulkan-spirv-shaders: phony
src/xenia/gpu/vulkan/CMakeFiles/xenia-gpu-vulkan-spirv-shaders: CUSTOM_COMMAND
/home/u/src/xenia-fork-check/build/src/xenia/gpu/vulkan/CMakeFiles/xenia-gpu-vulkan-spirv-shaders: phony
xenia-gpu-vulkan-spirv-shaders: phony
xenia-ui-vulkan-spirv-shaders: phony
xenia-base: phony
"""


class CacheKeyTest(unittest.TestCase):
    def test_stable(self):
        self.assertEqual(cache_key(CMAKE, SOURCES, SUBS), cache_key(CMAKE, SOURCES, SUBS))

    def test_cache_key_changes_when_cmake_changes(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE.replace("aaa", "ccc"), SOURCES, SUBS))

    def test_cache_key_changes_when_source_added(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE, SOURCES + "src/xenia/base/c.cc\n", SUBS))

    def test_cache_key_changes_when_submodule_moves(self):
        self.assertNotEqual(cache_key(CMAKE, SOURCES, SUBS),
                            cache_key(CMAKE, SOURCES, SUBS.replace("d980192", "e000000")))

    def test_fields_do_not_run_together(self):
        self.assertNotEqual(cache_key("ab", "c", ""), cache_key("a", "bc", ""))


class ShaderTargetsTest(unittest.TestCase):
    def test_bare_names_only(self):
        self.assertEqual(shader_targets(NINJA_TARGETS),
                         ["xenia-gpu-vulkan-spirv-shaders", "xenia-ui-vulkan-spirv-shaders"])
