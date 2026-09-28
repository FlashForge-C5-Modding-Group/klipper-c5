"""Guard Creator 5 step-generation batching without loading Klippy CFFI."""

import ast
import pathlib
import types
import unittest
from unittest import mock


SOURCE = (pathlib.Path(__file__).resolve().parents[1] / 'klippy' /
          'extras' / 'motion_queuing.py')


def load_flush_handler():
    tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
    constants = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    constants[target.id] = node.value.value
    cls = next(node for node in tree.body if isinstance(node, ast.ClassDef)
               and node.name == 'PrinterMotionQueuing')
    method = next(node for node in cls.body if isinstance(node, ast.FunctionDef)
                  and node.name == '_flush_handler')
    namespace = dict(constants, logging=mock.Mock())
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[method], type_ignores=[])), str(SOURCE), 'exec'),
         namespace)
    return namespace['_flush_handler'], constants


class Creator5MotionQueueTest(unittest.TestCase):
    def test_late_wakeup_is_split_into_short_batches(self):
        handler, constants = load_flush_handler()
        self.assertAlmostEqual(constants['BGFLUSH_SG_HIGH_TIME'] -
                               constants['BGFLUSH_SG_LOW_TIME'], 0.125)
        queue = types.SimpleNamespace(
            mcu=mock.Mock(), need_step_gen_time=20., kin_flush_delay=0.,
            last_step_gen_time=10., _advance_flush_time=mock.Mock(),
            reactor=types.SimpleNamespace(NEVER=1e16), printer=mock.Mock())
        queue.mcu.estimated_print_time.return_value = 10.05

        handler(queue, 10.05)

        queue._advance_flush_time.assert_called_once_with(0., 10.125)
        self.assertFalse(queue.printer.invoke_shutdown.called)

    def test_large_idle_gap_fast_forwards(self):
        handler, _ = load_flush_handler()
        queue = types.SimpleNamespace(
            mcu=mock.Mock(), need_step_gen_time=30., kin_flush_delay=0.,
            last_step_gen_time=10., _advance_flush_time=mock.Mock(),
            reactor=types.SimpleNamespace(NEVER=1e16), printer=mock.Mock())
        queue.mcu.estimated_print_time.return_value = 20.

        handler(queue, 20.)

        queue._advance_flush_time.assert_called_once_with(0., 21.25)


if __name__ == '__main__':
    unittest.main()
