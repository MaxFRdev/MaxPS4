#!/usr/bin/env python3
"""Exercise the bounded runner through real child processes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class BoundedRunnerTest(unittest.TestCase):
    def run_child(self, code, deadline="5"):
        runner = Path(__file__).with_name("run-bounded.py")
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "child.txt"
            result = subprocess.run(
                [sys.executable, str(runner), deadline, str(log),
                 sys.executable, "-c", code],
                capture_output=True, text=True, timeout=10,
            )
            return result, log.read_text()

    def test_success_preserves_output(self):
        result, log = self.run_child("print('completed')")
        self.assertEqual(result.returncode, 0)
        self.assertIn("completed", log)
        self.assertIn("completed", result.stdout)

    def test_failure_preserves_exit_code(self):
        result, _ = self.run_child("raise SystemExit(7)")
        self.assertEqual(result.returncode, 7)

    def test_timeout_preserves_partial_output(self):
        result, log = self.run_child(
            "import time; print('started', flush=True); time.sleep(10)", "1",
        )
        self.assertEqual(result.returncode, 124)
        self.assertIn("started", log)
        self.assertIn("deadline exceeded", log)

    @unittest.skipUnless(os.name == "nt", "Windows exit status")
    def test_windows_status_cannot_be_truncated_to_success(self):
        result, _ = self.run_child("raise SystemExit(1536)")
        self.assertEqual(result.returncode, 1)
        self.assertIn("1536", result.stdout)

    @unittest.skipIf(os.name == "nt", "POSIX signal status")
    def test_signal_is_reported_as_failure(self):
        result, _ = self.run_child(
            "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
        )
        self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
