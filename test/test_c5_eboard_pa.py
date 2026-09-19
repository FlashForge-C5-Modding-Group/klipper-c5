# Creator 5 eBoard pressure-advance scorer native test
#
# Copyright (C) 2026
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class C5EBoardPAScorerTest(unittest.TestCase):
    def test_production_scorer_vectors(self):
        with tempfile.TemporaryDirectory() as temp:
            executable = pathlib.Path(temp) / "c5_eboard_pa"
            compile_result = subprocess.run(
                ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", "src", "src/c5_eboard_pa.c", "test/c5_eboard_pa.c",
                 "-o", str(executable)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(run_result.returncode, 0,
                             run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
