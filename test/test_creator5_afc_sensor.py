"""Exercise Creator 5's AFC sensor callback without importing Linux-only Klippy."""

import ast
import pathlib
import types
import unittest
from unittest import mock


def load_method(filename, class_name, method_name, symbols=None):
    source = pathlib.Path(__file__).resolve().parents[1] / (
        'klippy/extras' + '/' + filename)
    tree = ast.parse(source.read_text(encoding='utf-8'))
    cls = next(node for node in tree.body
               if isinstance(node, ast.ClassDef) and node.name == class_name)
    method = next(node for node in cls.body
                  if isinstance(node, ast.FunctionDef)
                  and node.name == method_name)
    namespace = {'READY': 'Ready'}
    if symbols:
        namespace.update(symbols)
    future = ast.ImportFrom(module='__future__',
                            names=[ast.alias(name='annotations')], level=0)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[future, method], type_ignores=[])), str(source),
                 'exec'), namespace)
    return namespace[method_name]


class Creator5AfcSensorTest(unittest.TestCase):
    def test_unused_td1_probe_is_disabled_without_moonraker_query(self):
        get_td1_present = load_method('AFC.py', 'afc', 'td1_present')
        afc = types.SimpleNamespace(enable_td1_detection=False,
                                    moonraker=mock.Mock())
        self.assertFalse(get_td1_present.fget(afc))
        afc.moonraker.check_for_td1.assert_not_called()

    def test_unused_weight_timer_is_not_started_for_standalone_tools(self):
        enable = load_method('AFC_lane.py', 'AFCLane',
                             'enable_weight_timer')
        disable = load_method('AFC_lane.py', 'AFCLane',
                              'disable_weight_timer')
        lane = types.SimpleNamespace(cb_update_weight=None,
                                     reactor=mock.Mock(), afc=mock.Mock())
        enable(lane)
        disable(lane)
        lane.reactor.update_timer.assert_not_called()
        lane.afc.save_vars.assert_not_called()

    def test_only_pin_confirmed_head_reports_idle_or_printing(self):
        state = types.SimpleNamespace(ERROR='Error', PARKED='Parked',
                                      PRINTING='Printing',
                                      TOOL_DOCK='ToolDock',
                                      TOOL_PICKUP='ToolPickup')
        status = load_method('AFC_extruder.py', 'AFCExtruder', 'get_status',
                             {'State': state})
        afc = mock.Mock()
        extruder = mock.Mock(
            creator5_tool_index=1, creator5_mount_error=None,
            lanes={}, status='Idle', afc=afc)
        extruder.on_shuttle.return_value = False
        self.assertEqual(status(extruder)['status'], 'Parked')
        afc.function.is_printing.assert_not_called()

        extruder.on_shuttle.return_value = True
        afc.function.is_printing.return_value = False
        self.assertEqual(status(extruder)['status'], 'Idle')
        afc.function.is_printing.return_value = True
        self.assertEqual(status(extruder)['status'], 'Printing')

        extruder.creator5_mount_error = 'Missing dock confirmation'
        extruder.on_shuttle.return_value = False
        self.assertEqual(status(extruder)['status'], 'Error')

    def test_prep_labels_filament_and_mount_separately(self):
        prep = load_method('AFC_Toolchanger.py', 'AfcToolchanger',
                           'system_Test')
        extruder = mock.Mock(creator5_tool_index=0, lane_loaded='extruder',
                             tool_start_state=True)
        extruder.prep_on_shuttle_check.return_value = ' parked'
        lane = mock.Mock(name='extruder', prep_state=True, load_state=True,
                         extruder_obj=extruder, map='T0')
        lane.name = 'extruder'
        unit = mock.Mock()
        unit.afc.function.TcmdAssign = mock.Mock()
        prep(unit, lane, 0., '', False)
        line = unit.logger.raw.call_args.args[0]
        self.assertIn('FILAMENT PRESENT', line)
        self.assertIn('parked', line)
        self.assertNotIn('in ToolHead', line)

        lane.load_state = False
        extruder.tool_start_state = False
        extruder.prep_on_shuttle_check.return_value = ' mounted'
        prep(unit, lane, 0., '', False)
        line = unit.logger.raw.call_args.args[0]
        self.assertIn('FILAMENT ABSENT', line)
        self.assertIn('mounted', line)

    def test_filament_switch_updates_state_without_extruding(self):
        lane = mock.Mock(_afc_prep_done=True, custom_load_cmd=None)
        afc = mock.Mock()
        afc.function.is_printing.return_value = False
        extruder = types.SimpleNamespace(
            tc_unit_name='Tools', is_standalone=lambda: True,
            tc_lane=lane, tool_start_state=False, afc=afc,
            on_shuttle=lambda: True, printer=mock.Mock(state_message='Ready'),
            auto_load_on_tool_start=False, load_active=False,
            tool_stn=100, load_unload_sequence=mock.Mock(),
            logger=mock.Mock())

        callback = load_method('AFC_extruder.py', 'AFCExtruder',
                               'tool_start_callback')
        callback(extruder, 0., True)
        self.assertTrue(extruder.tool_start_state)
        lane.set_tool_loaded.assert_called_once_with()
        lane.set_loaded.assert_called_once_with()
        extruder.load_unload_sequence.assert_not_called()
        afc.save_vars.assert_called_once_with()

        callback(extruder, 1., False)
        lane.set_tool_unloaded.assert_called_once_with()
        lane.set_unloaded.assert_called_once_with()
        extruder.load_unload_sequence.assert_not_called()

    def test_t_command_swaps_head_without_filament_change(self):
        lane = mock.Mock(extruder_obj=types.SimpleNamespace(
            creator5_tool_index=0))
        afc = types.SimpleNamespace(position_saved=False, in_toolchange=False)
        afc.save_pos = mock.Mock(side_effect=lambda: setattr(
            afc, 'position_saved', True))
        afc.restore_pos = mock.Mock()

        change_tool = load_method('AFC.py', 'afc', 'CHANGE_TOOL')
        change_tool(afc, lane)
        lane.tool_swap.assert_called_once_with()
        afc.restore_pos.assert_called_once_with()
        self.assertFalse(afc.in_toolchange)

        lane.tool_swap.reset_mock()
        afc.restore_pos.reset_mock()
        lane.tool_swap.side_effect = RuntimeError('grab sensor failed')
        with self.assertRaisesRegex(RuntimeError, 'grab sensor failed'):
            change_tool(afc, lane)
        afc.restore_pos.assert_not_called()
        self.assertFalse(afc.in_toolchange)


if __name__ == '__main__':
    unittest.main()
