import io
import pathlib
import tempfile
import unittest
from contextlib import redirect_stderr
from unittest import mock

from fork_check import cli


class CliArgsTest(unittest.TestCase):
    def setUp(self):
        # main() takes the scratch lock; keep the tests off the real one.
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        patcher = mock.patch.object(cli.snapshot, "LOCK_FILE",
                                    pathlib.Path(tmp.name) / "lock")
        patcher.start()
        self.addCleanup(patcher.stop)
    def test_unknown_tool_is_usage_error(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
            cli.parse_args(["--win-repo", "/w", "--commit", "c", "--base", "b",
                            "--tools", "cppcheck,bogus"])
        self.assertEqual(cm.exception.code, 2)

    def test_defaults(self):
        args = cli.parse_args(["--win-repo", "/w", "--commit", "c", "--base", "b"])
        self.assertEqual(args.scope, "changed")
        self.assertEqual(args.tools, ["cppcheck", "flawfinder", "clang-tidy", "cpplint"])

    def test_broken_returns_2(self):
        with mock.patch.object(cli.runners, "require",
                               side_effect=cli.CheckerBroken("missing tools")), \
             redirect_stderr(io.StringIO()) as err:
            code = cli.main(["--win-repo", "/w", "--commit", "c", "--base", "b"])
        self.assertEqual(code, 2)
        self.assertIn("checker broken: missing tools", err.getvalue())

    def test_unexpected_exception_returns_2(self):
        # Review finding: a traceback exited 1, which reads as "findings".
        with mock.patch.object(cli.runners, "require",
                               side_effect=FileNotFoundError("cmake")), \
             redirect_stderr(io.StringIO()) as err:
            code = cli.main(["--win-repo", "/w", "--commit", "c", "--base", "b"])
        self.assertEqual(code, 2)
        self.assertIn("checker broken", err.getvalue())
        self.assertIn("FileNotFoundError", err.getvalue())
