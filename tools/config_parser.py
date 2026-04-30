#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Xaloqi-Commercial
# Copyright (c) 2026 Xaloqi
# Commercial license required. See LICENSE_COMMERCIAL.txt and COMMERCIAL_NOTICE.md.
"""
=============================================================================
Xaloqi EDS
FILE: tools/config_parser.py

PURPOSE: Parse diagnostics_config.yaml and validate the DID/DTC/session
         configuration against schema constraints before code generation.

USAGE:
    python3 config_parser.py --input <path/to/diagnostics_config.yaml>
                             --output <path/to/validated_config.json>

DEPENDENCIES:
    PyYAML    >= 6.0
    jsonschema >= 4.0
=============================================================================
"""

import argparse
import json
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML is required. Install with: pip install pyyaml", file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Schema version
#
# Bump this integer whenever a breaking change is made to the YAML schema
# (new required field, removed field, changed semantics). config_parser.py
# rejects any YAML whose schema_version does not match this value.
#
# YAML files without a schema_version field are accepted with a deprecation
# warning — this allows existing configs to continue working during the
# transition period. After v2.0.0 launches, the warning will become an error.
#
# History:
#   1 — initial schema (EDS v1.x): id, name, access, min_session,
#       read/write_security_level, data_length, dtcs, timing, safeboot
# ---------------------------------------------------------------------------
CURRENT_SCHEMA_VERSION: int = 1

# ---------------------------------------------------------------------------
# Schema definition
# ---------------------------------------------------------------------------

DID_ENTRY_SCHEMA = {
    "type": "object",
    "required": ["id", "name", "access"],
    "properties": {
        "id": {
            "type": "string",
            "pattern": "^0x[0-9A-Fa-f]{4}$",
            "description": "16-bit DID identifier in hex (e.g. 0xF190)"
        },
        "name": {
            "type": "string",
            "minLength": 1,
            "maxLength": 128
        },
        "access": {
            "type": "array",
            "items": {"enum": ["read", "write"]},
            "minItems": 1
        },
        "min_session": {
            "enum": ["default", "extended", "programming", "safety_system"],
            "default": "default"
        },
        "read_security_level": {
            "type": "integer",
            "minimum": 0,
            "maximum": 255
        },
        "write_security_level": {
            "type": "integer",
            "minimum": 0,
            "maximum": 255
        },
        "data_length": {
            "type": "integer",
            "minimum": 1,
            "maximum": 64
        }
    },
    "additionalProperties": False
}

DTC_ENTRY_SCHEMA = {
    "type": "object",
    "required": ["code", "description"],
    "properties": {
        "code": {
            "type": "string",
            "pattern": "^0x[0-9A-Fa-f]{6}$",
            "description": "24-bit DTC code in hex (e.g. 0xC00100)"
        },
        "description": {
            "type": "string",
            "minLength": 1,
            "maxLength": 256
        },
        "severity": {
            "type": "string",
            "enum": ["none", "maintenance_only", "check_at_next_halt", "check_immediately"],
            "default": "none"
        }
    },
    "additionalProperties": False
}

CONFIG_SCHEMA = {
    "type": "object",
    "required": ["metadata", "dids"],
    "properties": {
        "schema_version": {
            "type": "integer",
            "minimum": 1,
            "description": (
                "Schema version of this config file. Must match the version "
                "understood by the installed config_parser.py. "
                f"Current version: {CURRENT_SCHEMA_VERSION}. "
                "Add 'schema_version: 1' to your diagnostics_config.yaml."
            )
        },
        "metadata": {
            "type": "object",
            "required": ["ecu_name", "version"],
            "properties": {
                "ecu_name": {"type": "string"},
                "version":  {"type": "string"},
                "description": {"type": "string"}
            }
        },
        "timing": {
            "type": "object",
            "properties": {
                "p2_server_max_ms":      {"type": "integer", "minimum": 1},
                "p2_star_server_max_ms": {"type": "integer", "minimum": 1},
                "s3_server_timeout_ms":  {"type": "integer", "minimum": 1}
            }
        },
        "dids": {
            "type": "array",
            "items": DID_ENTRY_SCHEMA,
            "minItems": 0
        },
        "dtcs": {
            "type": "array",
            "items": DTC_ENTRY_SCHEMA,
            "minItems": 0
        },
        "safeboot": {
            "type": "object",
            "description": "SafeBoot — MCUboot DFU integration (Xaloqi EDS Professional)",
            "properties": {
                "enabled": {
                    "type": "boolean",
                    "description": "Set true to generate zephyr_flash_ops_init() call in uds_init.c"
                },
                "platform": {
                    "type": "string",
                    "enum": ["zephyr", "freertos"],
                    "default": "zephyr",
                    "description": "Target RTOS platform for flash ops implementation"
                },
                "max_block_length": {
                    "type": "integer",
                    "minimum": 64,
                    "maximum": 4093,
                    "default": 256,
                    "description": "Max bytes per TransferData block (returned in 0x34 response)"
                }
            },
            "additionalProperties": False
        },
        "jobs": {
            "type": "object",
            "description": (
                "Optional job definitions for use with tools/jobrunner.py "
                "(Developer and Professional tiers). Each key is a job name "
                "(snake_case). Jobs are validated structurally here; full "
                "semantic validation is performed at execution time by jobrunner.py."
            ),
            "additionalProperties": True
        }
    },
    "additionalProperties": False
}

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

