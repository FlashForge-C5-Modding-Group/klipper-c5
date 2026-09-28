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
    def test_infinite_spool_changes_to_selected_loaded_head(self):
        runout = load_method('AFC_lane.py', 'AFCLane',
                             '_perform_creator5_infinite_runout')
        source = mock.Mock(name='source', map='T0')
        source.name = 'extruder'
        source.runout_lane = 'extruder2'
        source.extruder_obj.toolhead_extruder.get_heater.return_value.target_temp = 220
        source.extruder_obj.toolhead_extruder.get_heater.return_value.min_extrude_temp = 170
        target = mock.Mock(name='target')
        target.name = 'extruder2'
        target.extruder_obj.creator5_tool_index = 2
        target.extruder_obj.tool_start_state = True
        target.extruder_obj.on_shuttle.return_value = True
        source.afc.lanes = {'extruder2': target}
        source.afc.error_state = False

        runout(source)

        source.afc.CHANGE_TOOL.assert_called_once_with(
            target, restore_pos=False)
        source.printer.lookup_object.return_value.set_temperature.assert_called_once_with(
            target.extruder_obj.toolhead_extruder.get_heater.return_value,
            220, wait=True)
        source.gcode.run_script_from_command.assert_called_once_with(
            'SET_MAP LANE=extruder2 MAP=T0')
        target.set_tool_loaded.assert_called_once_with(
            normal_toolchange=True)
        source.afc.restore_pos.assert_called_once_with(
            move_z_first=False)
        source.afc.error.pause_resume.send_resume_command.assert_called_once()

    def test_infinite_spool_does_not_change_to_empty_head(self):
        runout = load_method('AFC_lane.py', 'AFCLane',
                             '_perform_creator5_infinite_runout')
        source = mock.Mock(name='source')
        source.runout_lane = 'extruder2'
        target = mock.Mock(name='target')
        target.extruder_obj.creator5_tool_index = 2
        target.extruder_obj.tool_start_state = False
        source.afc.lanes = {'extruder2': target}

        runout(source)

        source.afc.CHANGE_TOOL.assert_not_called()
        source.afc.error.pause_resume.send_resume_command.assert_not_called()
        source.afc.error.AFC_error.assert_called_once()

    def test_infinite_spool_keeps_print_paused_on_swap_failure(self):
        runout = load_method('AFC_lane.py', 'AFCLane',
                             '_perform_creator5_infinite_runout')
        source = mock.Mock(name='source', map='T1')
        source.name = 'extruder1'
        source.runout_lane = 'extruder3'
        source.extruder_obj.toolhead_extruder.get_heater.return_value.target_temp = 230
        source.extruder_obj.toolhead_extruder.get_heater.return_value.min_extrude_temp = 170
        target = mock.Mock(name='target')
        target.name = 'extruder3'
        target.extruder_obj.creator5_tool_index = 3
        target.extruder_obj.tool_start_state = True
        source.afc.lanes = {'extruder3': target}
        source.afc.CHANGE_TOOL.side_effect = RuntimeError('grab failed')

        runout(source)

        source.afc.error.AFC_error.assert_called_once()
        source.afc.error.pause_resume.send_resume_command.assert_not_called()
        source.gcode.run_script_from_command.assert_not_called()
        source.afc.restore_pos.assert_not_called()


    def test_standalone_print_start_skips_blocking_metadata_timer(self):
        reset = load_method('AFC.py', 'afc', '_reset_file_callback')
        afc = types.SimpleNamespace(
            enable_print_metadata=False, in_print_timer=None,
            reactor=mock.Mock(), error=mock.Mock(), gcode=mock.Mock(),
            print_data_metadata=mock.Mock(), save_vars=mock.Mock(),
            number_of_toolchanges=9, current_toolchange=4,
            print_tool_temperatures=[220])

        reset(afc)

        afc.reactor.register_timer.assert_not_called()
        afc.print_data_metadata.reset.assert_called_once_with()
        self.assertEqual(afc.number_of_toolchanges, 0)
        self.assertEqual(afc.current_toolchange, -1)
        self.assertEqual(afc.print_tool_temperatures, [])

    def test_creator5_swap_does_not_wait_twice_before_return_travel(self):
        state = types.SimpleNamespace(TOOL_DOCK='dock', TOOL_SWAP='swap',
                                      TOOL_PICKUP='pickup', IDLE='idle')
        swap = load_method('AFC_Toolchanger.py', 'AfcToolchanger',
                           'tool_swap', {'State': state})
        afc = mock.Mock()
        afc.last_gcode_position = [0., 0., 0., 0.]
        afc.gcode_move.base_position = [0., 0., 0., 0.]
        afc.gcode_move.homing_position = [0., 0., 0., 0.]
        afc.afcDeltaTime.delta_time = 1.
        unit = mock.Mock(afc=afc)
        unit.function.get_current_extruder_obj.return_value = None
        extruder = mock.Mock(creator5_tool_index=0,
                             custom_tool_swap='C5_TOOL_SELECT T=0',
                             th_extruder_name='extruder')
        lane = mock.Mock(extruder_obj=extruder)

        swap(unit, lane)

        afc.gcode.run_script_from_command.assert_called_once_with(
            'C5_TOOL_SELECT T=0')
        lane.activate_toolhead_extruder.assert_called_once_with()
        afc.function._handle_activate_extruder.assert_not_called()
        afc.toolhead.wait_moves.assert_not_called()

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
            creator5_filament_sync=None, lanes={}, status='Idle', afc=afc)
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
        afc.restore_pos.assert_called_once_with(move_z_first=False)
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
