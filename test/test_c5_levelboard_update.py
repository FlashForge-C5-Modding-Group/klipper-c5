#!/usr/bin/env python3
# This file may be distributed under the terms of the GNU GPLv3 license.

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "scripts" / "build_c5_levelboard_update.py"
SPEC = importlib.util.spec_from_file_location("c5_update", TOOL_PATH)
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)

APP_START = 0x08004000
APP_END = 0x08010000
def dictionary_data(kconfig=None, version="test-version",
                    build_versions="test-tools"):
    commands = {
    name: index for index,
    name in enumerate(
        sorted(
            TOOL.REQUIRED_COMMANDS -
            {"identify offset=%u count=%c"}),
             2)}
    commands["identify offset=%u count=%c"] = 1
    first_response = max(commands.values()) + 1
    responses = {name: index for index, name in
                 enumerate(sorted(TOOL.REQUIRED_RESPONSES -
                                  {"identify_response offset=%u data=%.*s"}),
                           first_response)}
    responses["identify_response offset=%u data=%.*s"] = 0
    value = {
        "commands": commands,
        "responses": responses,
        "output": {},
        "config": dict(TOOL.REQUIRED_CONSTANTS),
        "kconfig": TOOL.SEED_CONFIG if kconfig is None else kconfig,
        "version": version,
        "build_versions": build_versions,
    }
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode()


def record(kind, address=0, payload=b""):
    body = bytes((len(payload), address >> 8, address & 0xff, kind)) + payload
    return b":" + (body + bytes((-sum(body) & 0xff,))
                   ).hex().upper().encode() + b"\n"


def ihex(records=(), sp=0x20004000, reset=APP_START + 9, eof=True):
    data = record(4, payload=struct.pack(">H", APP_START >> 16))
    vectors = struct.pack("<II", sp, reset)
    data += record(0, APP_START & 0xffff, vectors + b"\x00\xbf\x00\xbf")
    for entry in records:
        data += entry
    if eof:
        data += record(1)
    return data