# Valid job step action types — mirrors dispatch table in tools/jobrunner.py.
# When a new action is added to jobrunner.py, add it here too.
VALID_ACTIONS = frozenset({
    "session",
    "security_access",
    "read_did",
    "write_did",
    "read_dtc",
    "clear_dtc",
    "routine",
    "request_download",
    "transfer_data",
    "request_transfer_exit",
    "ecu_reset",
    "tester_present",
    "delay",
    "assert",
    "foreach_did",
})


def _validate_jobs(jobs: dict, context: str) -> list:
    """
    Validate the optional top-level jobs: block.

    Returns a list of error strings. Empty list means valid.

    Performs lightweight structural validation:
      - Each job must be a mapping with a 'steps' list
      - Each step must have an 'action' field from VALID_ACTIONS
      - on_failure must be 'abort' or 'continue' if specified

    Full semantic validation (safeboot dependency, variable references, etc.)
    is performed at execution time by tools/jobrunner.py (EDS-toolchain).
    """
    errors = []

    if not isinstance(jobs, dict):
        return [f"{context}: 'jobs' must be a mapping of job definitions"]

    for job_name, job_def in jobs.items():
        job_ctx = f"{context}.{job_name}"

        if not isinstance(job_def, dict):
            errors.append(f"{job_ctx}: job definition must be a mapping")
            continue

        steps = job_def.get("steps")
        if steps is None:
            errors.append(f"{job_ctx}: missing required 'steps' field")
            continue

        if not isinstance(steps, list) or len(steps) == 0:
            errors.append(f"{job_ctx}: 'steps' must be a non-empty list")
            continue

        on_failure = job_def.get("on_failure")
        if on_failure is not None and on_failure not in ("abort", "continue"):
            errors.append(
                f"{job_ctx}: on_failure must be 'abort' or 'continue', "
                f"got '{on_failure}'"
            )

        for i, step in enumerate(steps):
            step_ctx = f"{job_ctx}.steps[{i}]"
            if not isinstance(step, dict):
                errors.append(f"{step_ctx}: step must be a mapping")
                continue
            action = step.get("action")
            if not action:
                errors.append(f"{step_ctx}: missing required 'action' field")
            elif action not in VALID_ACTIONS:
                errors.append(
                    f"{step_ctx}: unknown action '{action}'. "
                    f"Valid actions: {sorted(VALID_ACTIONS)}"
                )

    return errors

