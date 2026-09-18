#!/usr/bin/env python3
# This file may be distributed under the terms of the GNU GPLv3 license.
"""Build and validate a Creator 5 Pro levelBoard firmware update."""

import argparse
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

REPO_ROOT = Path(__file__).resolve().parents[1]
APP_START = 0x08004000
APP_END = 0x08010000
RAM_START = 0x20000000
RAM_END = 0x20004000
NORMALIZATION = "application-48k-ff-fill-v1"
SEED_CONFIG = (
    "CONFIG_MACH_STM32=y\n"
    "CONFIG_MACH_N32G430F8S7=y\n"
    "CONFIG_STM32_CLOCK_REF_8M=y\n"
    "CONFIG_SERIAL=y\n"
    "CONFIG_STM32_SERIAL_USART1=y\n"
    "CONFIG_C5_LEVELBOARD=y\n"
)


class ToolError(Exception):
    def __init__(self, message, exit_code=2):
        super().__init__(message)
        self.exit_code = exit_code


def sanitize_diagnostic(message, secret=None):
    text = str(message)
    if secret:
        text = text.replace(secret, "[redacted]")
    text = re.sub(
    r"(?i)(?:[a-z]:[\\/]|/)(?:[^\s:'\"]+[\\/])+[^\s:'\"]*",
    "[redacted]",
     text)
    return text


