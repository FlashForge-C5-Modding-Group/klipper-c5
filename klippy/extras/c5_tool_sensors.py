# Creator 5 tool dock/mount sensor wait (tool changes)
#
# Copyright (C) 2026  wondercrash
#                     <155874349+wondercrash@users.noreply.github.com>
#
# This file may be distributed under the terms of the GNU GPLv3 license.

POLL_TIME = 0.010

class C5ToolSensors:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.dock_names = config.getlist('dock_buttons')
        self.mount_names = config.getlist('mount_buttons')
        self.timeout = config.getfloat('timeout', 1., above=0.)
        self.docks = self.mounts = []
        self.printer.register_event_handler('klippy:connect',
                                            self._handle_connect)
        gcode = self.printer.lookup_object('gcode')
        gcode.register_command('TOOL_SENSOR_WAIT', self.cmd_TOOL_SENSOR_WAIT,
                               desc=self.cmd_TOOL_SENSOR_WAIT_help)
    def _handle_connect(self):
        def lookup(name):
            return self.printer.lookup_object('gcode_button ' + name.strip())
        self.docks = [lookup(n) for n in self.dock_names]
        self.mounts = [lookup(n) for n in self.mount_names]
    def _pressed(self, button):
        return button.get_status()['state'] == 'PRESSED'
    def _matches(self, tool, held, docked):
        if (held is not None
            and any(self._pressed(b) for b in self.mounts) != held):
            return False
        if docked is not None and self._pressed(self.docks[tool]) != docked:
            return False
        return True
    cmd_TOOL_SENSOR_WAIT_help = "Wait for the tool dock/mount sensors"
    def cmd_TOOL_SENSOR_WAIT(self, gcmd):
        held = gcmd.get_int('HELD', None, minval=0, maxval=1)
        docked = gcmd.get_int('DOCKED', None, minval=0, maxval=1)
        tool = gcmd.get_int('TOOL', 0, minval=0, maxval=len(self.docks) - 1)
        timeout = gcmd.get_float('TIMEOUT', self.timeout, minval=0.)
        if held is not None:
            held = bool(held)
        if docked is not None:
            docked = bool(docked)
        self.printer.lookup_object('toolhead').wait_moves()
        eventtime = self.reactor.monotonic()
        deadline = eventtime + timeout
        while not self._matches(tool, held, docked):
            if eventtime >= deadline:
                return
            eventtime = self.reactor.pause(eventtime + POLL_TIME)

def load_config(config):
    return C5ToolSensors(config)
