# Creator 5 mainBoardGD motor-control (mclib) configuration
#
# Copyright (C) 2026
#
# This file may be distributed under the terms of the GNU GPLv3 license.

MAX_CURRENT = 4.0
MICROSTEPS = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4, 32: 5, 64: 6, 128: 7, 256: 8}
RESONANCE_SLOTS = (1, 2, 4)

def milli(value):
    return int(round(value * 1000.))

class MCLIB:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.stepper_name = config.get_name().split(None, 1)[-1]
        if not config.has_section(self.stepper_name):
            raise config.error(
                "Could not find config section '[%s]' required by mclib"
                % (self.stepper_name,))
        sconfig = config.getsection(self.stepper_name)
        self.mstep = sconfig.getchoice('microsteps', MICROSTEPS)
        self.interpolate = config.getboolean('interpolate', True)
        # Firmware units: milliohm, microhenry, micro-(N*m/A)
        self.rs = milli(config.getfloat('motor_rs', above=0.))
        self.ls = int(round(config.getfloat('motor_ls', above=0.) * 1000000.))
        self.km = int(round(config.getfloat('motor_km', above=0.) * 1000000.))
        bus_voltage = config.getfloat('bus_voltage', above=0.)
        run_current = config.getfloat('run_current', 1., above=0.,
                                      maxval=MAX_CURRENT)
        hold_current = config.getfloat('hold_current', run_current,
                                       minval=0., maxval=run_current)
        self.run_current = milli(run_current)
        self.hold_current = milli(hold_current)
        # Stock default: the bus voltage in millivolts
        self.stall_threshold = milli(config.getfloat(
            'stall_threshold', bus_voltage * 1000., above=0.))
        self.resonance = {}
        for tdx in RESONANCE_SLOTS:
            amp = config.getfloat('td%d_amp' % (tdx,), 0., minval=0.,
                                  maxval=run_current)
            phase1 = config.getfloat('td%d_phase1' % (tdx,), 0., minval=0.)
            phase2 = config.getfloat('td%d_phase2' % (tdx,), 0., minval=0.)
            self.resonance[tdx] = (milli(amp), milli(phase1), milli(phase2))
        self.mcu = self.oid = None
        self.set_current_cmd = self.set_resonance_damp_cmd = None
        # Steppers register with force_move as they are created
        self.force_move = self.printer.load_object(config, 'force_move')
        self.printer.register_event_handler("klippy:mcu_identify",
                                            self._handle_mcu_identify)
        gcode = self.printer.lookup_object('gcode')
        gcode.register_mux_command("MCLIB_SET_CURRENT", "STEPPER",
                                   self.stepper_name,
                                   self.cmd_MCLIB_SET_CURRENT,
                                   desc=self.cmd_MCLIB_SET_CURRENT_help)
        gcode.register_mux_command("MCLIB_SET_RESONANCE_DAMP", "STEPPER",
                                   self.stepper_name,
                                   self.cmd_MCLIB_SET_RESONANCE_DAMP,
                                   desc=self.cmd_MCLIB_SET_RESONANCE_DAMP_help)
    def _handle_mcu_identify(self):
        stepper = self.force_move.lookup_stepper(self.stepper_name)
        self.mcu = stepper.get_mcu()
        motors = self.mcu.get_enumerations().get('stepper', {})
        if self.stepper_name not in motors:
            raise self.printer.config_error(
                "mcu '%s' has no mclib motor for '%s'"
                % (self.mcu.get_name(), self.stepper_name))
        self.oid = self.mcu.create_oid()
        self.mcu.register_config_callback(self._build_config)
    def _build_config(self):
        mcu, oid = self.mcu, self.oid
        mcu.add_config_cmd("config_mclib oid=%d stepper=%s rs=%d ls=%d km=%d"
                           % (oid, self.stepper_name, self.rs, self.ls,
                              self.km))
        mcu.add_config_cmd("mclib_config_microstep oid=%d interpolate=%d"
                           " mstep=%d" % (oid, self.interpolate, self.mstep))
        mcu.add_config_cmd("mclib_set_current oid=%d run_current=%d"
                           " hold_current=%d"
                           % (oid, self.run_current, self.hold_current))
        mcu.add_config_cmd("mclib_config_stalldetect oid=%d stallthrs=%d"
                           % (oid, self.stall_threshold))
        for tdx in RESONANCE_SLOTS:
            amp, phase1, phase2 = self.resonance[tdx]
            mcu.add_config_cmd("mclib_set_resonance_damp oid=%d tdx=%d amp=%d"
                               " phase1=%d phase2=%d"
                               % (oid, tdx, amp, phase1, phase2))
        self.set_current_cmd = mcu.lookup_command(
            "mclib_set_current oid=%c run_current=%u hold_current=%u")
        self.set_resonance_damp_cmd = mcu.lookup_command(
            "mclib_set_resonance_damp oid=%c tdx=%c amp=%u"
            " phase1=%u phase2=%u")
    cmd_MCLIB_SET_CURRENT_help = "Set the run and hold current of a motor"
    def cmd_MCLIB_SET_CURRENT(self, gcmd):
        run_current = gcmd.get_float('CURRENT', self.run_current / 1000.,
                                     above=0., maxval=MAX_CURRENT)
        prev_hold = min(self.hold_current / 1000., run_current)
        hold_current = gcmd.get_float('HOLDCURRENT', prev_hold, minval=0.,
                                      maxval=run_current)
        self.run_current = milli(run_current)
        self.hold_current = milli(hold_current)
        self.set_current_cmd.send([self.oid, self.run_current,
                                   self.hold_current])
        gcmd.respond_info("%s: run_current=%.3f hold_current=%.3f"
                          % (self.stepper_name, self.run_current / 1000.,
                             self.hold_current / 1000.))
    cmd_MCLIB_SET_RESONANCE_DAMP_help = (
        "Set the resonance damping of an mclib motor")
    def cmd_MCLIB_SET_RESONANCE_DAMP(self, gcmd):
        tdx = gcmd.get_int('TDX', 1)
        if tdx not in RESONANCE_SLOTS:
            raise gcmd.error("TDX must be 1, 2 or 4")
        amp, phase1, phase2 = self.resonance[tdx]
        amp = milli(gcmd.get_float('AMP', amp / 1000., minval=0.,
                                   maxval=MAX_CURRENT))
        phase1 = milli(gcmd.get_float('PHASE1', phase1 / 1000., minval=0.))
        phase2 = milli(gcmd.get_float('PHASE2', phase2 / 1000., minval=0.))
        self.resonance[tdx] = (amp, phase1, phase2)
        self.set_resonance_damp_cmd.send([self.oid, tdx, amp, phase1, phase2])
        gcmd.respond_info("%s: td%d amp=%.3f phase1=%.3f phase2=%.3f"
                          % (self.stepper_name, tdx, amp / 1000.,
                             phase1 / 1000., phase2 / 1000.))

def load_config_prefix(config):
    return MCLIB(config)