class IntelHexTests(unittest.TestCase):
    def parse(self, value):
        return TOOL.parse_ihex(value)

    def assert_rejected(self, value, *words):
        with self.assertRaises(TOOL.ToolError) as raised:
            self.parse(value)
        text = str(raised.exception).lower()
        for word in words:
            self.assertIn(word.lower(), text)

    def test_accepts_sparse_image_and_reports_vectors_holes_and_hash(self):
        data = ihex([record(0, 0x4020, b"abc")])
        image = self.parse(data)
        self.assertEqual(image["stack_pointer"], 0x20004000)
        self.assertEqual(image["reset_handler"], APP_START + 9)
        self.assertEqual(image["mapped_byte_count"], 15)
        self.assertEqual(image["intervals"], [
                         [APP_START, APP_START + 12],
                         [APP_START + 0x20, APP_START + 0x23]])
        self.assertEqual(image["holes"][0], [APP_START + 12, APP_START + 0x20])
        self.assertEqual(image["holes"][-1], [APP_START + 0x23, APP_END])
        self.assertEqual(image["normalization"], "application-48k-ff-fill-v1")
        normalized = bytearray(b"\xff" * (APP_END - APP_START))
        normalized[:12] = (struct.pack("<II", 0x20004000, APP_START + 9) +
                           b"\x00\xbf\x00\xbf")
        normalized[0x20:0x23] = b"abc"
        self.assertEqual(image["normalized_sha256"],
                         hashlib.sha256(normalized).hexdigest())

    def test_rejects_bad_checksum_with_line_context(self):
        value = bytearray(ihex())
        value[value.index(b"\n") -
    1] = ord("0") if value[value.index(b"\n") -
     1] != ord("0") else ord("1")
        self.assert_rejected(bytes(value), "checksum", "line 1")

    def test_rejects_record_count_mismatch(self):
        line = record(0, 0x4000, b"1234")
        damaged = b":05" + line[3:]
        self.assert_rejected(damaged + record(1), "length", "line 1")

    def test_rejects_bad_extended_address_records_and_segment_address(self):
        self.assert_rejected(
    record(
        4,
        1,
        b"\x08\x00") +
        record(1),
        "address",
         "line 1")
        self.assert_rejected(
    record(
        4,
        0,
        b"\x08") +
        record(1),
        "length",
         "line 1")
        self.assert_rejected(
    record(
        2,
        0,
        b"\x08\x00") +
        record(1),
        "unsupported",
         "02")

    def test_overlap_requires_identical_bytes(self):
        same = ihex([record(0, 0x4008, b"\x00\xbf")])
        self.parse(same)
        different = ihex([record(0, 0x4008, b"\x01\xbf")])
        self.assert_rejected(different, "overlap", "08004008")

    def test_rejects_bytes_below_and_at_upper_bound(self):
        below = record(4,
    payload=b"\x08\x00") + record(0,
    0x3fff,
    b"x") + record(0,
    0x4000,
    struct.pack("<II",
    0x20004000,
     APP_START + 9)) + record(1)
        self.assert_rejected(below, "range", "08003fff")
        upper = ihex([record(4, payload=b"\x08\x01"), record(0, 0, b"x")])
        self.assert_rejected(upper, "range", "08010000")

    def test_detects_window_overflow_before_range(self):
        value = record(4, payload=b"\xff\xff") + \
                       record(0, 0xffff, b"ab") + record(1)
        self.assert_rejected(value, "window", "line 2")

    def test_stack_pointer_bounds_alignment_and_upper_endpoint(self):
        self.parse(ihex(sp=0x20004000))
        for sp, expected in ((0x1ffffff8, "sram"),
                             (0x20004008, "sram"), (0x20000004, "align")):
            with self.subTest(sp=hex(sp)):
                self.assert_rejected(ihex(sp=sp), expected)

    def test_reset_requires_thumb_and_application_target(self):
        self.assert_rejected(ihex(reset=APP_START + 8), "thumb")
        self.assert_rejected(ihex(reset=APP_END + 1), "reset", "range")

    def test_eof_is_exactly_once_and_final(self):
        self.assert_rejected(ihex(eof=False), "eof")
        self.assert_rejected(ihex() + record(1), "after eof")
        self.assert_rejected(ihex() + record(0, 0x4008, b"x"), "after eof")

    def test_start_linear_address_is_metadata(self):
        value = ihex([record(5, payload=struct.pack(">I", APP_START))])
        parsed = self.parse(value)
        self.assertEqual(parsed["start_linear_address"], APP_START)
        self.assertEqual(parsed["mapped_byte_count"], 12)
        self.assert_rejected(
            ihex([record(5, 1, struct.pack(">I", APP_START))]), "address")
        self.assert_rejected(ihex([record(5, payload=struct.pack(">I", 0))]),
                             "start", "application")

    def test_rejects_non_ascii_blank_internal_and_trailing_whitespace(self):
        self.assert_rejected(
    ihex().replace(
        b"\n",
        b"\n\n",
        1),
        "blank",
         "line 2")
        self.assert_rejected(ihex().replace(b"\n", b" \n", 1), "whitespace")
        self.assert_rejected(ihex() + b"\xff", "ascii")


