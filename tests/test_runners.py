import threading
import time
import unittest

from fork_check.findings import CheckerBroken, Finding
from fork_check.runners import (cppcheck_groups, cpplint_command, drop_flawfinder_ignored,
                               run_parallel, tidy_jobs)


class CppcheckGroupsTest(unittest.TestCase):
    def test_headers_are_parsed_as_cpp(self):
        # Without --language=c++ cppcheck parses .h as C and reports
        # syntaxError on every `namespace xe {`.
        groups = cppcheck_groups(["src/a.h", "src/b.cc", "src/c.c", "src/d.inl"])
        self.assertEqual(groups, [([], ["src/c.c"]),
                                  (["--language=c++"], ["src/a.h", "src/b.cc", "src/d.inl"])])

    def test_empty_groups_dropped(self):
        self.assertEqual(cppcheck_groups(["src/b.cc"]), [(["--language=c++"], ["src/b.cc"])])


class CpplintCommandTest(unittest.TestCase):
    def test_inl_files_accepted(self):
        # cpplint 2.0 prints "Ignoring x.inl; not a valid file name" otherwise.
        cmd = cpplint_command(["src/xenia/cpu/hir/opcodes.inl"])
        ext = next(a for a in cmd if a.startswith("--extensions="))
        self.assertIn("inl", ext.split("=", 1)[1].split(","))
        self.assertEqual(cmd[-1], "src/xenia/cpu/hir/opcodes.inl")


class FlawfinderIgnoredTest(unittest.TestCase):
    def test_memcpy_dropped(self):
        keep = Finding("a.cc", 1, "flawfinder", "strcpy", "m")
        drop = Finding("a.cc", 2, "flawfinder", "memcpy", "m")
        self.assertEqual(drop_flawfinder_ignored([keep, drop]), [keep])


class RunParallelTest(unittest.TestCase):
    def test_results_flattened_in_input_order(self):
        self.assertEqual(run_parallel(lambda x: [x, x], [1, 2, 3], 2), [1, 1, 2, 2, 3, 3])

    def test_stops_queueing_after_a_failure(self):
        calls = []

        def work(x):
            calls.append(x)
            raise CheckerBroken(f"failed on {x}")

        with self.assertRaises(CheckerBroken):
            run_parallel(work, [1, 2, 3, 4], 1)
        self.assertEqual(calls, [1])

    def test_free_worker_skips_queued_items_after_a_failure(self):
        # The main thread is still waiting on "slow" when "bad" fails, so it
        # can't cancel yet; the free worker must skip "x" on its own.
        bad_failed = threading.Event()
        calls = []

        def work(item):
            calls.append(item)
            if item == "slow":
                bad_failed.wait(5)
                time.sleep(0.2)
            elif item == "bad":
                bad_failed.set()
                raise CheckerBroken("bad")
            return [item]

        with self.assertRaises(CheckerBroken):
            run_parallel(work, ["slow", "bad", "x"], 2)
        self.assertNotIn("x", calls)


class TidyJobsTest(unittest.TestCase):
    GIB_KB = 1024 * 1024

    def test_memory_caps_jobs(self):
        # A heavy xenia TU peaks near 1 GB in clang-tidy; 1.5 GiB per job.
        self.assertEqual(tidy_jobs(16, 15 * self.GIB_KB), 10)

    def test_cpus_cap_jobs(self):
        self.assertEqual(tidy_jobs(4, 64 * self.GIB_KB), 4)

    def test_at_least_one_job(self):
        self.assertEqual(tidy_jobs(8, 0), 1)

    def test_unknown_memory_uses_cpus(self):
        self.assertEqual(tidy_jobs(8, None), 8)
