import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from fork_check import snapshot
from fork_check.findings import CheckerBroken
from fork_check.snapshot import (_current_key, cache_key, clean, ensure_version_h,
                                 shader_targets)


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


class VersionHeaderTest(unittest.TestCase):
    def test_written_when_missing(self):
        # -Full found emulator_window.cc includes "version.h", which
        # `xb` generates outside CMake.
        with tempfile.TemporaryDirectory() as d:
            ensure_version_h(pathlib.Path(d))
            text = (pathlib.Path(d) / "version.h").read_text()
            self.assertIn("#define XE_BUILD_COMMIT_SHORT", text)

    def test_existing_left_alone(self):
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / "version.h").write_text("// mine\n")
            ensure_version_h(pathlib.Path(d))
            self.assertEqual((pathlib.Path(d) / "version.h").read_text(), "// mine\n")


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

def commit_all(root, message):
    subprocess.run(["git", "-C", str(root), "commit", "-q", "--allow-empty", "-m",
                    message], check=True)
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()


class PrepareTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        tmp = pathlib.Path(self._tmp.name)
        self.win = make_repo(tmp / "win")
        for name, value in (("SCRATCH", tmp / "scratch"),
                            ("REFERENCE_CLONE", tmp / "no-reference")):
            patcher = mock.patch.object(snapshot, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    # A local clone copies every object, so each test clones first; the bugs
    # only show on the fetch of a later run.
    def _first_run(self):
        base = commit_all(self.win, "base")
        snapshot.prepare(str(self.win), base, base)
        return base

    def test_detached_head_commit_is_fetched(self):
        base = self._first_run()
        subprocess.run(["git", "-C", str(self.win), "checkout", "-q", "--detach"],
                       check=True)
        head = commit_all(self.win, "on no branch")
        root = snapshot.prepare(str(self.win), head, base)
        self.assertEqual(snapshot.git(root, "rev-parse", "HEAD").strip(), head)

    def test_unreachable_base_names_the_cause(self):
        head = self._first_run()
        subprocess.run(["git", "-C", str(self.win), "checkout", "-q", "--orphan", "x"],
                       check=True)
        stray = commit_all(self.win, "stray")
        subprocess.run(["git", "-C", str(self.win), "checkout", "-q", "--detach", head],
                       check=True)
        subprocess.run(["git", "-C", str(self.win), "branch", "-q", "-D", "x"], check=True)
        with self.assertRaises(CheckerBroken) as ctx:
            snapshot.prepare(str(self.win), head, stray)
        self.assertIn("-Base", str(ctx.exception))


class ShaderTargetNamesTest(unittest.TestCase):
    def test_failed_target_listing_is_broken(self):
        failed = subprocess.CompletedProcess([], 1, "", "ninja: error: loading 'build.ninja'")
        with mock.patch.object(snapshot, "_run", return_value=failed):
            with self.assertRaises(CheckerBroken) as ctx:
                snapshot.list_shader_targets(pathlib.Path("/b"), {})
        self.assertIn("build.ninja", str(ctx.exception))


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


class ExclusiveTest(unittest.TestCase):
    def test_second_run_waits_for_the_first(self):
        # Two concurrent checks used to share the scratch clone, and one's
        # checkout removed files the other was analyzing.
        import io
        import threading
        from contextlib import redirect_stderr
        with tempfile.TemporaryDirectory() as d:
            lock = pathlib.Path(d) / "lock"
            events = []
            entered = threading.Event()

            def second():
                with snapshot.exclusive(lock):
                    events.append("second")

            err = io.StringIO()
            with redirect_stderr(err):
                with snapshot.exclusive(lock):
                    thread = threading.Thread(target=second)
                    thread.start()
                    for _ in range(100):
                        if "waiting" in err.getvalue():
                            entered.set()
                            break
                        threading.Event().wait(0.01)
                    events.append("first")
                thread.join(5)
            self.assertTrue(entered.is_set())
            self.assertEqual(events, ["first", "second"])