def _inside(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_output_root(path):
    try:
        path = Path(path).expanduser().resolve(strict=False)
        repo = REPO_ROOT.resolve()
    except (OSError, RuntimeError):
        raise ToolError("unable to resolve output path")
    literal_out = repo / "out"
    if (path == Path(path.anchor) or path == repo or
            (_inside(path, repo) and not _inside(path, literal_out))):
        raise ToolError(
            "output must be outside the repository or beneath its "
            "out directory")
    return path


def _hex_context(line_number, message):
    raise ToolError("Intel HEX line %d: %s" % (line_number, message))


def parse_ihex(data):
    """Parse and strictly validate a levelBoard Intel HEX byte string."""
    if isinstance(data, str):
        try:
            data = data.encode("ascii")
        except UnicodeEncodeError:
            raise ToolError("Intel HEX input is not ASCII")
    try:
        text = bytes(data).decode("ascii")
    except UnicodeDecodeError:
        raise ToolError("Intel HEX input is not ASCII")
    if not text:
        raise ToolError("Intel HEX input is empty")
    # splitlines keeps line-ending policy explicit and exposes internal blanks.
    lines = text.splitlines(keepends=True)
    if not lines or not lines[-1].endswith(("\n", "\r")):
        # A final record without an ending newline is legal.
        pass
    memory = {}
    linear_base = 0
    eof_seen = False
    start_linear = None
    for number, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n")
        endings = raw[len(line):]
        if endings not in ("", "\n", "\r\n"):
            _hex_context(number, "unsupported line ending")
        if not line:
            _hex_context(number, "blank internal record")
        if line != line.strip():
            _hex_context(number, "whitespace outside line ending")
        if eof_seen:
            _hex_context(number, "record after EOF")
        if not line.startswith(":"):
            _hex_context(number, "record does not start with ':'")
        encoded = line[1:]
        if len(encoded) % 2:
            _hex_context(number, "odd hexadecimal length")
        if not re.fullmatch(r"[0-9A-Fa-f]+", encoded or "-"):
            _hex_context(number, "non-hexadecimal record")
        record_bytes = bytes.fromhex(encoded)
        if len(record_bytes) < 5:
            _hex_context(number, "record is too short")
        count = record_bytes[0]
        if len(record_bytes) != count + 5:
            _hex_context(number, "record length does not match byte count")
        if sum(record_bytes) & 0xff:
            _hex_context(number, "checksum mismatch")
        address = (record_bytes[1] << 8) | record_bytes[2]
        kind = record_bytes[3]
        payload = record_bytes[4:-1]
        if kind == 0:
            end_offset = address + count
            if end_offset > 0x10000:
                _hex_context(number, "data crosses a 64 KiB address window")
            absolute = linear_base + address
            end_absolute = absolute + count
            if end_absolute > 0x100000000:
                _hex_context(number, "32-bit address arithmetic overflow")
            if count and (absolute < APP_START or end_absolute > APP_END):
                _hex_context(
    number, "data address range 0x%08x..0x%08x is outside application range" %
     (absolute, end_absolute - 1))
            for offset, value in enumerate(payload):
                current = absolute + offset
                previous = memory.get(current)
                if previous is not None and previous != value:
                    _hex_context(
    number,
    "conflicting overlap at 0x%08x" %
     current)
                memory[current] = value
        elif kind == 1:
            if count or address:
                _hex_context(number, "EOF must have zero length and address")
            eof_seen = True
        elif kind == 4:
            if count != 2:
                _hex_context(
    number, "extended linear address record length must be 2")
            if address:
                _hex_context(
    number, "extended linear address record address must be zero")
            linear_base = int.from_bytes(payload, "big") << 16
        elif kind == 5:
            if count != 4:
                _hex_context(
    number, "start linear address record length must be 4")
            if address:
                _hex_context(
    number, "start linear address record address must be zero")
            if start_linear is not None:
                _hex_context(number, "duplicate start linear address record")
            start_linear = int.from_bytes(payload, "big")
            start_target = start_linear & ~1
            if not APP_START <= start_target < APP_END:
                _hex_context(
    number, "start linear address is outside application range")
        elif kind in (2, 3):
            _hex_context(
    number,
    "unsupported Intel HEX record type %02x" %
     kind)
        else:
            _hex_context(number, "unknown Intel HEX record type %02x" % kind)
    if not eof_seen:
        raise ToolError("Intel HEX is missing EOF record")
    if not memory:
        raise ToolError("Intel HEX contains no application data")
    missing_vectors = [
    address for address in range(
        APP_START,
        APP_START +
         8) if address not in memory]
    if missing_vectors:
        raise ToolError(
            "Intel HEX is missing vector bytes at application base")
    stack_pointer, reset_handler = struct.unpack(
    "<II", bytes(
        memory[address] for address in range(
            APP_START, APP_START + 8)))
    if not RAM_START <= stack_pointer <= RAM_END:
        raise ToolError("stack pointer 0x%08x is outside SRAM" % stack_pointer)
    if stack_pointer & 7:
        raise ToolError(
    "stack pointer 0x%08x is not eight-byte aligned" %
     stack_pointer)
    if not reset_handler & 1:
        raise ToolError(
    "reset handler 0x%08x does not set the Thumb bit" %
     reset_handler)
    reset_target = reset_handler & ~1
    if not APP_START <= reset_target < APP_END:
        raise ToolError(
    "reset target 0x%08x is outside application range" %
     reset_target)
    if reset_target not in memory:
        raise ToolError(
    "reset target 0x%08x is not mapped executable data" %
     reset_target)
    addresses = sorted(memory)
    intervals = []
    start = previous = addresses[0]
    for address in addresses[1:]:
        if address != previous + 1:
            intervals.append([start, previous + 1])
            start = address
        previous = address
    intervals.append([start, previous + 1])
    holes = [[left[1], right[0]] for left, right in zip(
        intervals, intervals[1:]) if left[1] != right[0]]
    if intervals[-1][1] < APP_END:
        holes.append([intervals[-1][1], APP_END])
    normalized = bytearray(b"\xff" * (APP_END - APP_START))
    for address, value in memory.items():
        normalized[address - APP_START] = value
    return {
        "memory": memory,
        "mapped_byte_count": len(memory),
        "intervals": intervals,
        "holes": holes,
        "stack_pointer": stack_pointer,
        "reset_handler": reset_handler,
        "reset_target": reset_target,
        "start_linear_address": start_linear,
        "normalization": NORMALIZATION,
        "normalized_sha256": hashlib.sha256(normalized).hexdigest(),
    }


REQUIRED_CONFIG = {
    "MACH_STM32": "y",
    "MACH_N32G430F8S7": "y",
    "C5_LEVELBOARD": "y",
    "MCU": "stm32f103xe",
    "STM32_SERIAL_USART1": "y",
    "FLASH_APPLICATION_ADDRESS": "0x08004000",
    "FLASH_BOOT_ADDRESS": "0x08000000",
    "FLASH_SIZE": "0x10000",
    "RAM_START": "0x20000000",
    "RAM_SIZE": "0x4000",
    "CLOCK_FREQ": "128000000",
    "CLOCK_REF_FREQ": "8000000",
    "SERIAL_BAUD": "230400",
    "SERIAL_RX_BUFFER_SIZE": "384",
}

REQUIRED_CONSTANTS = {
    "ADC_MAX": 4095,
    "CLOCK_FREQ": 128000000,
    "RECEIVE_WINDOW": 384,
    "RESERVE_PINS_serial": "PH10,PH9",
    "SERIAL_BAUD": 230400,
    "STATS_SUMSQ_BASE": 256,
}

REQUIRED_COMMANDS = {
    "identify offset=%u count=%c",
    "set_trigger_threshold threshold=%i",
    "get_mcu_version",
    "get_basic_param num=%u",
    "remove_peel action=%u",
    "clear_shutdown",
    "emergency_stop",
    "get_uptime",
    "get_clock",
    "finalize_config crc=%u",
    "get_config",
    "allocate_oids count=%c",
    "stepper_stop_on_trigger oid=%c trsync_oid=%c",
    "endstop_recover_state oid=%c",
    "endstop_query_state oid=%c",
    "endstop_home oid=%c clock=%u sample_ticks=%u sample_count=%c "
    "rest_ticks=%u pin_value=%c trsync_oid=%c trigger_reason=%c",
    "config_endstop oid=%c pin=%c pull_up=%c",
    "trsync_trigger oid=%c reason=%c",
    "trsync_set_timeout oid=%c clock=%u",
    "trsync_start oid=%c report_clock=%u report_ticks=%u expire_reason=%c",
    "config_trsync oid=%c",
    "reset",
}

REQUIRED_RESPONSES = {
    "identify_response offset=%u data=%.*s",
    "trigger_threshold threshold=%i",
    "mcu_version year=%u date=%u version=%u",
    "param_value value=%u reserve=%u",
    "peel_data value=%i",
    "uptime high=%u clock=%u",
    "clock clock=%u",
    "config is_config=%c crc=%u is_shutdown=%c move_count=%hu",
    "endstop_state oid=%c homing=%c next_clock=%u pin_value=%c",
    "trsync_state oid=%c can_trigger=%c trigger_reason=%c clock=%u",
    "starting",
    "is_shutdown static_string_id=%hu",
    "shutdown clock=%u static_string_id=%hu",
}


def _unsupported_build_path(value):
    return re.fullmatch(r"[A-Za-z0-9_./+\-]+", str(value)) is None


def _tool_path(name):
    try:
        found = shutil.which(name)
        path = str(Path(found).resolve()) if found else None
    except (OSError, RuntimeError):
        path = None
    if not path:
        raise ToolError("required external tool is unavailable: %s" % name, 3)
    if _unsupported_build_path(path):
        raise ToolError("unsupported build path for %s" % name)
    return path


def _run(command, logical_name, cwd=REPO_ROOT, env=None):
    try:
        result = subprocess.run(
    command,
    cwd=cwd,
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
     check=False)
    except OSError as exc:
        raise ToolError("unable to run %s: %s" % (logical_name, exc), 3)
    if result.returncode:
        diagnostic = result.stderr.decode("utf-8", "replace").strip()
        if not diagnostic:
            diagnostic = result.stdout.decode("utf-8", "replace").strip()
        diagnostic = sanitize_diagnostic(diagnostic)
        if len(diagnostic) > 1000:
            diagnostic = diagnostic[-1000:]
        raise ToolError("%s failed%s" % (logical_name,
                        ": " + diagnostic if diagnostic else ""), 3)
    return result.stdout


def _version(program):
    try:
        result = subprocess.run([program,
    "--version"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    check=False,
     timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    if result.returncode:
        return "unavailable"
    line = result.stdout.decode("utf-8", "replace").splitlines()
    if not line:
        return "unavailable"
    value = line[0].strip()
    base = Path(program).name
    value = re.sub(r"^.*[\\/]" + re.escape(base) + r"\s*", "", value)
    return value[:240]


def _load_resolved_config(config_path):
    module_path = REPO_ROOT / "lib" / "kconfiglib"
    sys.path.insert(0, str(module_path))
    old_srctree = os.environ.get("srctree")
    os.environ["srctree"] = str(REPO_ROOT)
    try:
        import kconfiglib
        kconf = kconfiglib.Kconfig(str(REPO_ROOT / "src" / "Kconfig"),
                                   warn=False)
        kconf.load_config(str(config_path))
        values = {}
        for name in REQUIRED_CONFIG:
            symbol = kconf.syms.get(name)
            if symbol is None:
                raise ToolError(
    "required Kconfig symbol is missing: %s" %
     name)
            values[name] = symbol.str_value
        for forbidden in ("MACH_STM32F1", "MACH_N32G45x"):
            symbol = kconf.syms.get(forbidden)
            if symbol is not None and symbol.str_value == "y":
                raise ToolError(
    "incompatible hardware Kconfig selected: %s" %
     forbidden)
    except ToolError:
        raise
    except (OSError, UnicodeError, ValueError):
        raise ToolError("unable to resolve supplied Kconfig")
    finally:
        if old_srctree is None:
            os.environ.pop("srctree", None)
        else:
            os.environ["srctree"] = old_srctree
        try:
            sys.path.remove(str(module_path))
        except ValueError:
            pass
    for name, expected in REQUIRED_CONFIG.items():
        actual = values[name]
        if name in (
    "FLASH_APPLICATION_ADDRESS",
    "FLASH_BOOT_ADDRESS",
    "FLASH_SIZE",
    "RAM_START",
     "RAM_SIZE"):
            try:
                matches = int(actual, 0) == int(expected, 0)
            except ValueError:
                matches = False
        else:
            matches = actual == expected
        if not matches:
            raise ToolError("resolved Kconfig %s is %s, expected %s" %
                            (name, actual, expected))
    return values


def _load_resolved_config_text(config_text):
    if not isinstance(config_text, str) or not config_text:
        raise ToolError("dictionary Kconfig is missing or invalid")
    try:
        with tempfile.TemporaryDirectory() as temp:
            config_path = Path(temp) / ".config"
            config_path.write_text(config_text, encoding="utf-8", newline="\n")
            return _load_resolved_config(config_path)
    except OSError:
        raise ToolError("unable to validate dictionary Kconfig")


def _parse_sections(text, elf_size):
    sections = []
    lines = text.splitlines()
    pattern = re.compile(
        r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]{8,16})\s+"
        r"([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+"
        r"([0-9a-fA-F]{8,16})\s+2\*\*\d+\s*$")
    index = 0
    while index < len(lines):
        match = pattern.match(lines[index])
        if not match:
            if re.match(r"^\s*\d+\s+", lines[index]):
                raise ToolError("ambiguous objdump section output")
            index += 1
            continue
        if index + 1 >= len(lines):
            raise ToolError("ambiguous objdump section output")
        flags = {item.strip() for item in lines[index + 1].strip().split(",")}
        if not flags or any(not item for item in flags):
            raise ToolError(
    "ambiguous objdump flags for section %s" %
     match.group(1))
        section = {
            "name": match.group(1), "size": int(match.group(2), 16),
            "vma": int(match.group(3), 16), "lma": int(match.group(4), 16),
            "offset": int(match.group(5), 16), "flags": flags,
        }
        if section["size"] and "CONTENTS" in flags:
            end = section["offset"] + section["size"]
            if end < section["offset"] or end > elf_size:
                raise ToolError(
    "ELF section %s exceeds file bounds" %
     section["name"])
        sections.append(section)
        index += 2
    if not sections:
        raise ToolError("objdump reported no ELF sections")
    return sections


def _redact_dictionary_metadata(value):
    value = str(value)
    if (len(value) > 240 or any(ord(char) < 0x20 for char in value) or
            re.search(r"(?i)(?:[a-z]:[\\/]|/|\\\\)", value)):
        return "[redacted: unsafe metadata]"
    return value


def _load_dictionary(data):
    try:
        raw = json.loads(data.decode("utf-8"))
    except (AttributeError, UnicodeDecodeError, ValueError) as exc:
        raise ToolError("invalid protocol dictionary JSON: %s" % exc)
    sys.path.insert(0, str(REPO_ROOT / "klippy"))
    previous_logging_disable = logging.root.manager.disable
    try:
        import msgproto
        parser = msgproto.MessageParser()
        logging.disable(logging.CRITICAL)
        try:
            parser.process_identify(data, decompress=False)
        except Exception as exc:
            raise ToolError("invalid protocol dictionary: %s" % exc)
        finally:
            logging.disable(previous_logging_disable)
    finally:
        try:
            sys.path.remove(str(REPO_ROOT / "klippy"))
        except ValueError:
            pass
    constants = parser.get_constants()
    for name, expected in REQUIRED_CONSTANTS.items():
        if constants.get(name) != expected:
            raise ToolError(
    "dictionary constant %s does not match required value" %
     name)
    resolved_config = _load_resolved_config_text(parser.get_kconfig())
    raw_tables = {"command": raw.get("commands", {}),
                  "response": raw.get("responses", {}),
                  "output": raw.get("output", {})}
    id_bindings = {}
    name_bindings = {}
    format_bindings = {}
    for msgtype, table in raw_tables.items():
        if not isinstance(table, dict):
            raise ToolError("dictionary message table is invalid")
        for msgformat, msgid in table.items():
            if not isinstance(msgformat, str) or not isinstance(msgid, int):
                raise ToolError("dictionary message assignment is invalid")
            binding = (msgtype, msgformat)
            previous = id_bindings.setdefault(msgid, binding)
            if previous != binding:
                raise ToolError(
                    "dictionary has conflicting message ID assignments")
            previous_format = format_bindings.setdefault(msgformat,
                                                         (msgtype, msgid))
            if previous_format != (msgtype, msgid):
                raise ToolError(
                    "dictionary has conflicting message format assignments")
            if msgtype != "output":
                name = msgformat.split(" ", 1)[0]
                name_binding = (msgid, msgtype, msgformat)
                previous_name = name_bindings.setdefault(name, name_binding)
                if previous_name != name_binding:
                    raise ToolError(
                        "dictionary has conflicting non-output message names")
    classified = {}
    for msgid, msgtype, msgformat in parser.get_messages():
        if msgformat in format_bindings:
            classified[msgformat] = (msgid, msgtype)
    message_types = {"command": set(), "response": set(), "output": set()}
    for msgformat, (expected_type, expected_id) in format_bindings.items():
        if classified.get(msgformat) != (expected_id, expected_type):
            raise ToolError(
                "dictionary message membership conflicts with MessageParser")
        message_types[expected_type].add(msgformat)
    missing_commands = REQUIRED_COMMANDS - message_types["command"]
    missing_responses = REQUIRED_RESPONSES - message_types["response"]
    if missing_commands:
        raise ToolError(
    "dictionary is missing required command membership: %s" %
     sorted(missing_commands)[0])
    if missing_responses:
        raise ToolError(
    "dictionary is missing required response membership: %s" %
     sorted(missing_responses)[0])
    command_names = {item.split(" ", 1)[0]
                                for item in message_types["command"]}
    response_names = {item.split(" ", 1)[0]
                                 for item in message_types["response"]}
    for forbidden in ("config_reset", "config_analog_in", "query_analog_in"):
        if forbidden in command_names:
            raise ToolError(
    "forbidden dictionary command is present: %s" %
     forbidden)
    if "endstop_recover_state" in response_names:
        raise ToolError(
            "forbidden dictionary response is present: endstop_recover_state")
    all_formats = set().union(*message_types.values())
    if any(item.split(" ", 1)[0] == "Levelboard" for item in all_formats):
        raise ToolError("forbidden Levelboard dictionary format is present")
    if raw.get("commands", {}).get("identify offset=%u count=%c") != 1:
        raise ToolError("bootstrap identify message ID is not 1")
    if raw.get("responses", {}).get(
        "identify_response offset=%u data=%.*s") != 0:
        raise ToolError("bootstrap identify_response message ID is not 0")
    version, build_versions = parser.get_version_info()
    return {
    "constants": {
        name: constants[name] for name in sorted(REQUIRED_CONSTANTS)},
        "version": _redact_dictionary_metadata(version),
        "build_versions": _redact_dictionary_metadata(build_versions),
        "kconfig": "[redacted: validated separately]",
        "resolved_config": resolved_config,
         }


def _read_input_file(value):
    name = Path(value).name
    try:
        path = Path(value).expanduser().resolve(strict=False)
        if not path.is_file():
            raise ToolError("required input is missing: %s" % name)
        data = path.read_bytes()
    except ToolError:
        raise
    except (OSError, RuntimeError):
        raise ToolError("unable to read required input: %s" % name)
    if not data:
        raise ToolError("firmware input is empty: %s" % name)
    return path, data
def validate_firmware(firmware, elf, dictionary, cross_prefix="arm-none-eabi-",
                      openssl="openssl"):
    firmware, exact_hex = _read_input_file(firmware)
    elf, elf_data = _read_input_file(elf)
    dictionary, dictionary_data = _read_input_file(dictionary)
    image = parse_ihex(exact_hex)
    objdump = _tool_path(cross_prefix + "objdump")
    nm = _tool_path(cross_prefix + "nm")
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    file_info = _run([objdump, "-f", str(elf)], "objdump",
                     env=env).decode("ascii", "replace")
    if "file format elf32-littlearm" not in file_info or not re.search(
        r"architecture:\s*arm", file_info):
        raise ToolError(
            "ELF is not an expected ELF32 little-endian ARM executable")
    entries = re.findall(r"start address 0x([0-9a-fA-F]+)", file_info)
    if len(entries) != 1:
        raise ToolError("objdump did not report exactly one ELF entry point")
    elf_entry = int(entries[0], 16)
    if (image["start_linear_address"] is not None and
            (image["start_linear_address"] & ~1) != (elf_entry & ~1)):
        raise ToolError(
            "Intel HEX start linear address does not match ELF entry point")
    section_text = _run([objdump, "-h", str(elf)], "objdump",
                        env=env).decode("ascii", "replace")
    sections = _parse_sections(section_text, len(elf_data))
    file_backed = []
    code_ranges = []
    allowed = set()
    for section in sections:
        size = section["size"]
        flags = section["flags"]
        if not size or "ALLOC" not in flags:
            continue
        vma_end = section["vma"] + size
        in_flash = (APP_START <= section["vma"] and vma_end <= APP_END)
        in_ram = (RAM_START <= section["vma"] and vma_end <= RAM_END)
        has_load_image = "CONTENTS" in flags and "LOAD" in flags
        if has_load_image:
            if vma_end > 0x100000000 or (not in_flash and not in_ram):
                if (RAM_START <= section["vma"] <= RAM_END or
                        section["vma"] < RAM_START < vma_end):
                    raise ToolError(
    "RAM section %s crosses SRAM bounds" %
     section["name"])
                raise ToolError(
    "allocated section %s is outside flash and SRAM" %
     section["name"])
            end = section["lma"] + size
            if (end > 0x100000000 or section["lma"] < APP_START or
                    end > APP_END):
                raise ToolError(
    "ELF load section %s crosses application flash bounds" %
     section["name"])
            body = elf_data[section["offset"]:section["offset"] + size]
            for offset, value in enumerate(body):
                address = section["lma"] + offset
                if image["memory"].get(address) != value:
                    raise ToolError("ELF/HEX mismatch in %s at 0x%08x" %
                                    (section["name"], address))
                allowed.add(address)
            file_backed.append(section)
            if "CODE" in flags:
                code_ranges.append((section["vma"], vma_end))
        elif vma_end > 0x100000000 or not in_ram:
            raise ToolError(
    "RAM ALLOC/NOLOAD section %s is outside SRAM bounds" %
     section["name"])
    extra = sorted(set(image["memory"]) - allowed)
    if extra:
        raise ToolError(
    "HEX has byte outside ELF load sections at 0x%08x" %
     extra[0])
    if not any(start <= image["reset_target"] <
               end for start, end in code_ranges):
        raise ToolError("reset target is not within an ELF CODE section")
    nm_text = _run([nm, "-S", "--defined-only", str(elf)],
                   "nm", env=env).decode("ascii", "replace")
    matches = re.findall(
    r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\S\s+command_identify_data$",
    nm_text,
     re.MULTILINE)
    if len(matches) != 1:
        raise ToolError(
            "ELF must define exactly one sized command_identify_data symbol")
    symbol_address, symbol_size = (int(item, 16) for item in matches[0])
    owner = [section for section in file_backed
             if section["vma"] <= symbol_address and
             symbol_address + symbol_size <= section["vma"] + section["size"]]
    if len(owner) != 1 or not symbol_size:
        raise ToolError(
            "command_identify_data is not contained in one ELF load section")
    section = owner[0]
    position = section["offset"] + symbol_address - section["vma"]
    compressed = elf_data[position:position + symbol_size]
    try:
        embedded = zlib.decompress(compressed)
    except zlib.error as exc:
        raise ToolError("embedded dictionary is not valid zlib data: %s" % exc)
    if embedded != dictionary_data:
        raise ToolError(
            "supplied dictionary does not match embedded dictionary")
    dictionary_report = _load_dictionary(dictionary_data)
    report = {key: value for key, value in image.items() if key != "memory"}
    report.update({
        "bounds": {"application": [APP_START, APP_END],
                   "sram": [RAM_START, RAM_END]},
        "hex_sha256": hashlib.sha256(exact_hex).hexdigest(),
        "elf_sha256": hashlib.sha256(elf_data).hexdigest(),
        "dictionary_sha256": hashlib.sha256(dictionary_data).hexdigest(),
        "constants": dictionary_report["constants"],
        "dictionary": {key: dictionary_report[key]
                       for key in ("version", "build_versions", "kconfig")},
        "resolved_config": dictionary_report["resolved_config"],
        "tools": {
            "python": sys.version.splitlines()[0],
            "compiler": _version(_tool_path(cross_prefix + "gcc")),
            "objcopy": _version(_tool_path(cross_prefix + "objcopy")),
            "openssl": _version(openssl),
        },
    })
    return report


def build_firmware(output_dir, jobs=1, cross_prefix="arm-none-eabi-",
                   openssl="openssl"):
    if not isinstance(jobs, int) or jobs < 1:
        raise ToolError("jobs must be a positive integer")
    output = validate_output_root(output_dir)
    try:
        if output.exists():
            if not output.is_dir():
                raise ToolError("build output path is not a directory")
            if any(output.iterdir()):
                raise ToolError("build output directory is not empty")
        else:
            output.mkdir(parents=True)
    except ToolError:
        raise
    except (OSError, RuntimeError):
        raise ToolError("unable to prepare build output directory")
    try:
        python_path = Path(sys.executable).resolve()
        repository_path = REPO_ROOT.resolve()
    except (OSError, RuntimeError):
        raise ToolError("unable to resolve required build paths")
    for item in (repository_path, output, python_path, cross_prefix):
        if _unsupported_build_path(item):
            raise ToolError(
                "unsupported build path contains whitespace or Make "
                "metacharacters")
    make = _tool_path("make")
    gcc = _tool_path(cross_prefix + "gcc")
    objcopy = _tool_path(cross_prefix + "objcopy")
    for suffix in ("as", "ld", "nm", "objdump"):
        _tool_path(cross_prefix + suffix)
    libc_name = _run([gcc, "-print-file-name=libc_nano.a"],
                     "ARM GCC").decode().strip()
    if not libc_name or libc_name == "libc_nano.a" or not Path(
        libc_name).is_file():
        raise ToolError("ARM GCC newlib nano library is unavailable", 3)
    config = output / ".config"
    products_dir = output
    try:
        config.write_text(SEED_CONFIG, encoding="ascii", newline="\n")
    except OSError:
        raise ToolError("unable to write build configuration")
    make_out = str(products_dir) + os.sep
    env = os.environ.copy()
    for name in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "KCONFIG_CONFIG"):
        env.pop(name, None)
    env["LC_ALL"] = "C"
    common = [make, "OUT=" + make_out, "KCONFIG_CONFIG=" + str(config),
              "PYTHON=" + str(python_path), "CROSS_PREFIX=" + cross_prefix]
    _run(common + ["olddefconfig"], "make olddefconfig", env=env)
    resolved = _load_resolved_config(config)
    _run(common + ["-j%d" % jobs, "all"], "firmware build", env=env)
    elf = output / "klipper.elf"
    binary = output / "klipper.bin"
    dictionary = output / "klipper.dict"
    firmware = output / "levelBoard.hex"
    for path in (elf, binary, dictionary):
        try:
            valid = path.is_file() and bool(path.stat().st_size)
        except OSError:
            raise ToolError(
    "unable to inspect build product: %s" %
     path.name, 3)
        if not valid:
            raise ToolError("build did not produce nonempty %s" % path.name, 3)
    _run([objcopy, "-O", "ihex", str(elf), str(firmware)], "objcopy", env=env)
    try:
        valid_firmware = firmware.is_file() and bool(firmware.stat().st_size)
    except OSError:
        raise ToolError("unable to inspect levelBoard.hex", 3)
    if not valid_firmware:
        raise ToolError("objcopy did not produce levelBoard.hex", 3)
    validation = validate_firmware(
    firmware, elf, dictionary, cross_prefix, openssl)
    validation["resolved_config"] = resolved
    return {
        "stage": "build",
        "products": {"elf": elf.name, "bin": binary.name,
                     "firmware": firmware.name, "dictionary": dictionary.name,
                     "config": config.name},
        "firmware": validation,
    }


