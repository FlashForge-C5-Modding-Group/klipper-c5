import importlib.util
import json
import os
import tempfile
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / 'klippy' / 'extras' / 'creator5_toolchanger.py'
SPEC = importlib.util.spec_from_file_location('creator5_toolchanger', MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GCmd:
    def __init__(self, **params):
        self.params = params

    def get_int(self, name, default=None, **kwargs):
        return self.params.get(name, default)

    def get_float(self, name, default=None, **kwargs):
        return self.params.get(name, default)

    def error(self, message):
        return RuntimeError(message)

    def respond_info(self, message):
        self.message = message


class Creator5OffsetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = os.path.join(self.temp.name, 'extruder.json')
        with open(self.path, 'w') as target:
            target.write('{"now_extruder": -1, "t0_offset_x": 16.25, '
                         '"z_station_pos": -1.5}\n/* stock comment */\n')
        self.board = MODULE.Creator5Toolchanger.__new__(
            MODULE.Creator5Toolchanger)
        self.board.extruder_json_path = self.path
        self.board.measurements = [[15., 215., 1.] for _ in range(4)]
        self.board.docks = [[298., 50. + i * 50.] for i in range(4)]
        self.board.station_x = 29.
        self.board.station_y = 214.
        self.board.station_z = -1.
        self.board.busy = False
        self.board.printer = mock.Mock()
        self.board.printer.lookup_object.return_value.get_status.return_value = {
            'homing_origin': (.1, .2, .3, 0.)}

    def test_load_and_save_stock_schema(self):
        self.board._load_extruder_json()
        self.assertEqual(self.board.measurements[0][0], 16.25)
        self.assertEqual(self.board.station_z, -1.5)
        self.board.measurements[1][2] = 1.23
        self.board.cmd_save_offsets_json(GCmd())
        data, comment = self.board._read_stock_json(self.path)
        self.assertEqual(data['t1_offset_z'], 1.23)
        self.assertEqual(data['now_extruder'], -1)
        self.assertEqual(comment, '/* stock comment */')
        backups = list(Path(self.temp.name).glob('extruder.json.bak-*'))
        self.assertEqual(len(backups), 1)

    def test_applies_relative_xyz_plus_manual_z(self):
        self.board.measurements[0] = [16.25, 214., 1.3]
        self.board.measurements[1] = [15.95, 213.5, 1.2]
        self.board.z_adjustments = [0., 0.04, 0., 0.]
        commands = []
        self.board._run = commands.append
        self.board._apply_offsets(1)
        self.assertEqual(commands, [
            'SET_GCODE_OFFSET X=-0.3000 Y=-0.5000 Z=-0.0600 MOVE=0'])

    def test_no_levelboard_motion_before_plate_confirmation(self):
        self.board.reference_scan_height = None
        self.board._preflight = lambda command: self.fail('preflight reached')
        with self.assertRaisesRegex(RuntimeError, 'Remove the build plate'):
            self.board.cmd_reference_calibrate(GCmd())
        with self.assertRaisesRegex(RuntimeError, 'Remove the build plate'):
            self.board.cmd_offset_calibrate(GCmd(T=0))

    def test_automatic_z_precedes_xy_and_sets_scan_height(self):
        calls = []
        self.board.scan_height = None
        self.board._preflight = lambda command: ([], [], 1)
        self.board._with_motion = lambda command, operation: operation()
        self.board._run = calls.append
        self.board._calibrate_tool_z = lambda command, tool: (
            calls.append(('z', tool)) or 1.23)
        self.board._calibrate_tool_xy = lambda command, tool, height: (
            calls.append(('xy', tool, height)))
        self.board._apply_offsets = lambda tool: calls.append(('apply', tool))
        self.board.cmd_offset_calibrate(GCmd(
            BUILDPLATE_REMOVED=1, T=1, Z=1, SAVE=0))
        self.assertEqual(calls[0],
                         'SET_GCODE_OFFSET X=0 Y=0 Z=0 MOVE=0')
        self.assertEqual(calls[1], ('z', 1))
        self.assertEqual(calls[2][0:2], ('xy', 1))
        self.assertAlmostEqual(calls[2][2], 1.83)
        self.assertEqual(calls[3], ('apply', 1))

    def test_xy_failure_restores_measurements_and_runtime_offsets(self):
        self.board.scan_height = None
        self.board._preflight = lambda command: ([], [], 1)
        self.board._with_motion = lambda command, operation: operation()
        self.board._run = mock.Mock()
        original = [list(v) for v in self.board.measurements]
        def probe_z(command, tool):
            self.board.measurements[tool][2] = 2.
            return 2.
        self.board._calibrate_tool_z = probe_z
        self.board._calibrate_tool_xy = mock.Mock(
            side_effect=RuntimeError('XY probe failed'))
        self.board.configfile = mock.Mock()
        with self.assertRaisesRegex(RuntimeError, 'XY probe failed'):
            self.board.cmd_offset_calibrate(GCmd(
                BUILDPLATE_REMOVED=1, T=1, Z=1, SAVE=1))
        self.assertEqual(self.board.measurements, original)
        self.board.configfile.set.assert_not_called()
        self.board._run.assert_called_with(
            'SET_GCODE_OFFSET X=0.1000 Y=0.2000 Z=0.3000 MOVE=0')

    def test_manual_mount_save_survives_json_reload(self):
        self.board.max_mount_correction = 2.
        self.board.cmd_save_offsets_json(GCmd())
        self.board.cmd_mount_correct(GCmd(T=0, X=299., Y=51., SAVE=1))
        self.board._load_extruder_json()
        self.assertEqual(self.board.docks[0], [299., 51.])

    def test_purge_selects_attached_hotend_for_shared_drive(self):
        self.board.purge_length = 12.
        self.board.purge_x, self.board.purge_y, self.board.purge_z = 266.5, 13.8, 1.
        self.board.purge_speed = 3.
        self.board._preflight = lambda command: ([], [], 1)
        toolhead, heater, other = mock.Mock(), mock.Mock(), mock.Mock()
        toolhead.get_extruder.return_value = other
        heater.get_status.return_value = {'can_extrude': True}
        self.board.printer.lookup_object.side_effect = (
            lambda name: {'toolhead': toolhead, 'extruder1': heater}[name])
        self.board._with_motion = lambda command, operation: operation()
        self.board._raise_z = mock.Mock()
        self.board._move = mock.Mock()
        self.board._run = mock.Mock()
        self.board.cmd_purge(GCmd())
        self.assertEqual(self.board._run.call_args_list[0],
                         mock.call('ACTIVATE_EXTRUDER EXTRUDER=extruder1'))
        self.board._run.assert_any_call('G1 E12.000 F180')
        self.board._run.reset_mock()
        heater.get_status.return_value = {'can_extrude': False}
        with self.assertRaisesRegex(RuntimeError, 'not hot enough'):
            self.board.cmd_purge(GCmd())
        self.board._run.assert_not_called()

    def test_print_homing_recovers_head_before_full_home(self):
        calls = []
        self.board._preflight = mock.Mock(side_effect=[
            ([True, True, False, True], [], 2), ([True]*4, [], None)])
        self.board.cmd_recover_attached = lambda cmd: calls.append('recover')
        self.board._run = calls.append
        self.board.cmd_home_for_print(GCmd())
        self.assertEqual(calls, ['recover', 'G28'])

    def test_failed_recovery_never_homes_z(self):
        self.board._preflight = lambda command, **kwargs: ([], [], 2)
        self.board._run = mock.Mock()
        self.board.cmd_recover_attached = mock.Mock(
            side_effect=RuntimeError('dock failed'))
        with self.assertRaisesRegex(RuntimeError, 'dock failed'):
            self.board.cmd_home_for_print(GCmd())
        self.board._run.assert_not_called()

    def test_recovery_uses_xy_homing_and_no_fixture_probe(self):
        self.board.safe_z = 10.
        self.board.printer.lookup_object.return_value = mock.Mock(z_hop=10.)
        self.board._preflight = lambda command, **kwargs: ([], [], 2)
        self.board._with_motion = lambda command, op: op()
        calls = []
        self.board._run = calls.append
        self.board._dock = lambda cmd, tool, **kw: calls.append((tool, kw))
        self.board.cmd_recover_attached(GCmd())
        self.assertEqual(calls, ['G28 X Y', (2, {'raise_z': False}),
                                 'AFC_UNSELECT_TOOL'])

    def test_print_homing_requires_all_heads_parked(self):
        self.board._run = mock.Mock()
        self.board._preflight = lambda command, **kwargs: (
            [True, False, True, True], [], None)
        with self.assertRaisesRegex(RuntimeError, 'all heads'):
            self.board.cmd_home_for_print(GCmd())
        self.board._run.assert_not_called()
        self.board._preflight = lambda command, **kwargs: ([True]*4, [], None)
        self.board.cmd_home_for_print(GCmd())
        self.board._run.assert_called_once_with('G28')

    def test_missing_dock_confirmation_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Missing dock confirmation for T1'):
            self.board._validate_sensor_state(
                GCmd(), [True, False, True, True], None)

    def test_failed_verification_turns_off_heaters(self):
        commands = []
        self.board._run = commands.append
        self.board._pause_ms = mock.Mock()
        self.board.sensor_settle_ms = 0
        self.board._sensor_state = mock.Mock(return_value=(
            [True, False, True, True], [False] * 4, None))

        with self.assertRaisesRegex(RuntimeError, 'Missing dock confirmation'):
            self.board._verify(GCmd(), None, parked=1)

        self.assertIn('TURN_OFF_HEATERS', commands)

    def test_attached_calibration_rolls_back_if_dock_fails(self):
        original = [list(values) for values in self.board.measurements]
        self.board._preflight = mock.Mock(return_value=([True] * 4, [], 0))
        self.board._with_motion = lambda gcmd, operation: operation()
        self.board._run = mock.Mock()
        self.board._apply_offsets = mock.Mock()
        self.board._calibrate_tool_xy = mock.Mock(
            side_effect=lambda gcmd, tool: self.board.measurements[tool].__setitem__(0, 99.))
        self.board._dock = mock.Mock(side_effect=RuntimeError('dock failed'))

        with self.assertRaisesRegex(RuntimeError, 'dock failed'):
            self.board.cmd_calibrate_attached(
                GCmd(BUILDPLATE_REMOVED=1))

        self.assertEqual(self.board.measurements, original)

    def test_move_rejects_non_finite_coordinates(self):
        self.board.printer.command_error = RuntimeError
        with self.assertRaisesRegex(RuntimeError, 'must be finite'):
            self.board._move(x=float('nan'))

    def test_machine_move_bypasses_gcode_and_resyncs_position(self):
        toolhead, gcode_move = mock.Mock(), mock.Mock()
        self.board.printer.lookup_object.side_effect = {
            'toolhead': toolhead, 'gcode_move': gcode_move}.__getitem__
        self.board._run = mock.Mock()
        self.board._move(x=298., y=50., feed=1200)
        toolhead.manual_move.assert_called_once_with([298., 50., None], 20.)
        gcode_move.reset_last_position.assert_called_once_with()
        self.board._run.assert_not_called()
        toolhead.manual_move.side_effect = RuntimeError('move rejected')
        with self.assertRaisesRegex(RuntimeError, 'move rejected'):
            self.board._move(x=299.)
        self.assertEqual(gcode_move.reset_last_position.call_count, 2)

    def test_purge_coordinates_can_keep_print_transform(self):
        self.board._run = mock.Mock()
        self.board._move(z=1., feed=600, machine=False)
        self.board._run.assert_called_once_with('G1 Z1.0000 F600')

    def test_all_offsets_failure_rolls_back_entire_batch(self):
        original = [list(v) for v in self.board.measurements]
        station = self.board.station_x
        self.board._preflight = mock.Mock()
        self.board.configfile = mock.Mock()
        commands = []
        def run(command):
            commands.append(command)
            if command.startswith('C5_LEVELBOARD_REFERENCE'):
                self.board.station_x += 1.
            if command.startswith('C5_TOOL_OFFSET_CALIBRATE'):
                self.assertIn('SAVE=0', command)
                self.board.measurements[0][0] += .1
                if 'T=2 ' in command:
                    raise RuntimeError('T2 failed')
        self.board._run = run
        self.board.cmd_save_offsets_json = mock.Mock()
        with self.assertRaisesRegex(RuntimeError, 'T2 failed'):
            self.board.cmd_calibrate_all_offsets(GCmd(
                BUILDPLATE_REMOVED=1, SAVE=1))
        self.assertEqual(self.board.measurements, original)
        self.assertEqual(self.board.station_x, station)
        self.board.configfile.set.assert_not_called()
        self.board.cmd_save_offsets_json.assert_not_called()
        self.assertFalse(any('TOOL=extruder3' in cmd for cmd in commands))

    def test_all_offsets_json_failure_also_rolls_back(self):
        original = [list(v) for v in self.board.measurements]
        self.board._preflight = mock.Mock()
        self.board.configfile = mock.Mock()
        def run(command):
            if command.startswith('C5_TOOL_OFFSET_CALIBRATE'):
                self.board.measurements[0][0] += .1
        self.board._run = run
        self.board.cmd_save_offsets_json = mock.Mock(
            side_effect=RuntimeError('disk full'))
        with self.assertRaisesRegex(RuntimeError, 'disk full'):
            self.board.cmd_calibrate_all_offsets(GCmd(
                BUILDPLATE_REMOVED=1, SAVE=1))
        self.assertEqual(self.board.measurements, original)
        self.board.configfile.set.assert_not_called()

    def test_all_offsets_saves_once_after_final_dock(self):
        self.board._preflight = mock.Mock()
        self.board._run = mock.Mock()
        self.board.configfile = mock.Mock()
        def save(command):
            self.board._run.assert_called_with('AFC_UNSELECT_TOOL')
            self.assertEqual(sum('C5_TOOL_OFFSET_CALIBRATE' in c.args[0]
                                 for c in self.board._run.call_args_list), 4)
        self.board.cmd_save_offsets_json = mock.Mock(side_effect=save)
        self.board.cmd_calibrate_all_offsets(GCmd(
            BUILDPLATE_REMOVED=1, SAVE=1))
        self.board.cmd_save_offsets_json.assert_called_once()
        self.board.configfile.set.assert_not_called()

    def test_load_check_requires_attached_and_matching_active_hotend(self):
        self.board._preflight = mock.Mock(return_value=([], [], 1))
        active = self.board.printer.lookup_object.return_value.get_extruder()
        active.get_name.return_value = 'extruder1'
        self.board.cmd_check_load_tool(GCmd(T=1))
        with self.assertRaisesRegex(RuntimeError, 'requested tool'):
            self.board.cmd_check_load_tool(GCmd(T=0))
        active.get_name.return_value = 'extruder'
        with self.assertRaisesRegex(RuntimeError, 'Active hotend'):
            self.board.cmd_check_load_tool(GCmd(T=1))


if __name__ == '__main__':
    unittest.main()
