# Creator 5 host PWM buzzer, as used by firmwareExe's buzzerPlay class.
# Copyright (C) 2026
# This file may be distributed under the terms of the GNU GPLv3 license.
import subprocess


class Creator5Beeper:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.gcode = self.printer.lookup_object('gcode')
        self.command = config.get('command', 'cmd_pwm')
        self.channel = config.get('channel', 'pc12')
        aio = self.printer.load_object(config, 'aio_executor')
        self.executor = aio.allocate_executor('creator5_beeper')
        self.gcode.register_command('C5_BUZZER', self.cmd_C5_BUZZER,
                                    desc='Sound the Creator 5 host PWM buzzer')

    def _run(self, *args):
        try:
            result = self.executor.submit(
                                    subprocess.run,
                                    [self.command] + list(args),
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,
                                    check=False, timeout=3)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise self.printer.command_error(
                'Creator 5 buzzer command failed: %s' % (exc,))
        if result.returncode:
            detail = result.stderr.decode('utf-8', 'replace').strip()
            raise self.printer.command_error(
                'Creator 5 buzzer command failed (%d): %s'
                % (result.returncode, detail))

    def cmd_C5_BUZZER(self, gcmd):
        duration_ms = gcmd.get_int('DURATION', 5000, minval=1,
                                   maxval=30000)
        frequency = gcmd.get_int('FREQUENCY', 2500, minval=20, maxval=20000)
        level = gcmd.get_int('LEVEL', 100, minval=0, maxval=100)
        optional = gcmd.get_int('OPTIONAL', 0, minval=0, maxval=1)
        try:
            self._play_tone(duration_ms, frequency, level)
        except self.printer.command_error as exc:
            if not optional:
                raise
            gcmd.respond_info('Creator 5 buzzer unavailable; tune skipped: %s'
                              % (exc,))

    def _play_tone(self, duration_ms, frequency, level):
        # The factory application configures a 50 MHz base and prescale 6.
        # set_wc takes period/high counts at that base, not a frequency in Hz.
        period = max(2, int(round(50000000. / frequency)))
        # LEVEL is a duty-based intensity percentage: 100 preserves the
        # factory 50% duty tone; 0 is silent. Perceived volume is not linear.
        high = period * level // 200
        self._run('config', self.channel, 'freq=50000000', 'max_level=300',
                  'active_level=1', 'accuracy_priority=freq')
        try:
            self._run('set_level', self.channel, '100')
            self._run('set_prescale', self.channel, '6')
            # Program the intended waveform before enabling the channel.
            self._run('set_wc', self.channel, str(period), str(high))
            self._run('enable_channels', self.channel)
            self.reactor.pause(self.reactor.monotonic() + duration_ms / 1000.)
        finally:
            try:
                self._run('set_wc', self.channel, '1', '0')
            finally:
                self._run('disable_channels', self.channel)


def load_config(config):
    return Creator5Beeper(config)
