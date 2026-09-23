import os
import re
import logging

LOG = '/usr/data/logs/firmwareExe.log'

class FFAdaptiveMesh:
    def __init__(self, config):
        self.printer = config.get_printer()
        gcode = self.printer.lookup_object('gcode')
        gcode.register_command(
            "FF_PREPARSE_OBJECTS", self.cmd_FF_PREPARSE_OBJECTS,
            desc="Pre-parse gcode file for EXCLUDE_OBJECT_DEFINE")

    def _find_gcode_file(self):
        try:
            with open(LOG, 'r', errors='replace') as f:
                lines = f.readlines()
        except Exception:
            logging.exception("ff_adaptive_mesh: error reading log")
            return None
        # find newest print path
        print_path = None
        for line in reversed(lines):
            m = re.search(r'print path:\s*(\S+)', line)
            if m:
                print_path = m.group(1).rstrip(',')
                break
        if not print_path:
            return None
        # if not 3mf, use directly
        if not print_path.endswith('.3mf'):
            if os.path.isfile(print_path):
                return print_path
            return None
        # find the extracted gcode from the 3mf
        for line in reversed(lines):
            m = re.search(r'Successfully extracted:(\S+\.gcode)', line)
            if m:
                path = m.group(1)
                if os.path.isfile(path):
                    return path
                return None
        return None

    def cmd_FF_PREPARSE_OBJECTS(self, gcmd):
        exclude_obj = self.printer.lookup_object('exclude_object', None)
        if exclude_obj is None:
            gcmd.respond_info("ff_adaptive_mesh: [exclude_object] not found")
            return
        if exclude_obj.get_status(None).get("objects", []):
            return
        filepath = self._find_gcode_file()
        if not filepath:
            gcmd.respond_info("ff_adaptive_mesh: no gcode file found")
            return
        gcmd.respond_info("ff_adaptive_mesh: parsing %s"
                          % os.path.basename(filepath))
        objects = []
        try:
            with open(filepath, 'r') as f:
                for i, line in enumerate(f):
                    if i > 500:
                        break
                    line = line.strip()
                    if line.startswith('EXCLUDE_OBJECT_DEFINE '):
                        objects.append(line)
        except Exception:
            logging.exception("ff_adaptive_mesh: error reading file")
        if not objects:
            gcmd.respond_info("ff_adaptive_mesh: no EXCLUDE_OBJECT_DEFINE found. "
                              "Enable 'Exclude objects' in OrcaSlicer.")
            return
        for line in objects:
            try:
                self.printer.lookup_object('gcode').run_script_from_command(line)
            except Exception:
                logging.exception("ff_adaptive_mesh: error registering object")
        gcmd.respond_info("ff_adaptive_mesh: registered %d objects" % len(objects))

def load_config(config):
    return FFAdaptiveMesh(config)