class FoundationTests(unittest.TestCase):
    def run_cli(self, *args, env=None):
        return subprocess.run([sys.executable,
    str(TOOL_PATH),
    *args],
    cwd=ROOT,
    text=True,
    capture_output=True,
     env=env)

    def test_each_subcommand_has_help_and_global_defaults(self):
        for command in ("build", "validate", "inspect", "package", "all"):
            with self.subTest(command=command):
                result = self.run_cli(command, "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout.lower())
        parsed = TOOL.create_argument_parser().parse_args(
            ["build", "--output-dir", "x"])
        self.assertEqual((parsed.cross_prefix, parsed.openssl,
                         parsed.jobs), ("arm-none-eabi-", "openssl", 1))

    def test_expected_input_error_has_exit_two_without_traceback(self):
        result = self.run_cli(
    "validate",
    "--firmware",
    "missing.hex",
    "--elf",
    "missing.elf",
    "--dictionary",
     "missing.dict")
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("traceback", result.stderr.lower())
        self.assertEqual(result.stdout, "")

    def test_output_root_policy_accepts_external_or_out_only(self):
        with tempfile.TemporaryDirectory() as outside:
            self.assertEqual(
    TOOL.validate_output_root(
        Path(outside)),
         Path(outside).resolve())
        self.assertEqual(
    TOOL.validate_output_root(
        ROOT / "out" / "firmware"),
         (ROOT / "out" / "firmware").resolve())
        for path in (ROOT, ROOT / "scripts", ROOT / "src" / "generated"):
            with self.subTest(path=path), self.assertRaises(TOOL.ToolError):
                TOOL.validate_output_root(path)
        with self.assertRaises(TOOL.ToolError):
            TOOL.validate_output_root(Path(ROOT.anchor))

    def test_build_rejects_nonempty_output_before_running_tools(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "build"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("owned")
            with self.assertRaisesRegex(TOOL.ToolError, "not empty") as raised:
                TOOL.build_firmware(output, 1, "missing-toolchain-", "openssl")
            self.assertEqual(raised.exception.exit_code, 2)
            self.assertEqual(sentinel.read_text(), "owned")

    def test_output_root_rejects_symlink_into_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            link = Path(temp) / "linked"
            try:
                link.symlink_to(ROOT / "scripts", target_is_directory=True)
            except OSError:
                self.skipTest("symlink creation unavailable")
            with self.assertRaises(TOOL.ToolError):
                TOOL.validate_output_root(link)
    def test_output_root_rejects_repository_out_alias_into_source(self):
        with tempfile.TemporaryDirectory() as temp:
            fake_repo = Path(temp)
            source = fake_repo / "src"
            source.mkdir()
            try:
                (fake_repo / "out").symlink_to(
                    source, target_is_directory=True)
            except OSError:
                self.skipTest("symlink creation unavailable")
            original = TOOL.REPO_ROOT
            TOOL.REPO_ROOT = fake_repo
            try:
                with self.assertRaisesRegex(
                        TOOL.ToolError,
                        "outside.*repository|out directory"):
                    TOOL.validate_output_root(fake_repo / "out" / "generated")
            finally:
                TOOL.REPO_ROOT = original

    def test_build_path_filter_rejects_command_substitution_and_make_syntax(
        self):
        for value in ("safe\x60touch-pwned\x60", "safe$(touch-pwned)",
                      "safe;touch-pwned", "safe#comment", "safe=value",
                      r"C:\repo\out", "safe:colon", r"safe\suffix"):
            with self.subTest(value=value):
                self.assertTrue(TOOL._unsupported_build_path(value))
        for value in ("arm-none-eabi-", "/tmp/build", "safe+path"):
            with self.subTest(safe=value):
                self.assertFalse(TOOL._unsupported_build_path(value))

    def test_filesystem_failure_is_exit_two_without_traceback_or_path(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp) / "regular-file"
            parent.write_text("not a directory")
            output = parent / "build"
            result = self.run_cli("build", "--output-dir", str(output))
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertNotIn("traceback", result.stderr.lower())
            self.assertNotIn(str(Path(temp)), result.stderr)
            self.assertEqual(result.stdout, "")

    def test_cyclic_symlinks_are_sanitized_input_and_output_errors(self):
        with tempfile.TemporaryDirectory() as temp:
            loop = Path(temp) / "loop"
            try:
                loop.symlink_to(loop, target_is_directory=True)
            except OSError:
                self.skipTest("symlink creation unavailable")
            for arguments in (
                    ("build", "--output-dir", str(loop / "build")),
                    ("validate", "--firmware", str(loop), "--elf",
                     "missing.elf",
                     "--dictionary", "missing.dict")):
                with self.subTest(command=arguments[0]):
                    result = self.run_cli(*arguments)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertNotIn("traceback", result.stderr.lower())
                    self.assertNotIn(str(Path(temp)), result.stderr)
                    self.assertEqual(result.stdout, "")


class ConfigTests(unittest.TestCase):
    def test_seed_config_is_exact(self):
        self.assertEqual(TOOL.SEED_CONFIG, (
            "CONFIG_MACH_STM32=y\n"
            "CONFIG_MACH_N32G430F8S7=y\n"
            "CONFIG_STM32_CLOCK_REF_8M=y\n"
            "CONFIG_SERIAL=y\n"
            "CONFIG_STM32_SERIAL_USART1=y\n"
            "CONFIG_C5_LEVELBOARD=y\n"
        ))

    def test_sanitizer_redacts_paths_and_secret(self):
        secret = "test-generated-secret"
        sanitized = TOOL.sanitize_diagnostic(
    f"failure {secret} at C:\\private\\file", secret)
        self.assertNotIn(secret, sanitized)
        self.assertNotIn("C:\\private", sanitized)
        self.assertIn("[redacted]", sanitized)
    def test_objdump_parser_rejects_unrecognized_section_rows(self):
        output = """Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .bad name     00000004  20004000  20004000  00000034  2**2
                  ALLOC
"""
        with self.assertRaisesRegex(TOOL.ToolError, "ambiguous.*section"):
            TOOL._parse_sections(output, 1024)

    def test_dictionary_uses_resolved_kconfig_not_literal_lines(self):
        contradictory = TOOL.SEED_CONFIG + "CONFIG_MACH_STM32F103=y\n"
        with self.assertRaisesRegex(TOOL.ToolError, "Kconfig|hardware"):
            TOOL._load_dictionary(dictionary_data(kconfig=contradictory))

    def test_dictionary_membership_uses_message_parser_classification(self):
        value = json.loads(dictionary_data())
        required_response = "trigger_threshold threshold=%i"
        value["responses"][required_response] = \
            value["commands"]["get_mcu_version"]
        with self.assertRaisesRegex(TOOL.ToolError,
                                    "response|membership|conflicting "
                                    "message ID"):
            TOOL._load_dictionary(json.dumps(value).encode())
    def test_dictionary_rejects_conflicting_ids_and_nonoutput_names(self):
        value = json.loads(dictionary_data())
        responses = sorted(TOOL.REQUIRED_RESPONSES -
                           {"identify_response offset=%u data=%.*s"})
        value["responses"][responses[1]] = value["responses"][responses[0]]
        with self.assertRaisesRegex(
                TOOL.ToolError, "conflicting.*ID|ID.*conflict"):
            TOOL._load_dictionary(json.dumps(value).encode())

        value = json.loads(dictionary_data())
        next_id = max(value["responses"].values()) + 1
        value["responses"]["duplicate value=%u"] = next_id
        value["responses"]["duplicate value=%c"] = next_id + 1
        with self.assertRaisesRegex(
                TOOL.ToolError, "conflicting.*name|name.*conflict"):
            TOOL._load_dictionary(json.dumps(value).encode())

    def test_dictionary_metadata_paths_are_explicitly_redacted(self):
        report = TOOL._load_dictionary(dictionary_data(
            version="version[/private/build/tree]",
            build_versions=r"v@C:\private\toolchain"))
        self.assertEqual(report["version"], "[redacted: unsafe metadata]")
        self.assertEqual(
    report["build_versions"],
     "[redacted: unsafe metadata]")

    def test_malformed_dictionary_does_not_log_parser_traceback(self):
        value = json.loads(dictionary_data())
        value["commands"]["malformed value=%q"] = max(
            value["commands"].values()) + 100
        captured = io.StringIO()
        with contextlib.redirect_stderr(captured):
            with self.assertRaisesRegex(
                    TOOL.ToolError, "invalid protocol dictionary"):
                TOOL._load_dictionary(json.dumps(value).encode())
        self.assertEqual(captured.getvalue(), "")


SYNTHETIC_TOOL_NAMES = tuple("arm-none-eabi-" + name for name in
                             ("as", "ld", "objcopy", "objdump", "nm", "gcc"))


@unittest.skipUnless(all(shutil.which(name) for name in SYNTHETIC_TOOL_NAMES),
                     "ARM binutils are required for synthetic ELF validation")
class SyntheticElfValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.workspace.name)
        cls.dictionary = cls.directory / "firmware.dict"
        cls.dictionary.write_bytes(dictionary_data())
        (cls.directory / "dictionary.z").write_bytes(
            zlib.compress(cls.dictionary.read_bytes(), 9))
        (cls.directory / "fixture.S").write_text(r"""
.syntax unified
.cpu cortex-m3
.thumb
.section .vectors,"a",%progbits
.word 0x20004000
.word reset_handler + 1
.section .text,"ax",%progbits
.thumb_func
.global reset_handler
.type reset_handler,%function
reset_handler:
  nop
  nop
  bx lr
.size reset_handler, .-reset_handler
.section .rodata,"a",%progbits
.balign 4
.global command_identify_data
.type command_identify_data,%object
command_identify_data:
.incbin "dictionary.z"
.size command_identify_data, .-command_identify_data
.section .data,"aw",%progbits
.global data_word
data_word:
.word 0x12345678
.section .bss,"aw",%nobits
.space 8
""", encoding="ascii")
        (cls.directory / "fixture.ld").write_text("""
ENTRY(reset_handler)
MEMORY {
  FLASH (rx) : ORIGIN = 0x08004000, LENGTH = 0xC000
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x4000
}
SECTIONS {
  .vectors 0x08004000 : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM AT> FLASH
  .bss (NOLOAD) : { *(.bss*) } > RAM
}
""", encoding="ascii")
        cls._checked(["arm-none-eabi-as", "-mcpu=cortex-m3", "-mthumb",
                      "-o", "fixture.o", "fixture.S"])
        cls._checked(["arm-none-eabi-ld", "-T", "fixture.ld", "-o",
                      "firmware.elf", "fixture.o"])
        cls._checked(["arm-none-eabi-objcopy", "-O", "ihex", "firmware.elf",
                      "firmware.hex"])
        cls.elf = cls.directory / "firmware.elf"
        cls.firmware = cls.directory / "firmware.hex"

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    @classmethod
    def _checked(cls, command):
        result = subprocess.run(command, cwd=cls.directory, text=True,
                                capture_output=True)
        if result.returncode:
            raise RuntimeError(
    "synthetic ARM fixture failed: " +
     result.stderr)

    def run_cli(self, firmware="firmware.hex", elf="firmware.elf",
                dictionary="firmware.dict"):
        return subprocess.run(
            [sys.executable, str(TOOL_PATH), "validate", "--firmware",
             firmware,
             "--elf", elf, "--dictionary", dictionary],
            cwd=self.directory, text=True, capture_output=True)

    def test_validate_cli_accepts_relative_inputs_and_initialized_data(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["resolved_config"]["MACH_N32G430F8S7"], "y")
        self.assertEqual(report["resolved_config"]["RAM_SIZE"], "0x4000")
        self.assertNotIn(str(self.directory), result.stdout)

    def test_validate_cli_rejects_hex_and_dictionary_correspondence_failures(
        self):
        lines = self.firmware.read_bytes().splitlines(keepends=True)
        for index, line in enumerate(lines):
            raw = bytearray.fromhex(line[1:].strip().decode())
            if raw[3] == 0 and raw[0] and ((raw[1] << 8) | raw[2]) >= 0x4008:
                raw[4] ^= 1
                raw[-1] = (-sum(raw[:-1])) & 0xff
                lines[index] = b":" + raw.hex().upper().encode() + b"\n"
                break
        (self.directory / "mutated.hex").write_bytes(b"".join(lines))
        mismatch = self.run_cli(firmware="mutated.hex")
        self.assertEqual(mismatch.returncode, 2, mismatch.stderr)
        self.assertIn("mismatch", mismatch.stderr.lower())
        (self.directory / "unrelated.dict").write_bytes(
            dictionary_data(version="unrelated"))
        unrelated = self.run_cli(dictionary="unrelated.dict")
        self.assertEqual(unrelated.returncode, 2, unrelated.stderr)
        self.assertIn("embedded dictionary", unrelated.stderr.lower())

    def test_validate_cli_bounds_initialized_data_vma_and_flash_lma(self):
        for option, value, output, words in (
                ("--change-section-vma", ".data=0x20003fff",
                 "crossing-vma.elf",
                 ("ram", "bounds")),
                ("--change-section-vma", ".data=0x10000000", "outside-vma.elf",
                 ("allocated", "outside", "sram")),
                ("--change-section-vma", ".bss=0x08005000", "bad-bss.elf",
                 ("bss", "sram")),
                ("--change-section-lma", ".data=0x0800ffff", "bad-lma.elf",
                 ("load", "flash", "bounds"))):
            with self.subTest(option=option):
                self._checked(["arm-none-eabi-objcopy", option, value,
                               "firmware.elf", output])
                result = self.run_cli(elf=output)
                self.assertEqual(result.returncode, 2, result.stderr)
                for word in words:
                    self.assertIn(word, result.stderr.lower())
                self.assertNotIn("traceback", result.stderr.lower())
@unittest.skipUnless(os.environ.get("RUN_C5_BUILD_TESTS") == "1",
                     "set RUN_C5_BUILD_TESTS=1 with the ARM toolchain "
                     "available")
class RealFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory()
        cls.output = Path(cls.workspace.name) / "build"
        cls.repo_config = ROOT / ".config"
        cls.config_before = (cls.repo_config.read_bytes()
                             if cls.repo_config.exists() else None)
        cls.build_report = TOOL.build_firmware(cls.output, 1, "arm-none-eabi-",
                                               "openssl")

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def test_actual_c5_build_is_fully_valid_and_does_not_touch_repo_config(
        self):
        paths = self.build_report["products"]
        for key in ("elf", "bin", "firmware", "dictionary", "config"):
            self.assertTrue((self.output / paths[key]).is_file(), key)
        current = (self.repo_config.read_bytes()
                   if self.repo_config.exists() else None)
        self.assertEqual(current, self.config_before)
        firmware = self.build_report["firmware"]
        self.assertEqual(
    firmware["normalization"],
     "application-48k-ff-fill-v1")
        self.assertEqual(
    firmware["bounds"]["application"], [
        APP_START, APP_END])
        self.assertEqual(firmware["constants"]["RECEIVE_WINDOW"], 384)
        self.assertEqual(firmware["constants"]["SERIAL_BAUD"], 230400)
        self.assertEqual(firmware["resolved_config"]["MCU"], "stm32f103xe")
        self.assertNotIn(str(self.output), json.dumps(self.build_report))

    def test_mutated_dictionary_is_rejected_as_not_embedded(self):
        paths = self.build_report["products"]
        source = self.output / paths["dictionary"]
        mutated = self.output / "unrelated.dict"
        value = json.loads(source.read_text())
        value["version"] = "unrelated"
        mutated.write_text(
    json.dumps(
        value,
        separators=(
            ",",
            ":"),
             sort_keys=True))
        with self.assertRaisesRegex(TOOL.ToolError, "embedded dictionary"):
            TOOL.validate_firmware(self.output / paths["firmware"],
                                   self.output / paths["elf"], mutated,
                                   "arm-none-eabi-", "openssl")

    def test_data_flash_load_crossing_application_limit_is_rejected(self):
        paths = self.build_report["products"]
        changed = self.output / "crossing-data.elf"
        objcopy = TOOL._tool_path("arm-none-eabi-objcopy")
        subprocess.run([objcopy,
    "--change-section-lma",
    ".data=0x0800fff0",
    str(self.output / paths["elf"]),
    str(changed)],
     check=True)
        with self.assertRaisesRegex(TOOL.ToolError, "section .data.*crosses"):
            TOOL.validate_firmware(self.output / paths["firmware"], changed,
                                   self.output / paths["dictionary"],
                                   "arm-none-eabi-", "openssl")

    def test_mutated_hex_load_byte_is_rejected_as_elf_mismatch(self):
        paths = self.build_report["products"]
        source = (self.output / paths["firmware"]).read_bytes()
        lines = source.splitlines(keepends=True)
        for index, line in enumerate(lines):
            raw = bytearray.fromhex(line[1:].strip().decode())
            if raw[3] == 0 and raw[0] and ((raw[1] << 8) | raw[2]) >= 0x4008:
                raw[4] ^= 1
                raw[-1] = (-sum(raw[:-1])) & 0xff
                lines[index] = b":" + raw.hex().upper().encode() + b"\n"
                break
        mutated = self.output / "mutated.hex"
        mutated.write_bytes(b"".join(lines))
        with self.assertRaisesRegex(
                TOOL.ToolError, "ELF.*mismatch|mismatch.*ELF"):
            TOOL.validate_firmware(mutated, self.output / paths["elf"],
                                   self.output / paths["dictionary"],
                                   "arm-none-eabi-", "openssl")


if __name__ == "__main__":
    unittest.main(verbosity=2)