def validate_config(config: dict) -> list:
    """
    Validate a parsed config dictionary against the schema and additional
    semantic constraints.

    Returns a list of error message strings. Empty list means valid.
    """
    errors = []

    # ---------------------------------------------------------------------------
    # Schema version check
    #
    # If schema_version is present and does not match CURRENT_SCHEMA_VERSION,
    # reject immediately — the config may use fields or semantics that this
    # parser version does not understand (or may be missing required fields
    # added in a newer schema version).
    #
    # If schema_version is absent, emit a deprecation warning but continue.
    # This allows existing configs created before schema versioning was
    # introduced to keep working. Add 'schema_version: 1' to silence the
    # warning.
    # ---------------------------------------------------------------------------
    if "schema_version" not in config:
        print(
            "WARNING: 'schema_version' is missing from diagnostics_config.yaml. "
            f"Add 'schema_version: {CURRENT_SCHEMA_VERSION}' to the top of your "
            "config file to suppress this warning. Future versions of EDS will "
            "require this field.",
            file=sys.stderr,
        )
    else:
        sv = config["schema_version"]
        if not isinstance(sv, int):
            errors.append(
                f"schema_version must be an integer, got: {sv!r}"
            )
        elif sv != CURRENT_SCHEMA_VERSION:
            errors.append(
                f"schema_version mismatch: config has version {sv}, "
                f"but this config_parser.py requires version "
                f"{CURRENT_SCHEMA_VERSION}. "
                + (
                    "Upgrade your config file."
                    if sv < CURRENT_SCHEMA_VERSION
                    else "Upgrade your Xaloqi EDS installation."
                )
            )

    # Structural checks (manual, to avoid jsonschema dependency)
    if "metadata" not in config:
        errors.append("Missing required top-level key: 'metadata'")
    else:
        meta = config["metadata"]
        if "ecu_name" not in meta:
            errors.append("metadata.ecu_name is required")
        if "version" not in meta:
            errors.append("metadata.version is required")

    if "dids" not in config:
        errors.append("Missing required top-level key: 'dids'")
    else:
        seen_ids = set()
        for idx, did in enumerate(config.get("dids", [])):
            did_id = did.get("id", "")
            if not did_id:
                errors.append(f"dids[{idx}]: missing 'id' field")
                continue
            if did_id in seen_ids:
                errors.append(f"dids[{idx}]: duplicate DID id '{did_id}'")
            seen_ids.add(did_id)
            if "name" not in did:
                errors.append(f"dids[{idx}] ({did_id}): missing 'name' field")
            if "access" not in did or not did["access"]:
                errors.append(f"dids[{idx}] ({did_id}): missing or empty 'access' field")
            # REQ-SAFE-006: write length validation requires data_length.
            # If a DID has write access and no data_length, service_0x2E cannot
            # enforce that the tester sends exactly the right number of bytes,
            # allowing arbitrary-length writes to the DID.
            if "write" in did.get("access", []) and "data_length" not in did:
                errors.append(
                    f"dids[{idx}] ({did_id}): 'data_length' is required for "
                    f"writable DIDs (REQ-SAFE-006). Add 'data_length: <bytes>' "
                    f"to enforce write length validation in service 0x2E."
                )

    for idx, dtc in enumerate(config.get("dtcs", [])):
        dtc_code = dtc.get("code", "")
        if not dtc_code:
            errors.append(f"dtcs[{idx}]: missing 'code' field")
        if "description" not in dtc:
            errors.append(f"dtcs[{idx}] ({dtc_code}): missing 'description' field")

    # jobs: block is optional — validate structure if present
    if "jobs" in config:
        jobs_errors = _validate_jobs(config["jobs"], "jobs")
        errors.extend(jobs_errors)

    return errors


def load_and_validate(input_path: Path) -> dict:
    """Load YAML file, parse it, and validate it. Returns validated config."""
    if not input_path.exists():
        print(f"ERROR: Input file not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    with open(input_path, "r", encoding="utf-8") as fh:
        try:
            config = yaml.safe_load(fh)
        except yaml.YAMLError as exc:
            print(f"ERROR: YAML parse error in '{input_path}': {exc}", file=sys.stderr)
            sys.exit(1)

    if not isinstance(config, dict):
        print("ERROR: Config root must be a YAML mapping.", file=sys.stderr)
        sys.exit(1)

    errors = validate_config(config)
    if errors:
        print("ERROR: Configuration validation failed:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        sys.exit(1)

    return config


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Parse and validate diagnostics_config.yaml"
    )
    parser.add_argument(
        "--input", required=True,
        help="Path to diagnostics_config.yaml"
    )
    parser.add_argument(
        "--output", required=False,
        help="Optional path to write validated config as JSON"
    )

    args = parser.parse_args()

    config = load_and_validate(Path(args.input))

    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as fh:
            json.dump(config, fh, indent=2)
        print(f"Validated config written to: {output_path}")
    else:
        print(json.dumps(config, indent=2))

    print(f"Validation passed: {len(config.get('dids', []))} DID(s), "
          f"{len(config.get('dtcs', []))} DTC(s).")


if __name__ == "__main__":
    main()
