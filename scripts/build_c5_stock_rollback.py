#!/usr/bin/env python3
"""Build MCU-only stock rollback packages from canonical 1.9.9 updates.

The four HEX payloads are copied byte-for-byte from each model's factory
control archive.  Only the update wrapper, checksums, and installer scripts
are transformed by build_c5_update's validated MCU-only packaging path.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

import build_c5_update as update


BOARDS = tuple(update.BOARD_PROFILES)
WORKSPACE = update.REPO_ROOT.parent
DEFAULT_TEMPLATES = {
    "Creator5": WORKSPACE / "Creator5-1.9.9-1.3.0-20260907.gz",
    "Creator5Pro": WORKSPACE / "Creator5Pro-1.9.9-1.3.0-20260907.gz",
}


def stock_firmware(profile):
    """Return only the exact factory HEX bytes for the four known MCUs."""
    control = profile["control"]
    firmware = {}
    for board in BOARDS:
        path = "./" + update.BOARD_PROFILES[board]["firmware_name"]
        if path not in control:
            raise update.ToolError("factory control archive lacks " + path)
        data = control[path]["_data"]
        if not data or not data.startswith(b":"):
            raise update.ToolError("factory firmware is not Intel HEX: " + path)
        firmware[board] = data
    return firmware


def build_rollback(model, template, output_dir, openssl="openssl"):
    if model not in DEFAULT_TEMPLATES:
        raise update.ToolError("unsupported printer model: " + model)
    output = Path(output_dir) / (model + "-stock-1.9.9-mcus.tgz")
    output = update._prepare_package_output(output, [template])
    template_data = update._read_archive_input(template)
    if not template_data.startswith(b"Salted__"):
        raise update.ToolError("factory update is not encrypted")
    template_model = update._inspect_archive_model(
        template_data, decrypt=True, openssl=openssl)
    if (template_model.get("format") != "encrypted" or
            template_model.get("payload", {}).get("format") != "tar"):
        raise update.ToolError("factory update did not decrypt to a tar")
    profile = update._canonical_template_profile(template_model["payload"])
    if profile["device"] != model:
        raise update.ToolError("factory update model does not match " + model)

    firmware = stock_firmware(profile)
    plaintext, evidence = update._build_reduced_plaintext(profile, firmware)
    constructed = update._inspect_archive_model(plaintext)
    members = update._assert_reduced_profile(
        constructed, evidence, BOARDS)
    ciphertext = update.openssl_crypt(plaintext, False, openssl)

    # Reopen the encrypted result and confirm every retained HEX and checksum.
    with tempfile.TemporaryDirectory(prefix="c5-stock-rollback-") as temp:
        candidate = Path(temp) / "candidate.tgz"
        candidate.write_bytes(ciphertext)
        verified = update._inspect_archive_model(
            update._read_archive_input(candidate), decrypt=True,
            openssl=openssl)
        if (verified.get("format") != "encrypted" or
                verified.get("_payload") != plaintext):
            raise update.ToolError("rollback encryption roundtrip failed")
        verified_members = update._assert_reduced_profile(
            verified["payload"], evidence, BOARDS)
    if verified_members != members:
        raise update.ToolError("rollback member validation changed")

    manifest = {
        "schema_version": 1,
        "purpose": "MCU-only rollback to factory 1.9.9 firmware",
        "model": model,
        "factory_template": Path(template).name,
        "factory_template_sha256": hashlib.sha256(template_data).hexdigest(),
        "stock_firmware": {
            board: {
                "filename": update.BOARD_PROFILES[board]["firmware_name"],
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
            } for board, data in firmware.items()
        },
        "output": {
            "filename": output.name,
            "sha256": hashlib.sha256(ciphertext).hexdigest(),
            "plaintext_sha256": hashlib.sha256(plaintext).hexdigest(),
            "members": members,
        },
        "validation": [
            "canonical factory template hash and control checksums",
            "exact factory HEX bytes for all four MCUs",
            "MCU-only archive allowlist and regenerated checksums",
            "installer shell syntax",
            "fresh ciphertext decryption and member reinspection",
        ],
    }
    manifest_bytes = (json.dumps(manifest, sort_keys=True, indent=2) +
                      "\n").encode("utf-8")
    update._publish_outputs(output, ciphertext, manifest_bytes)
    return {"package": str(output), "manifest": str(output) +
            ".manifest.json", "sha256": manifest["output"]["sha256"],
            "stock_firmware": manifest["stock_firmware"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--creator5-template", type=Path,
                        default=DEFAULT_TEMPLATES["Creator5"])
    parser.add_argument("--creator5pro-template", type=Path,
                        default=DEFAULT_TEMPLATES["Creator5Pro"])
    parser.add_argument("--output-dir", type=Path,
                        default=WORKSPACE / "stock-1.9.9-mcu-rollback")
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args(argv)
    try:
        results = {}
        for model, template in (
                ("Creator5", args.creator5_template),
                ("Creator5Pro", args.creator5pro_template)):
            results[model] = build_rollback(
                model, template, args.output_dir, args.openssl)
        print(json.dumps(results, sort_keys=True, indent=2))
        return 0
    except update.ToolError as exc:
        print("error: " + update.sanitize_diagnostic(exc), file=sys.stderr)
        return exc.exit_code
    except OSError:
        print("error: filesystem operation failed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
