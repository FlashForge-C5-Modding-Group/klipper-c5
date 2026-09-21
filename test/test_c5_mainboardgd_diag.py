# Creator 5 mainBoardGD diagnostic native tests
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class C5MainBoardGDDiagnosticTest(unittest.TestCase):
    def test_diagnostic_state_transitions(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = pathlib.Path(temp) / "c5_mainboardgd_diag"
            command = [
                "gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Isrc", "test/c5_mainboardgd_diag.c",
                "src/c5_mainboardgd_diag.c", "-o", str(executable),
            ]
            compiled = subprocess.run(
                command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(
                compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run(
                [str(executable)], cwd=ROOT, text=True, capture_output=True,
                timeout=2)
            self.assertEqual(result.returncode, 0,
                             result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
