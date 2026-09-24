import sys
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'klippy'))
import gcode


class GCodeNumericPrefixTests(unittest.TestCase):
    def setUp(self):
        dispatch = gcode.GCodeDispatch.__new__(gcode.GCodeDispatch)
        dispatch.printer = mock.Mock()
        dispatch.printer.config_error = ValueError
        dispatch.ready_gcode_handlers = {}
        dispatch.base_gcode_handlers = {}
        dispatch.gcode_handlers = dispatch.ready_gcode_handlers
        dispatch.gcode_help = {}
        dispatch.output_callbacks = []
        dispatch.is_printer_ready = True
        dispatch.is_fileinput = False
        self.dispatch = dispatch
        self.calls = []

    def test_creator5_extended_command_registers_and_dispatches(self):
        self.dispatch.register_command('C5_PREPARE_FILAMENT_LOAD',
                                       lambda cmd: self.calls.append(
                                           (cmd.get_command(), cmd.get('TOOL'),
                                            cmd.get('TEMP'))))
        self.dispatch.run_script_from_command(
            'C5_PREPARE_FILAMENT_LOAD TOOL=2 TEMP=220')
        self.assertEqual(self.calls,
                         [('C5_PREPARE_FILAMENT_LOAD', '2', '220')])

    def test_normal_gcode_and_invalid_numeric_name(self):
        self.dispatch.register_command(
            'G1', lambda cmd: self.calls.append(
                (cmd.get_command(), cmd.get('X'))))
        self.dispatch.run_script_from_command('G1 X10')
        self.assertEqual(self.calls, [('G1', '10')])
        with self.assertRaisesRegex(ValueError, 'invalid name'):
            self.dispatch.register_command('C5PREPARE', lambda cmd: None)


if __name__ == '__main__':
    unittest.main()
