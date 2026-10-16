import pathlib
import subprocess
import tempfile
import unittest

from fork_check.snapshot import _current_key, cache_key, clean, shader_targets


def make_repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    for args in (["config", "user.email", "t@t"], ["config", "user.name", "t"]):
        subprocess.run(["git", "-C", str(root), *args], check=True)
    (root / "CMakeLists.txt").write_text("project(x)\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "init"], check=True)
    return root


class CleanTest(unittest.TestCase):
    def test_clean_keeps_every_shader_bytecode_dir_and_build(self):
        # Review finding: the UI shader headers were deleted while ninja's
        # stamp survived in build/, so they were never regenerated.
        with tempfile.TemporaryDirectory() as d:
            root = make_repo(d)
            keep = ["src/xenia/ui/shaders/bytecode/vulkan_spirv/a.h",
                    "src/xenia/gpu/shaders/bytecode/vulkan_spirv/b.h",
                    "build/x.stamp"]
            for p in keep + ["src/junk.txt"]:
                (root / p).parent.mkdir(parents=True, exist_ok=True)
                (root / p).write_text("x")
            clean(root)
            self.assertEqual([p for p in keep if not (root / p).exists()], [])
            self.assertFalse((root / "src/junk.txt").exists())


class CurrentKeyTest(unittest.TestCase):
    def test_key_changes_when_shader_source_added(self):
        # Shader sources are collected with file(GLOB) too.
        with tempfile.TemporaryDirectory() as d:
            root = make_repo(d)
            before = _current_key(root)
            (root / "src").mkdir()
            (root / "src/new.xesl").write_text("x")
            subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
            self.assertNotEqual(_current_key(root), before)

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