def _add_global(parser):
    parser.add_argument(
    "--cross-prefix",
    default="arm-none-eabi-",
     help="ARM toolchain prefix")
    parser.add_argument(
    "--openssl",
    default="openssl",
     help="OpenSSL executable")


def create_argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    _add_global(parser)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser(
    "build", help="build and validate C5 levelBoard firmware")
    build.add_argument("--output-dir", required=True, type=Path)
    build.add_argument("--jobs", type=int, default=1)
    validate = sub.add_parser("validate",
     help="validate firmware representations")
    validate.add_argument("--firmware", required=True, type=Path)
    validate.add_argument("--elf", required=True, type=Path)
    validate.add_argument("--dictionary", required=True, type=Path)
    inspect = sub.add_parser("inspect", help="inspect an update package")
    inspect.add_argument("--input", required=True, type=Path)
    inspect.add_argument("--decrypt", action="store_true")
    inspect.add_argument("--extract-temp", action="store_true")
    package = sub.add_parser(
    "package",
     help="create an encrypted update package")
    package.add_argument("--template", required=True, type=Path)
    package.add_argument("--firmware", required=True, type=Path)
    package.add_argument("--elf", required=True, type=Path)
    package.add_argument("--dictionary", required=True, type=Path)
    package.add_argument("--output", required=True, type=Path)
    all_cmd = sub.add_parser("all", help="build, validate, and package")
    all_cmd.add_argument("--template", required=True, type=Path)
    all_cmd.add_argument("--output-dir", required=True, type=Path)
    all_cmd.add_argument("--output", required=True, type=Path)
    all_cmd.add_argument("--jobs", type=int, default=1)
    return parser


def _json_print(report):
    json.dump(report, sys.stdout, sort_keys=True, indent=2)
    sys.stdout.write("\n")


def main(argv=None):
    args = create_argument_parser().parse_args(argv)
    try:
        if args.command == "build":
            _json_print(
    build_firmware(
        args.output_dir,
        args.jobs,
        args.cross_prefix,
         args.openssl))
        elif args.command == "validate":
            _json_print(
    validate_firmware(
        args.firmware,
        args.elf,
        args.dictionary,
        args.cross_prefix,
         args.openssl))
        else:
            raise ToolError("%s stage is not implemented yet" % args.command)
        return 0
    except ToolError as exc:
        secret = os.environ.get("C5_UPDATE_PASSPHRASE")
        print("error: " + sanitize_diagnostic(exc, secret), file=sys.stderr)
        return exc.exit_code
    except OSError:
        print("error: filesystem operation failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
