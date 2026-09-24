# Creator 5 endstop probing along X, Y or Z (tool calibrations)
#
# Copyright (C) 2026  wondercrash
#                     <155874349+wondercrash@users.noreply.github.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.
from . import homing

REBASELINE_TRIES = 6

class C5EndstopProbe:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.name = config.get_name().split()[-1]
        self.axes = config.get('axes').strip().lower()
        if not self.axes or any(a not in 'xyz' for a in self.axes):
            raise config.error("axes in [%s] must be letters from xyz"
                               % (config.get_name(),))
        self.speed = config.getfloat('speed', 5., above=0.)
        self.retract = config.getfloat('retract', 3., minval=0.)
        self.samples = config.getint('samples', 3, minval=1)
        self.tolerance = config.getfloat('samples_tolerance', 0.02, minval=0.)
        self.retries = config.getint('samples_tolerance_retries', 10,
                                     minval=0)
        ppins = self.printer.lookup_object('pins')
        self.mcu_endstop = ppins.setup_pin('endstop', config.get('pin'))
        self.printer.register_event_handler('klippy:mcu_identify',
                                            self._handle_mcu_identify)
        query_endstops = self.printer.load_object(config, 'query_endstops')
        query_endstops.register_endstop(self.mcu_endstop, self.name)
        self.basic_param_cmd = None
        if config.getboolean('rebaseline', False):
            self.mcu_endstop.get_mcu().register_config_callback(
                self._build_config)
        self.last_result = 0.
        gcode = self.printer.lookup_object('gcode')
        gcode.register_mux_command('ENDSTOP_PROBE', 'NAME', self.name,
                                   self.cmd_ENDSTOP_PROBE,
                                   desc=self.cmd_ENDSTOP_PROBE_help)
    def _build_config(self):
        self.basic_param_cmd = self.mcu_endstop.get_mcu().lookup_query_command(
            "get_basic_param num=%u", "param_value value=%u reserve=%u")
    def _handle_mcu_identify(self):
        kin = self.printer.lookup_object('toolhead').get_kinematics()
        for stepper in kin.get_steppers():
            if any(stepper.is_active_axis(a) for a in self.axes):
                self.mcu_endstop.add_stepper(stepper)
    def _ensure_clear(self, toolhead):
        # A levelBoard eddy sensor that starts out triggered is re-baselined
        for i in range(REBASELINE_TRIES):
            print_time = toolhead.get_last_move_time()
            if not self.mcu_endstop.query_endstop(print_time):
                return
            if self.basic_param_cmd is None:
                break
            self.basic_param_cmd.send([0])
            toolhead.dwell(1.)
        raise self.printer.command_error(
            "%s triggered prior to movement" % (self.name,))
    def _probing_move(self, pos):
        hmove = homing.HomingMove(self.printer, [(self.mcu_endstop, "probe")])
        try:
            epos = hmove.homing_move(pos, self.speed, probe_pos=True)
        except self.printer.command_error:
            if self.printer.is_shutdown():
                raise self.printer.command_error(
                    "Probing failed due to printer shutdown")
            raise
        if hmove.check_no_movement() is not None:
            return None
        return epos
    def _sample(self, axis, target):
        toolhead = self.printer.lookup_object('toolhead')
        gcode = self.printer.lookup_object('gcode')
        pos = toolhead.get_position()
        start = pos[axis]
        pos[axis] = target
        # Stock e_stop discards a sample that triggers before the axis moves
        # and probes again; the levelBoard sensor can report clear to a query
        # and still trigger as soon as it is armed.
        for i in range(REBASELINE_TRIES):
            self._ensure_clear(toolhead)
            epos = self._probing_move(pos)
            if epos is not None:
                break
            gcode.respond_info("%s: triggered before moving, probing again"
                               % (self.name,))
            if self.basic_param_cmd is not None:
                self.basic_param_cmd.send([0])
                toolhead.dwell(1.)
        else:
            raise self.printer.command_error(
                "%s triggered prior to movement" % (self.name,))
        if self.retract:
            coord = [None, None, None]
            coord[axis] = epos[axis] + (self.retract if target < start
                                        else -self.retract)
            toolhead.manual_move(coord, self.speed)
        return epos[axis]
    cmd_ENDSTOP_PROBE_help = "Move one axis until the endstop triggers"
    def cmd_ENDSTOP_PROBE(self, gcmd):
        axis_name = gcmd.get('AXIS').lower()
        if len(axis_name) != 1 or axis_name not in self.axes:
            raise gcmd.error("%s: AXIS must be one of %s"
                             % (self.name, self.axes.upper()))
        axis = 'xyz'.index(axis_name)
        target = gcmd.get_float('TARGET')
        results = []
        retries = 0
        while len(results) < self.samples:
            results.append(self._sample(axis, target))
            if max(results) - min(results) > self.tolerance:
                if retries >= self.retries:
                    raise gcmd.error("%s: samples exceed samples_tolerance"
                                     % (self.name,))
                retries += 1
                results = []
        self.last_result = sum(results) / len(results)
        gcmd.respond_info("%s: %s=%.4f"
                          % (self.name, axis_name.upper(), self.last_result))
    def get_status(self, eventtime):
        return {'last_result': self.last_result}

def load_config_prefix(config):
    return C5EndstopProbe(config)
