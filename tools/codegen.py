#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Xaloqi-Commercial
# Copyright (c) 2026 Xaloqi
# Commercial license required. See LICENSE_COMMERCIAL.txt
# File: tools/codegen.py
"""
=============================================================================
Xaloqi EDS
FILE: tools/codegen.py

PURPOSE: Production code generator — reads diagnostics_config.yaml and
         renders Jinja2 templates into deployable C source and header files.

PIPELINE:
    load_config()
      -> validate_config()          (Phase 2A)
        -> validate_safety_config() (Phase 3 — ASIL-B checks)
          -> build_contexts()
            -> render_templates()
              -> render_safety_wrappers() (Phase 3 — optional --safety-wrappers)
                -> generate_tests()     (Phase 4 — optional --test-gen)
                  -> write_outputs()
                    -> write_manifest()

OUTPUTS (into --out directory):
    generated_config.h      Compile-time constants derived from YAML
    did_handlers.h          DID handler declarations
    did_handlers.c          DID read/write stubs + static DID table
    uds_init.h              uds_generated_init() declaration
    uds_init.c              Full stack initialisation wiring

OPTIONAL (--safety-wrappers, Phase 3):
    safety_config.h         ASIL compile-time macro configuration
    did_safety_wrappers.h   Safe DID access wrappers (session + bounds checked)
    did_safety_wrappers.c   Implementation of generated safety wrappers

OPTIONAL (--test-gen, Phase 4):
    tests/conftest.py           pytest fixtures + full ISO-TP transport layer
    tests/test_services.py      UDS service tests (0x10/0x11/0x19/0x22/0x27/0x2E/0x3E)
    tests/test_did_XXXX.py      Per-DID exhaustive tests (one file per DID)
    tests/pytest.ini            pytest configuration
    tests/requirements_testgen.txt  pip dependencies
    tests/README.md             How to run the generated tests

USAGE:
    # Standard generation (Phase 2A compatible):
    python3 tools/codegen.py \\
        --config examples/basic_ecu/diagnostics_config.yaml \\
        --out    generated/

    # Phase 3 — generate safety wrappers alongside standard outputs:
    python3 tools/codegen.py \\
        --config  examples/basic_ecu/diagnostics_config.yaml \\
        --out     generated/ \\
        --safety-wrappers \\
        --asil-level B

EXIT CODES:
    0  Success — all files written.
    1  Validation error (duplicate DID, invalid ID range, safety constraint, etc.).
    2  I/O or argument error.
    3  Template rendering error.

DEPENDENCIES:
    PyYAML >= 6.0    (pip install pyyaml)
    Jinja2 >= 3.1    (pip install jinja2)

PHASE 3 ADDITIONS:
    - validate_safety_config(): ASIL-B DID constraint checks.
    - build_safety_config_context(): context for safety_config.h.
    - build_did_safety_wrappers_context(): context for safety wrapper files.
    - render_safety_wrappers(): renders the 3 safety wrapper files.
    - --safety-wrappers CLI flag: opt-in safety wrapper generation.
    - --asil-level CLI flag: sets ASIL level macro in safety_config.h.
    - write_manifest() updated to Phase 3 naming and safety flag tracking.

VERSION : 0.4.0 (Phase 3)
SPDX-License-Identifier: LicenseRef-Xaloqi-Commercial\n# Copyright (c) 2026 Xaloqi\n# Commercial license required. See LICENSE_COMMERCIAL.txt
=============================================================================
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML is required.  pip install pyyaml", file=sys.stderr)
    sys.exit(2)

try:
    from jinja2 import Environment, FileSystemLoader, StrictUndefined, TemplateError
except ImportError:
    print("ERROR: Jinja2 is required.  pip install jinja2", file=sys.stderr)
    sys.exit(2)

# =============================================================================
# Constants
# =============================================================================

SESSION_MAP: Dict[str, str] = {
    "default":       "UDS_SESSION_DEFAULT",
    "extended":      "UDS_SESSION_EXTENDED",
    "programming":   "UDS_SESSION_PROGRAMMING",
    "safety_system": "UDS_SESSION_SAFETY_SYSTEM",
}

SESSION_ORDINAL: Dict[str, int] = {
    "default":       1,
    "extended":      3,
    "programming":   2,
    "safety_system": 4,
}

# =============================================================================
# Phase 3 — ASIL level configuration
# =============================================================================

#: Valid ASIL target levels accepted by --asil-level
VALID_ASIL_LEVELS: List[str] = ["QM", "A", "B", "C", "D"]

#: Maximum data_length for a DID in ASIL-B builds (static buffer limit).
ASIL_B_MAX_DID_DATA_LEN: int = 64

#: Maximum DID count for ASIL-B builds (compile-time array bound).
ASIL_B_MAX_DID_COUNT: int = 64

#: Minimum number of DIDs expected in a properly configured ASIL build.
ASIL_B_MIN_DID_COUNT: int = 1

#: ASIL-B requires every DID to have an explicit min_session declaration.
ASIL_B_REQUIRE_EXPLICIT_SESSION: bool = True

#: ASIL-B requires every write-capable DID to have a write_security_level > 0.
#:
#: [HIGH-1 FIX] Set to True — write_security_level=0 on a write-capable DID is
#: now a hard fatal error under ASIL-B, not an advisory warning.
#:
#: RATIONALE: A write-capable DID with no security gate allows any tester in an
#: Extended session to overwrite safety-relevant calibration data without any
#: security challenge (SID 0x27 SecurityAccess is bypassed entirely).  Under
#: ISO 26262-6 ASIL-B this is unacceptable — unauthenticated write access to ECU
#: parameters must be blocked.
#:
#: TO OVERRIDE (requires formal safety deviation record):
#:   Set write_security_level >= 1 in the YAML for each write-capable DID, OR
#:   set ASIL_B_REQUIRE_WRITE_SECURITY = False here with a documented deviation
#:   record referencing the specific DID IDs and justification per ISO 26262-8 §7.
#:   Do NOT set it False globally without per-DID justification.
ASIL_B_REQUIRE_WRITE_SECURITY: bool = True  # [HIGH-1 FIX] Hard error, not advisory.

_DID_PATTERN = re.compile(r"^0[xX][0-9A-Fa-f]{4}$")
_DTC_PATTERN = re.compile(r"^0[xX][0-9A-Fa-f]{6}$")
_RID_PATTERN = re.compile(r"^0[xX][0-9A-Fa-f]{4}$")  # RID is 16-bit like DID
_CAN_ID_PATTERN = re.compile(r"^0[xX][0-9A-Fa-f]{1,8}$")

# ISO 14229-1 DID constraints
MAX_DID_COUNT     = 64
MAX_DTC_COUNT     = 128
MAX_ROUTINE_COUNT = 32
CAN_ID_11BIT_MAX = 0x7FF
CAN_ID_29BIT_MAX = 0x1FFFFFFF


# =============================================================================
# Utilities
# =============================================================================

def _fatal(message: str, code: int = 1) -> None:
    """Print formatted error to stderr and terminate with exit code."""
    print(f"\n[codegen] ERROR: {message}", file=sys.stderr)
    print(f"[codegen] Exit code: {code}", file=sys.stderr)
    sys.exit(code)


def _warn(message: str) -> None:
    """Print a non-fatal warning to stderr."""
    print(f"[codegen] WARN: {message}", file=sys.stderr)


def _now_utc() -> str:
    """Return current time in ISO 8601 UTC format."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _c_identifier(name: str) -> str:
    """
    Convert a human-readable name into a safe lowercase C identifier.

    Examples:
        "Engine Speed"         -> "engine_speed"
        "CoolantTemp (degC)"   -> "coolanttemp_degc"
        "0xF190 — VIN"         -> "0xf190_vin"
    """
    s = name.lower()
    s = re.sub(r"[^a-z0-9]+", "_", s)
    return s.strip("_")


def _normalise_hex(raw: str) -> str:
    """Normalise a hex string to uppercase '0x' prefix form: '0xF190'."""
    return "0x" + raw[2:].upper()


def _parse_can_id(raw: str, field_name: str) -> int:
    """
    Parse and validate a CAN ID string from YAML.

    Accepts 11-bit (0x000–0x7FF) and 29-bit (0x00000000–0x1FFFFFFF) IDs.

    Returns:
        Integer CAN ID value.
    Raises:
        SystemExit(1): if the ID is out of valid range or malformed.
    """
    if not isinstance(raw, str) or not _CAN_ID_PATTERN.match(raw):
        _fatal(
            f"VALIDATION: {field_name} must be a hex string "
            f"(e.g. '0x7DF'), got: {raw!r}"
        )
    val = int(raw, 16)
    if val > CAN_ID_29BIT_MAX:
        _fatal(
            f"VALIDATION: {field_name} value {raw} (={val}) exceeds "
            f"maximum 29-bit CAN ID (0x{CAN_ID_29BIT_MAX:08X})."
        )
    return val


# =============================================================================
# STEP 1 — Load
# =============================================================================

def load_config(config_path: Path) -> Dict[str, Any]:
    """
    Load and parse a YAML configuration file.

    Args:
        config_path: Absolute path to the YAML file.

    Returns:
        Parsed configuration dictionary.

    Raises:
        SystemExit(2): on file-not-found or YAML parse failure.
    """
    if not config_path.is_file():
        _fatal(f"Configuration file not found: {config_path}", code=2)

    with config_path.open("r", encoding="utf-8") as fh:
        try:
            cfg: Any = yaml.safe_load(fh)
        except yaml.YAMLError as exc:
            _fatal(f"YAML parse error in '{config_path}': {exc}", code=2)

    if not isinstance(cfg, dict):
        _fatal(f"Configuration root must be a YAML mapping: {config_path}", code=2)

    return cfg  # type: ignore[return-value]


# =============================================================================
# STEP 2 — Validate
# =============================================================================

def validate_config(cfg: Dict[str, Any]) -> None:
    """
    Validate the loaded configuration against schema and constraint rules.

    Validation scope:
      - Required top-level sections: metadata, timing, dids.
      - metadata.ecu_name and metadata.version: non-empty strings.
      - timing: p2_server_max_ms, p2_star_server_max_ms, s3_server_timeout_ms
                are positive integers; P2 < P2* (ISO 14229-1 constraint).
      - can (optional): rx_can_id and tx_can_id are valid hex CAN IDs.
      - dids: no duplicates, 4-digit hex IDs, valid session/access values,
              data_length in [1, 4095], count <= MAX_DID_COUNT.
      - dtcs (optional): no duplicates, 6-digit hex codes, count <= MAX_DTC_COUNT.
      - service_ids (optional): each value in [0x10, 0xFE].

    Raises:
        SystemExit(1): on any validation failure, with a descriptive message.
    """

    # ── Required sections ────────────────────────────────────────────────────
    for section in ("metadata", "timing", "dids"):
        if section not in cfg:
            _fatal(f"VALIDATION: Missing required section '{section}'.")

    # ── metadata ─────────────────────────────────────────────────────────────
    meta = cfg["metadata"]
    if not isinstance(meta, dict):
        _fatal("VALIDATION: 'metadata' must be a YAML mapping.")
    for field in ("ecu_name", "version"):
        val = meta.get(field)
        if not isinstance(val, str) or not val.strip():
            _fatal(f"VALIDATION: metadata.{field} must be a non-empty string.")

    # Validate ecu_name is a valid C identifier base
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", meta["ecu_name"]):
        _warn(
            f"metadata.ecu_name '{meta['ecu_name']}' contains characters that "
            "may not be suitable for a C macro name. Consider using only "
            "alphanumeric characters and underscores."
        )

    # ── timing ───────────────────────────────────────────────────────────────
    timing = cfg["timing"]
    if not isinstance(timing, dict):
        _fatal("VALIDATION: 'timing' must be a YAML mapping.")

    for key in ("p2_server_max_ms", "p2_star_server_max_ms", "s3_server_timeout_ms"):
        val = timing.get(key)
        if not isinstance(val, int) or val <= 0:
            _fatal(
                f"VALIDATION: timing.{key} must be a positive integer, got: {val!r}"
            )

    if timing["p2_server_max_ms"] >= timing["p2_star_server_max_ms"]:
        _fatal(
            "VALIDATION: timing.p2_server_max_ms must be strictly less than "
            "timing.p2_star_server_max_ms (ISO 14229-1 §6.2.3 constraint)."
        )

    # ── can (optional) ───────────────────────────────────────────────────────
    can_cfg = cfg.get("can", {})
    if can_cfg:
        if not isinstance(can_cfg, dict):
            _fatal("VALIDATION: 'can' must be a YAML mapping.")
        for field in ("rx_can_id", "tx_can_id"):
            raw = can_cfg.get(field)
            if raw is not None:
                val = _parse_can_id(raw, f"can.{field}")
                if val > CAN_ID_11BIT_MAX:
                    _warn(
                        f"can.{field} = {raw} ({val}) is a 29-bit extended CAN ID. "
                        "Ensure the CAN controller is configured for extended addressing."
                    )
        rx = can_cfg.get("rx_can_id")
        tx = can_cfg.get("tx_can_id")
        if rx is not None and tx is not None and rx == tx:
            _fatal(
                f"VALIDATION: can.rx_can_id and can.tx_can_id must be different "
                f"(both are {rx})."
            )

    # ── dids ─────────────────────────────────────────────────────────────────
    dids: List[Any] = cfg.get("dids", [])

    if not isinstance(dids, list) or len(dids) == 0:
        _fatal("VALIDATION: 'dids' must be a non-empty list.")

    if len(dids) > MAX_DID_COUNT:
        _fatal(f"VALIDATION: {len(dids)} DIDs exceed maximum of {MAX_DID_COUNT}.")

    seen_dids: Dict[str, int] = {}

    for idx, did in enumerate(dids):
        pfx = f"VALIDATION: dids[{idx}]"

        if not isinstance(did, dict):
            _fatal(f"{pfx}: each DID entry must be a YAML mapping.")

        # id — must be 4-digit hex
        did_id: Optional[str] = did.get("id")
        if not isinstance(did_id, str) or not _DID_PATTERN.match(did_id):
            _fatal(
                f"{pfx}: 'id' must be a 4-digit hex string like 0xF190, "
                f"got: {did_id!r}"
            )
        assert did_id is not None
        key = _normalise_hex(did_id)

        # Duplicate check
        if key in seen_dids:
            _fatal(
                f"{pfx}: DID {key} is already declared at dids[{seen_dids[key]}]. "
                "Each DID must be declared exactly once."
            )
        seen_dids[key] = idx

        # Reserved DID range check (ISO 14229-1 §B.2)
        did_val = int(did_id, 16)
        if did_val in (0x0000, 0xFFFF):
            _fatal(
                f"{pfx}: DID {key} is reserved by ISO 14229-1 §B.2 "
                "and must not be used."
            )

        # name — non-empty string
        if not isinstance(did.get("name"), str) or not did["name"].strip():
            _fatal(f"{pfx}: 'name' must be a non-empty string.")

        # access — list of 'read' / 'write'
        access = did.get("access", [])
        if not isinstance(access, list) or len(access) == 0:
            _fatal(f"{pfx}: 'access' must be a non-empty list.")
        for acc in access:
            if acc not in ("read", "write"):
                _fatal(
                    f"{pfx}: invalid access value '{acc}'. "
                    "Must be 'read', 'write', or both."
                )

        # min_session
        min_sess = did.get("min_session", "default")
        if min_sess not in SESSION_MAP:
            _fatal(
                f"{pfx}: unknown min_session '{min_sess}'. "
                f"Valid values: {sorted(SESSION_MAP.keys())}"
            )

        # security levels
        for field in ("read_security_level", "write_security_level"):
            lvl = did.get(field, 0)
            if not isinstance(lvl, int) or not (0 <= lvl <= 0xFF):
                _fatal(
                    f"{pfx}: '{field}' must be an integer in [0, 255], "
                    f"got: {lvl!r}"
                )
            # Odd values only valid for non-zero levels (matches 0x27 sub-function)
            if lvl > 0 and (lvl % 2) == 0:
                _warn(
                    f"dids[{idx}].{field} = {lvl} is even. "
                    "ISO 14229-1 §10.4: SecurityAccess sub-function values "
                    "for requestSeed use odd numbers. "
                    "Ensure this matches your 0x27 handler configuration."
                )

        # data_length
        data_len = did.get("data_length", 1)
        if not isinstance(data_len, int) or not (1 <= data_len <= 4095):
            _fatal(
                f"{pfx}: 'data_length' must be in [1, 4095], "
                f"got: {data_len!r}"
            )

    # ── dtcs ─────────────────────────────────────────────────────────────────
    dtcs: List[Any] = cfg.get("dtcs", [])

    if not isinstance(dtcs, list):
        _fatal("VALIDATION: 'dtcs' must be a list (can be empty).")

    if len(dtcs) > MAX_DTC_COUNT:
        _fatal(f"VALIDATION: {len(dtcs)} DTCs exceed maximum of {MAX_DTC_COUNT}.")

    seen_dtcs: Dict[str, int] = {}

    for idx, dtc in enumerate(dtcs):
        pfx = f"VALIDATION: dtcs[{idx}]"

        if not isinstance(dtc, dict):
            _fatal(f"{pfx}: each DTC entry must be a YAML mapping.")

        dtc_code: Optional[str] = dtc.get("code")
        if not isinstance(dtc_code, str) or not _DTC_PATTERN.match(dtc_code):
            _fatal(
                f"{pfx}: 'code' must be a 6-digit hex string like 0xC00100, "
                f"got: {dtc_code!r}"
            )
        assert dtc_code is not None
        key = _normalise_hex(dtc_code)
        if key in seen_dtcs:
            _fatal(
                f"{pfx}: DTC {key} already declared at dtcs[{seen_dtcs[key]}]. "
                "Each DTC code must be declared exactly once."
            )
        seen_dtcs[key] = idx

        # description is optional but recommended
        if not dtc.get("description"):
            _warn(f"dtcs[{idx}] ({key}): no 'description' provided.")


    # ── routines (optional) ───────────────────────────────────────────────────
    routines: List[Any] = cfg.get("routines", [])

    if not isinstance(routines, list):
        _fatal("VALIDATION: 'routines' must be a list (can be empty).")

    if len(routines) > MAX_ROUTINE_COUNT:
        _fatal(f"VALIDATION: {len(routines)} routines exceed maximum of {MAX_ROUTINE_COUNT}.")

    seen_rids: Dict[str, int] = {}

    for idx, routine in enumerate(routines):
        pfx = f"VALIDATION: routines[{idx}]"

        if not isinstance(routine, dict):
            _fatal(f"{pfx}: each routine entry must be a YAML mapping.")

        rid_raw = routine.get("id")
        if not rid_raw:
            _fatal(f"{pfx}: 'id' field is required.")

        if not isinstance(rid_raw, str) or not _RID_PATTERN.match(rid_raw):
            _fatal(
                f"{pfx}: 'id' must be a 4-digit hex string (e.g. '0xFF00'), "
                f"got: {rid_raw!r}"
            )

        key = rid_raw.upper()
        if key in seen_rids:
            _fatal(
                f"{pfx}: RID {key} already declared at routines[{seen_rids[key]}]. "
                "Each routine ID must be declared exactly once."
            )
        seen_rids[key] = idx

        if not routine.get("name"):
            _fatal(f"{pfx}: 'name' field is required.")

        session = routine.get("min_session", "default")
        if session not in SESSION_MAP:
            _fatal(
                f"{pfx}: 'min_session' must be one of "
                f"{list(SESSION_MAP.keys())}, got: {session!r}"
            )

        sec = routine.get("security_level", 0)
        if not isinstance(sec, int) or sec < 0:
            _fatal(f"{pfx}: 'security_level' must be a non-negative integer, got: {sec!r}")

        support = routine.get("support", ["start"])
        if not isinstance(support, list) or not support:
            _fatal(f"{pfx}: 'support' must be a non-empty list (e.g. ['start', 'stop']).")
        for s in support:
            if s not in ("start", "stop", "results"):
                _fatal(
                    f"{pfx}: 'support' values must be 'start', 'stop', or 'results', "
                    f"got: {s!r}"
                )
        if "start" not in support:
            _fatal(f"{pfx}: 'start' must always be in 'support' — it is mandatory per ISO 14229-1.")

    # ── service_ids (optional override list) ─────────────────────────────────
    service_ids: List[Any] = cfg.get("service_ids", [])
    for idx, sid in enumerate(service_ids):
        if not isinstance(sid, int) or not (0x10 <= sid <= 0xFE):
            _fatal(
                f"VALIDATION: service_ids[{idx}]: value {sid!r} must be an "
                "integer in [0x10, 0xFE]."
            )


# =============================================================================
# STEP 3 — Build template contexts
# =============================================================================

def _security_level_label(level: int) -> str:
    """
    Return a human-readable, audit-accurate label for a YAML security level
    integer as it will appear in the generated C wrapper comments.

    FIX [H4]: The previous template emitted only the raw integer
    (e.g. "Security level >= 1"), which was ambiguous: readers and auditors
    could not tell whether "1" referred to the YAML configuration field, the
    UDS sub-function byte, or some other encoding.  A previous iteration of
    the template incorrectly added 1 to the YAML value before emitting it,
    producing "Security level >= 2" for write_security_level:1 — a
    documentation off-by-one that would mislead a security reviewer into
    thinking Level 2 authentication was required when only Level 1 was.

    Mapping (ISO 14229-1 §10.4.2, uds_security.h UDS_SEC_LEVEL_* defines):
      YAML level 0 → UDS_SEC_LEVEL_UNLOCKED (0x00) — no authentication required
      YAML level 1 → UDS_SEC_LEVEL_1_SEED   (0x01) — Level 1 seed sub-function
      YAML level 2 → UDS_SEC_LEVEL_1_KEY    (0x02) — Level 1 key sub-function
      YAML level 3 → UDS_SEC_LEVEL_2_SEED   (0x03) — Level 2 seed sub-function
      YAML level 4 → UDS_SEC_LEVEL_2_KEY    (0x04) — Level 2 key sub-function

    Runtime correspondence: entry->write_access_level (or read_access_level)
    stores the YAML integer directly.  uds_safety_validate_did_access() calls
    uds_security_is_unlocked(ctx, required_security_level, &is_unlocked), which
    compares ctx->active_level == required_security_level.  ctx->active_level is
    set from the seed sub-function byte passed in the UDS 0x27 PDU — so for
    Level 1, active_level == 0x01 == UDS_SEC_LEVEL_1_SEED.  The YAML integer
    is therefore the exact byte value compared at runtime, with no offset.

    Args:
        level: YAML security level integer (0 = unlocked, 1..N = authenticated).

    Returns:
        String suitable for embedding in a generated C comment.
    """
    # Mapping of YAML integer → UDS_SEC_LEVEL_* constant name
    _CONST_MAP: Dict[int, str] = {
        0: "UDS_SEC_LEVEL_UNLOCKED",
        1: "UDS_SEC_LEVEL_1_SEED",
        2: "UDS_SEC_LEVEL_1_KEY",
        3: "UDS_SEC_LEVEL_2_SEED",
        4: "UDS_SEC_LEVEL_2_KEY",
    }
    const_name = _CONST_MAP.get(level, f"0x{level:02X}")
    if level == 0:
        return f"0 ({const_name} — no authentication required)"
    return f"{level} ({const_name} = 0x{level:02X})"


def _build_did_list(cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    """
    Enrich raw YAML DID entries with computed fields for template consumption.

    Returns a list of dictionaries, one per DID, with:
      id, id_int, name, c_name,
      access_read, access_write,
      min_session (C constant string), min_session_ordinal,
      read_security_level,  read_security_level_label,
      write_security_level, write_security_level_label,
      data_length.

    The *_label fields are pre-computed human-readable strings used by
    did_safety_wrappers.h.j2 to emit unambiguous security level annotations
    in generated wrapper documentation (fix H4).
    """
    result = []
    for did in cfg.get("dids", []):
        raw_id  = did["id"]
        norm_id = _normalise_hex(raw_id)
        c_name  = _c_identifier(did["name"])
        r_level = did.get("read_security_level",  0)
        w_level = did.get("write_security_level", 0)

        result.append({
            "id":                         norm_id,
            "id_int":                     int(raw_id, 16),
            "name":                       did["name"],
            "c_name":                     c_name,
            "access_read":                "read"  in did.get("access", []),
            "access_write":               "write" in did.get("access", []),
            "min_session":                SESSION_MAP.get(
                                              did.get("min_session", "default"),
                                              "UDS_SESSION_DEFAULT"),
            "min_session_ordinal":        SESSION_ORDINAL.get(
                                              did.get("min_session", "default"), 1),
            "read_security_level":        r_level,
            "read_security_level_label":  _security_level_label(r_level),
            "write_security_level":       w_level,
            "write_security_level_label": _security_level_label(w_level),
            "data_length":                did.get("data_length", 4),
        })
    return result


def _build_dtc_list(cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    """
    Enrich raw YAML DTC entries with computed fields for template consumption.
    """
    result = []
    for dtc in cfg.get("dtcs", []):
        raw_code = dtc["code"]
        norm     = _normalise_hex(raw_code)
        result.append({
            "code":        norm,
            "code_int":    int(raw_code, 16),
            "description": dtc.get("description", ""),
            "severity":    dtc.get("severity", "check_at_next_halt"),
        })
    return result



def _build_routine_list(cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    """
    Enrich raw YAML routine entries with computed fields for template consumption.

    Returns a list of dicts, one per routine, with:
      id, id_int, name, c_name,
      min_session, min_session_ordinal,
      security_level, security_level_label,
      support_start, support_stop, support_results,
      support_flags_c  (C bitfield expression for routine_database_register)
    """
    result = []
    for routine in cfg.get("routines", []):
        raw_id  = routine["id"]
        norm_id = _normalise_hex(raw_id)
        c_name  = _c_identifier(routine["name"])
        sec     = routine.get("security_level", 0)
        support = routine.get("support", ["start"])

        flags_parts = []
        if "start"   in support: flags_parts.append("ROUTINE_SUPPORT_START")
        if "stop"    in support: flags_parts.append("ROUTINE_SUPPORT_STOP")
        if "results" in support: flags_parts.append("ROUTINE_SUPPORT_RESULTS")

        result.append({
            "id":                      norm_id,
            "id_int":                  int(raw_id, 16),
            "name":                    routine["name"],
            "c_name":                  c_name,
            "min_session":             SESSION_MAP.get(
                                           routine.get("min_session", "extended"),
                                           "UDS_SESSION_EXTENDED"),
            "min_session_ordinal":     SESSION_ORDINAL.get(
                                           routine.get("min_session", "extended"), 3),
            "security_level":          sec,
            "security_level_label":    _security_level_label(sec),
            "support_start":           "start"   in support,
            "support_stop":            "stop"    in support,
            "support_results":         "results" in support,
            "support":                 list(support),           # [NEW-H2 FIX] preserve original
                                                                # list so generate_gui_types()
                                                                # can read r.get("support") and
                                                                # emit the correct TS support array.
                                                                # Without this key the dict lookup
                                                                # always returned the default
                                                                # ["start"], silently truncating
                                                                # routines that include "results"
                                                                # or "stop" sub-functions.
            "support_flags_c":         " | ".join(flags_parts) if flags_parts
                                       else "ROUTINE_SUPPORT_START",
            "description":             routine.get("description", ""),
        })
    return result

def _build_can_context(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """
    Extract CAN addressing from config, applying defaults if not present.

    Defaults:
        rx_can_id: 0x7DF  (ISO 15765-4 functional addressing)
        tx_can_id: 0x7E8  (ISO 15765-4 physical response ECU address 0)
    """
    can_cfg = cfg.get("can", {}) or {}
    rx_raw  = can_cfg.get("rx_can_id", "0x7DF")
    tx_raw  = can_cfg.get("tx_can_id", "0x7E8")
    rx_val  = int(rx_raw, 16)
    tx_val  = int(tx_raw, 16)
    return {
        "rx_can_id":       _normalise_hex(rx_raw),
        "tx_can_id":       _normalise_hex(tx_raw),
        "rx_can_id_int":   rx_val,
        "tx_can_id_int":   tx_val,
        "rx_is_extended":  rx_val > CAN_ID_11BIT_MAX,
        "tx_is_extended":  tx_val > CAN_ID_11BIT_MAX,
    }


def build_generated_config_context(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Build the template context for generated_config.h."""
    timing = cfg["timing"]
    meta   = cfg["metadata"]
    dids   = cfg.get("dids", [])
    dtcs   = cfg.get("dtcs", [])
    can    = _build_can_context(cfg)
    return {
        "ecu_name":              meta["ecu_name"],
        "version":               meta["version"],
        "generated":             _now_utc(),
        "p2_server_max_ms":      timing["p2_server_max_ms"],
        "p2_star_server_max_ms": timing["p2_star_server_max_ms"],
        "s3_server_timeout_ms":  timing["s3_server_timeout_ms"],
        "did_count":             len(dids),
        "dtc_count":             len(dtcs),
        "can_rx_id":             can["rx_can_id"],
        "can_tx_id":             can["tx_can_id"],
        "can_rx_id_int":         can["rx_can_id_int"],
        "can_tx_id_int":         can["tx_can_id_int"],
        "can_rx_is_extended":    can["rx_is_extended"],
        "can_tx_is_extended":    can["tx_is_extended"],
    }


def build_did_handlers_context(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Build the template context for did_handlers.h and did_handlers.c."""
    meta = cfg["metadata"]
    return {
        "ecu_name":  meta["ecu_name"],
        "version":   meta["version"],
        "generated": _now_utc(),
        "dids":      _build_did_list(cfg),
    }


def build_uds_init_context(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Build the template context for uds_init.h and uds_init.c."""
    timing = cfg["timing"]
    meta   = cfg["metadata"]
    can    = _build_can_context(cfg)

    # SafeBoot — optional MCUboot DFU integration
    safeboot_block   = cfg.get("safeboot", {}) or {}
    safeboot_enabled = bool(safeboot_block.get("enabled", False))
    safeboot_platform = safeboot_block.get("platform", "zephyr")
    safeboot_max_block = int(safeboot_block.get("max_block_length", 256))

    # Transport configuration (v1.6.0 — additive, optional)
    # transport: can   — ISO-TP over CAN only (default when ecu: block absent)
    # transport: doip  — DoIP over Ethernet only (EDS_DOIP_ONLY_BUILD)
    # transport: both  — ISO-TP CAN + DoIP simultaneously
    ecu_block  = cfg.get("ecu", {}) or {}
    transport  = ecu_block.get("transport", "can").lower()
    doip_block = ecu_block.get("doip", {}) or {}
    doip_logical_address = doip_block.get("logical_address", "0xE400")
    doip_source_address  = doip_block.get("source_address", "0x0E00")
    doip_port            = int(doip_block.get("port", 13400))
    is_doip = transport in ("doip", "both")

    return {
        "ecu_name":              meta["ecu_name"],
        "version":               meta["version"],
        "generated":             _now_utc(),
        "p2_server_max_ms":      timing["p2_server_max_ms"],
        "p2_star_server_max_ms": timing["p2_star_server_max_ms"],
        "s3_server_timeout_ms":  timing["s3_server_timeout_ms"],
        "can_rx_id":             can["rx_can_id"],
        "can_tx_id":             can["tx_can_id"],
        "dids":                  _build_did_list(cfg),
        "dtcs":                  _build_dtc_list(cfg),
        "routines":              _build_routine_list(cfg),
        "safeboot_enabled":      safeboot_enabled,
        "safeboot_platform":     safeboot_platform,
        "safeboot_max_block":    safeboot_max_block,
        # Transport context (v1.6.0)
        "transport":             transport,
        "is_doip":               is_doip,
        "doip_logical_address":  doip_logical_address,
        "doip_source_address":   doip_source_address,
        "doip_port":             doip_port,
    }


# =============================================================================
# Phase 3 — ASIL-B safety validation (Step 2B)
# =============================================================================

def validate_safety_config(cfg: Dict[str, Any], asil_level: str = "B") -> None:
    """
    Perform ASIL-B-oriented safety validation on the loaded configuration.

    This function runs in addition to validate_config() when --safety-wrappers
    or --asil-level is specified.  It enforces stricter constraints suited
    to safety-relevant ECU software:

      1. Every DID must have data_length <= ASIL_B_MAX_DID_DATA_LEN.
         (Prevents unbounded static buffer requirements.)
      2. DID count must not exceed ASIL_B_MAX_DID_COUNT.
         (Compile-time array sizing constraint.)
      3. Every write-capable DID is encouraged to have write_security_level > 0.
         (Advisory: ASIL_B_REQUIRE_WRITE_SECURITY controls whether this is fatal.)
      4. Timing constraints must meet minimum ASIL-B response headroom:
         p2_server_max_ms >= 10 ms (below 10 ms is unrealistic for ASIL-B).
      5. CAN addressing section must be present in ASIL builds (no defaults).

    Args:
        cfg:        Validated configuration dictionary (validate_config passed).
        asil_level: ASIL target level string ("QM", "A", "B", "C", "D").

    Raises:
        SystemExit(1): on any ASIL-B safety constraint violation.
    """
    if asil_level == "QM":
        # No additional constraints for QM builds.
        return

    _warn(f"ASIL-{asil_level} safety validation active.")

    dids   = cfg.get("dids", [])
    timing = cfg["timing"]

    # ── 1. DID count upper bound ─────────────────────────────────────────────
    if len(dids) > ASIL_B_MAX_DID_COUNT:
        _fatal(
            f"SAFETY: {len(dids)} DIDs exceed ASIL-B maximum of "
            f"{ASIL_B_MAX_DID_COUNT}.  Increase ASIL_B_MAX_DID_COUNT or "
            "split into multiple diagnostic configurations."
        )

    # ── 2. Per-DID data_length bound ─────────────────────────────────────────
    for idx, did in enumerate(dids):
        data_len = did.get("data_length", 1)
        if data_len > ASIL_B_MAX_DID_DATA_LEN:
            _fatal(
                f"SAFETY: dids[{idx}] (id={did.get('id', '?')!r}) "
                f"data_length={data_len} exceeds ASIL-B maximum of "
                f"{ASIL_B_MAX_DID_DATA_LEN} bytes.  "
                "Large DID data should be split into multiple DIDs or "
                "handled via upload/download services."
            )

        # ── 3. Write security advisory ────────────────────────────────────────
        access = did.get("access", [])
        write_sec = did.get("write_security_level", 0)
        if "write" in access and write_sec == 0:
            if ASIL_B_REQUIRE_WRITE_SECURITY:
                _fatal(
                    f"SAFETY [HIGH-1]: dids[{idx}] (id={did.get('id', '?')!r}) "
                    f"'{did.get('name', '')}' has write access but "
                    "write_security_level=0.  "
                    "ASIL-B policy (ISO 26262-6) requires that every write-capable "
                    "DID is protected by a SecurityAccess challenge. "
                    "FIX: set write_security_level >= 1 in diagnostics_config.yaml "
                    "for this DID.  "
                    "OVERRIDE (requires formal deviation record per ISO 26262-8 §7): "
                    "set ASIL_B_REQUIRE_WRITE_SECURITY = False in tools/codegen.py "
                    "with a documented justification referencing the DID ID and "
                    "the risk assessment that permits unauthenticated write access."
                )
            else:
                _warn(
                    f"SAFETY ADVISORY: dids[{idx}] (id={did.get('id', '?')!r}) "
                    "has write access but write_security_level=0. "
                    "Consider setting write_security_level >= 1 for ASIL-B."
                )

    # ── 4. Timing headroom ───────────────────────────────────────────────────
    p2 = timing["p2_server_max_ms"]
    if p2 < 10:
        _fatal(
            f"SAFETY: timing.p2_server_max_ms={p2} ms is below the ASIL-B "
            "minimum of 10 ms.  Values below 10 ms are impractical for "
            "interrupt-driven CAN controllers and create protocol violations."
        )

    # ── 5. CAN section must be explicit in ASIL builds ───────────────────────
    if not cfg.get("can"):
        _warn(
            "SAFETY ADVISORY: No 'can' section found in config. "
            "ASIL builds should explicitly declare rx_can_id and tx_can_id "
            "rather than relying on defaults (0x7DF / 0x7E8)."
        )


# =============================================================================
# Phase 3 — Safety context builders
# =============================================================================

def build_safety_config_context(
    cfg:        Dict[str, Any],
    asil_level: str = "B",
) -> Dict[str, Any]:
    """
    Build the template context for the generated safety_config.h header.

    safety_config.h emits compile-time macros that control which safety
    check categories are active at build time:
      - GEN_ASIL_LEVEL               Numeric ASIL level (QM=0, A=1, B=2, C=3, D=4)
      - GEN_SAFETY_ENABLE_*          Feature-gate macros matching uds_safety.h
      - GEN_SAFETY_MAX_DID_DATA_LEN  DID buffer bound from YAML validation
      - GEN_SAFETY_DTC_SUPPORT_MASK  DTC status availability mask

    Args:
        cfg:        Validated configuration dictionary.
        asil_level: ASIL target level string.

    Returns:
        Dictionary suitable for rendering the safety_config.h.j2 template.
    """
    asil_map: Dict[str, int] = {"QM": 0, "A": 1, "B": 2, "C": 3, "D": 4}
    asil_numeric = asil_map.get(asil_level.upper(), 2)

    # All safety checks active at ASIL-A and above; QM has no mandatory checks.
    checks_active = (asil_numeric >= 1)

    meta   = cfg["metadata"]
    timing = cfg["timing"]
    dids   = cfg.get("dids", [])

    # Compute the maximum data_length across all DIDs for buffer sizing.
    max_did_data_len = max(
        (d.get("data_length", 1) for d in dids),
        default=1,
    )

    return {
        "ecu_name":                   meta["ecu_name"],
        "version":                    meta["version"],
        "generated":                  _now_utc(),
        "asil_level":                 asil_level.upper(),
        "asil_level_numeric":         asil_numeric,
        "enable_session_checks":      (1 if checks_active else 0),
        "enable_security_checks":     (1 if checks_active else 0),
        "enable_bounds_checks":       (1 if checks_active else 0),
        "enable_null_checks":         (1 if checks_active else 0),
        "max_did_data_len":           max_did_data_len,
        "dtc_support_mask":           0xFF,
        "p2_server_max_ms":           timing["p2_server_max_ms"],
        "did_count":                  len(dids),
    }


def build_did_safety_wrappers_context(
    cfg:        Dict[str, Any],
    asil_level: str = "B",
) -> Dict[str, Any]:
    """
    Build the template context for did_safety_wrappers.h and .c.

    Generates a per-DID safe accessor wrapper function for each DID declared
    in the config.  Each wrapper:
      1. Calls uds_safety_check_null_ptr() on all pointer arguments.
      2. Calls uds_safety_find_did() to verify the DID is registered.
      3. Calls uds_safety_validate_did_access() with session + security contexts.
      4. Calls uds_safety_check_did_data_length() against buf_len.
      5. Invokes the underlying DID read/write callback.

    This allows service handlers (service_0x22.c, service_0x2E.c) to call a
    single safe accessor rather than assembling the check sequence inline.

    Args:
        cfg:        Validated configuration dictionary.
        asil_level: ASIL target level string.

    Returns:
        Dictionary suitable for rendering did_safety_wrappers.h/.c templates.
    """
    meta = cfg["metadata"]
    dids = _build_did_list(cfg)

    return {
        "ecu_name":    meta["ecu_name"],
        "version":     meta["version"],
        "generated":   _now_utc(),
        "asil_level":  asil_level.upper(),
        "dids":        dids,
    }


# =============================================================================
# STEP 4 — Render templates and write outputs
# =============================================================================

# (template_name, context_builder, output_filename)
RENDER_PLAN: List[Tuple[str, Any, str]] = [
    ("generated_config.h.j2", build_generated_config_context, "generated_config.h"),
    ("did_handlers.h.j2",     build_did_handlers_context,     "did_handlers.h"),
    ("did_handlers.c.j2",     build_did_handlers_context,     "did_handlers.c"),
    ("uds_init.h.j2",         build_uds_init_context,         "uds_init.h"),
    ("uds_init.c.j2",         build_uds_init_context,         "uds_init.c"),
]


def render_and_write(
    cfg:          Dict[str, Any],
    template_dir: Path,
    output_dir:   Path,
) -> List[str]:
    """
    Render all templates from RENDER_PLAN and write to output_dir.

    Args:
        cfg:          Validated configuration dictionary.
        template_dir: Directory containing Jinja2 .j2 template files.
        output_dir:   Directory to write generated output files into.

    Returns:
        List of paths (as strings) for all successfully written files.

    Raises:
        SystemExit(3): on any Jinja2 rendering error (StrictUndefined enforces
                       that every template variable must be present in the context).
        SystemExit(2): if the template directory does not exist.
    """
    if not template_dir.is_dir():
        _fatal(
            f"Template directory not found: {template_dir}. "
            "Pass --template-dir to specify an alternate location.",
            code=2
        )

    output_dir.mkdir(parents=True, exist_ok=True)

    env = Environment(
        loader=FileSystemLoader(str(template_dir)),
        undefined=StrictUndefined,
        autoescape=False,
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )

    written: List[str] = []
    skipped: List[str] = []

    for template_name, context_fn, output_name in RENDER_PLAN:
        template_path = template_dir / template_name

        if not template_path.exists():
            _warn(f"Template not found, skipping: {template_name}")
            skipped.append(template_name)
            continue

        try:
            template = env.get_template(template_name)
            context  = context_fn(cfg)
            rendered = template.render(**context)
        except TemplateError as exc:
            _fatal(
                f"Jinja2 render error in template '{template_name}': {exc}\n"
                "       This usually means a variable referenced in the template "
                "is not present in the context. Check the context builder function.",
                code=3
            )

        out_path = output_dir / output_name
        out_path.write_text(rendered, encoding="utf-8")
        written.append(str(out_path.resolve()))
        print(f"  [OK]     {out_path}")

    if skipped:
        print(f"\n  [WARN]   Skipped {len(skipped)} template(s): {skipped}")

    return written


# =============================================================================
# STEP 4B — Phase 3: Render safety wrapper files
# =============================================================================

#: Safety-specific render plan — only emitted when --safety-wrappers is set.
#: Each entry: (template_filename, context_builder, output_filename)
SAFETY_RENDER_PLAN: List[Tuple[str, Any, str]] = [
    (
        "safety_config.h.j2",
        build_safety_config_context,
        "safety_config.h",
    ),
    (
        "did_safety_wrappers.h.j2",
        build_did_safety_wrappers_context,
        "did_safety_wrappers.h",
    ),
    (
        "did_safety_wrappers.c.j2",
        build_did_safety_wrappers_context,
        "did_safety_wrappers.c",
    ),
]


def render_safety_wrappers(
    cfg:          Dict[str, Any],
    template_dir: Path,
    output_dir:   Path,
    asil_level:   str = "B",
) -> List[str]:
    """
    Render Phase 3 safety wrapper templates and write to output_dir.

    Generates three files using SAFETY_RENDER_PLAN:
      safety_config.h        Compile-time ASIL feature-gate macros.
      did_safety_wrappers.h  Per-DID safe accessor declarations.
      did_safety_wrappers.c  Per-DID safe accessor implementations.

    If a safety template file is not found (e.g. when running with a Phase 2A
    template directory), a warning is printed and the file is skipped rather
    than causing a fatal error.  This preserves backward compatibility.

    Args:
        cfg:          Validated configuration dictionary.
        template_dir: Directory containing Jinja2 .j2 template files.
        output_dir:   Directory to write generated output files into.
        asil_level:   ASIL target level string (default: "B").

    Returns:
        List of paths for all successfully written safety files.

    Raises:
        SystemExit(3): on any Jinja2 rendering error.
    """
    if not template_dir.is_dir():
        _fatal(
            f"Template directory not found: {template_dir}.",
            code=2
        )

    output_dir.mkdir(parents=True, exist_ok=True)

    env = Environment(
        loader=FileSystemLoader(str(template_dir)),
        undefined=StrictUndefined,
        autoescape=False,
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )

    written: List[str] = []

    for template_name, context_fn, output_name in SAFETY_RENDER_PLAN:
        template_path = template_dir / template_name

        if not template_path.exists():
            # Safety templates are optional — skip gracefully.
            # The inline fallback writer (below) generates them without a template.
            _warn(
                f"Safety template '{template_name}' not found — "
                "generating inline fallback."
            )
            fallback_path = _write_safety_fallback(
                output_dir, output_name, cfg, asil_level, context_fn
            )
            if fallback_path:
                written.append(str(fallback_path.resolve()))
                print(f"  [OK]     {fallback_path}  (inline fallback)")
            continue

        try:
            ctx_fn   = context_fn
            # Inject asil_level into context builders that accept it.
            import inspect
            sig = inspect.signature(ctx_fn)
            if "asil_level" in sig.parameters:
                context = ctx_fn(cfg, asil_level=asil_level)
            else:
                context = ctx_fn(cfg)
            template = env.get_template(template_name)
            rendered = template.render(**context)
        except TemplateError as exc:
            _fatal(
                f"Jinja2 render error in safety template '{template_name}': {exc}",
                code=3
            )

        out_path = output_dir / output_name
        out_path.write_text(rendered, encoding="utf-8")
        written.append(str(out_path.resolve()))
        print(f"  [OK]     {out_path}")

    return written


def _write_safety_fallback(
    output_dir:  Path,
    output_name: str,
    cfg:         Dict[str, Any],
    asil_level:  str,
    context_fn:  Any,
) -> Optional[Path]:
    """
    Generate a safety file inline when the Jinja2 template is absent.

    Produces minimal-but-correct C source files that allow the project to
    compile with safety checks enabled even without the full template set.

    Returns the written Path, or None if the file could not be generated.
    """
    import inspect
    sig = inspect.signature(context_fn)
    if "asil_level" in sig.parameters:
        ctx = context_fn(cfg, asil_level=asil_level)
    else:
        ctx = context_fn(cfg)

    dids     = ctx.get("dids", [])
    ecu_name = ctx.get("ecu_name", "ECU")
    ts       = ctx.get("generated", _now_utc())
    asil     = ctx.get("asil_level", "B")
    asil_num = ctx.get("asil_level_numeric", 2)

    lines: List[str] = []

    if output_name == "safety_config.h":
        lines = [
            "// File: generated/safety_config.h",
            "// GENERATED — do NOT edit manually.",
            f"// ECU: {ecu_name}  ASIL-{asil}  Generated: {ts}",
            "",
            "#ifndef SAFETY_CONFIG_H",
            "#define SAFETY_CONFIG_H",
            "",
            "/* =======================================================",
            " * ASIL compile-time configuration",
            " * Generated by tools/codegen.py Phase 3",
            " * ======================================================= */",
            "",
            f"#define GEN_ASIL_LEVEL                    ({asil_num}U)",
            f"#define GEN_ASIL_LEVEL_STRING             \"{asil}\"",
            "",
            f"#define GEN_SAFETY_ENABLE_SESSION_CHECKS  ({1 if asil_num >= 1 else 0}U)",
            f"#define GEN_SAFETY_ENABLE_SECURITY_CHECKS ({1 if asil_num >= 1 else 0}U)",
            f"#define GEN_SAFETY_ENABLE_BOUNDS_CHECKS   ({1 if asil_num >= 1 else 0}U)",
            f"#define GEN_SAFETY_ENABLE_NULL_CHECKS     ({1 if asil_num >= 1 else 0}U)",
            "",
            f"#define GEN_SAFETY_MAX_DID_DATA_LEN       ({ctx.get('max_did_data_len', 64)}U)",
            f"#define GEN_SAFETY_DTC_SUPPORT_MASK       (0x{ctx.get('dtc_support_mask', 0xFF):02X}U)",
            f"#define GEN_SAFETY_DID_COUNT              ({ctx.get('did_count', 0)}U)",
            "",
            "/* Override uds_safety.h compile-time gates with generated values. */",
            "#ifndef UDS_SAFETY_ENABLE_SESSION_CHECKS",
            "#define UDS_SAFETY_ENABLE_SESSION_CHECKS   GEN_SAFETY_ENABLE_SESSION_CHECKS",
            "#endif",
            "#ifndef UDS_SAFETY_ENABLE_SECURITY_CHECKS",
            "#define UDS_SAFETY_ENABLE_SECURITY_CHECKS  GEN_SAFETY_ENABLE_SECURITY_CHECKS",
            "#endif",
            "#ifndef UDS_SAFETY_ENABLE_BOUNDS_CHECKS",
            "#define UDS_SAFETY_ENABLE_BOUNDS_CHECKS    GEN_SAFETY_ENABLE_BOUNDS_CHECKS",
            "#endif",
            "#ifndef UDS_SAFETY_ENABLE_NULL_CHECKS",
            "#define UDS_SAFETY_ENABLE_NULL_CHECKS      GEN_SAFETY_ENABLE_NULL_CHECKS",
            "#endif",
            "",
            "#endif /* SAFETY_CONFIG_H */",
            "",
        ]

    elif output_name == "did_safety_wrappers.h":
        lines = [
            "// File: generated/did_safety_wrappers.h",
            "// GENERATED — do NOT edit manually.",
            f"// ECU: {ecu_name}  ASIL-{asil}  Generated: {ts}",
            "",
            "#ifndef DID_SAFETY_WRAPPERS_H",
            "#define DID_SAFETY_WRAPPERS_H",
            "",
            "#include \"uds_types.h\"",
            "#include \"uds_safety.h\"",
            "#include \"did_database.h\"",
            "",
            "/* ================================================================",
            " * Per-DID safe accessor declarations (ASIL-" + asil + ")",
            " * Each wrapper performs NULL checks, session, security, and",
            " * bounds validation before invoking the DID callback.",
            " * ================================================================ */",
            "",
            "#ifdef __cplusplus",
            'extern "C" {',
            "#endif",
            "",
        ]
        for did in dids:
            c_name = did["c_name"]
            did_id = did["id"]
            if did.get("access_read"):
                lines += [
                    f"/* Safe read accessor for DID {did_id} — {did['name']} */",
                    f"uds_status_t did_safe_read_{c_name}(",
                    "    const uds_session_ctx_t  *session_ctx,",
                    "    const uds_security_ctx_t *security_ctx,",
                    "    uint8_t                  *buf,",
                    "    uint16_t                  buf_len,",
                    "    uint16_t                 *out_len",
                    ");",
                    "",
                ]
            if did.get("access_write"):
                lines += [
                    f"/* Safe write accessor for DID {did_id} — {did['name']} */",
                    f"uds_status_t did_safe_write_{c_name}(",
                    "    const uds_session_ctx_t  *session_ctx,",
                    "    const uds_security_ctx_t *security_ctx,",
                    "    const uint8_t            *buf,",
                    "    uint16_t                  len",
                    ");",
                    "",
                ]
        lines += [
            "#ifdef __cplusplus",
            "}",
            "#endif",
            "",
            "#endif /* DID_SAFETY_WRAPPERS_H */",
            "",
        ]

    elif output_name == "did_safety_wrappers.c":
        lines = [
            "// File: generated/did_safety_wrappers.c",
            "// GENERATED — do NOT edit manually.",
            f"// ECU: {ecu_name}  ASIL-{asil}  Generated: {ts}",
            "",
            "#include \"did_safety_wrappers.h\"",
            "#include \"did_handlers.h\"",
            "",
            "/* ================================================================",
            " * Per-DID safe accessor implementations (ASIL-" + asil + ")",
            " *",
            " * Each function:",
            " *   1. NULL-checks all pointer arguments (REQ-SAFE-004).",
            " *   2. Resolves the DID in the database (REQ-SAFE-001).",
            " *   3. Validates session + security access (REQ-SAFE-002/003).",
            " *   4. Checks buffer length against DID data_length (REQ-SAFE-006).",
            " *   5. Invokes the DID read/write callback.",
            " * ================================================================ */",
            "",
        ]
        for did in dids:
            c_name   = did["c_name"]
            did_id   = did["id"]
            did_id_u = f"(uint16_t){did['id_int']}U"

            if did.get("access_read"):
                lines += [
                    f"uds_status_t did_safe_read_{c_name}(",
                    "    const uds_session_ctx_t  *session_ctx,",
                    "    const uds_security_ctx_t *security_ctx,",
                    "    uint8_t                  *buf,",
                    "    uint16_t                  buf_len,",
                    "    uint16_t                 *out_len)",
                    "{",
                    "    uds_safety_result_t sr;",
                    "    const did_entry_t  *entry = NULL;",
                    "",
                    f"    /* DID {did_id} — {did['name']} */",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,  \"session_ctx\"));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx, \"security_ctx\"));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(buf,          \"buf\"));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(out_len,      \"out_len\"));",
                    f"    UDS_SAFETY_RETURN_IF_ERR(uds_safety_find_did({did_id_u}, &entry));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_did_access(",
                    "        entry, session_ctx, security_ctx, UDS_SAFETY_ACCESS_READ));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_did_data_length(entry, buf_len));",
                    "",
                    f"    return did_read_{c_name}(buf, buf_len, out_len);",
                    "}",
                    "",
                ]
            if did.get("access_write"):
                lines += [
                    f"uds_status_t did_safe_write_{c_name}(",
                    "    const uds_session_ctx_t  *session_ctx,",
                    "    const uds_security_ctx_t *security_ctx,",
                    "    const uint8_t            *buf,",
                    "    uint16_t                  len)",
                    "{",
                    "    const did_entry_t  *entry = NULL;",
                    "",
                    f"    /* DID {did_id} — {did['name']} */",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(session_ctx,  \"session_ctx\"));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(security_ctx, \"security_ctx\"));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(buf,          \"buf\"));",
                    f"    UDS_SAFETY_RETURN_IF_ERR(uds_safety_find_did({did_id_u}, &entry));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_did_access(",
                    "        entry, session_ctx, security_ctx, UDS_SAFETY_ACCESS_WRITE));",
                    "    UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_write_data_length(entry, len));",
                    "",
                    f"    return did_write_{c_name}(buf, len);",
                    "}",
                    "",
                ]
        lines.append("")

    else:
        return None

    out_path = output_dir / output_name
    out_path.write_text("\n".join(lines), encoding="utf-8")
    return out_path



#: Routine-specific render plan — emitted when routines: section present in YAML.
ROUTINE_RENDER_PLAN: List[Tuple[str, Any, str]] = [
    (
        "routine_handlers.c.j2",
        lambda cfg: {
            "ecu_name":  cfg["metadata"]["ecu_name"],
            "version":   cfg["metadata"]["version"],
            "generated": _now_utc(),
            "routines":  _build_routine_list(cfg),
        },
        "routine_handlers.c",
    ),
    (
        "routine_handlers.h.j2",
        lambda cfg: {
            "ecu_name":  cfg["metadata"]["ecu_name"],
            "version":   cfg["metadata"]["version"],
            "generated": _now_utc(),
            "routines":  _build_routine_list(cfg),
        },
        "routine_handlers.h",
    ),
]


def render_routine_handlers(
    cfg:          Dict[str, Any],
    template_dir: Path,
    output_dir:   Path,
) -> List[str]:
    """
    Render routine_handlers.h/.c when routines: section is present in YAML.
    Falls back to inline generation if templates are absent.
    """
    routines = _build_routine_list(cfg)
    if not routines:
        return []  # No routines configured — nothing to generate.

    output_dir.mkdir(parents=True, exist_ok=True)

    env = Environment(
        loader=FileSystemLoader(str(template_dir)),
        undefined=StrictUndefined,
        autoescape=False,
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )

    written: List[str] = []
    ecu_name  = cfg["metadata"]["ecu_name"]
    version   = cfg["metadata"]["version"]
    timestamp = _now_utc()

    for template_name, context_fn, output_name in ROUTINE_RENDER_PLAN:
        template_path = template_dir / template_name
        out_path      = output_dir / output_name

        if template_path.exists():
            try:
                tmpl     = env.get_template(template_name)
                ctx      = context_fn(cfg)
                rendered = tmpl.render(**ctx)
                out_path.write_text(rendered, encoding="utf-8")
            except TemplateError as exc:
                _fatal(f"Jinja2 render error in '{template_name}': {exc}", code=3)
        else:
            # Inline fallback — generate without template
            _write_routine_handlers_inline(
                out_path, output_name, routines, ecu_name, version, timestamp
            )

        written.append(str(out_path.resolve()))
        print(f"  [OK]     {out_path}")

    return written


def _write_routine_handlers_inline(
    out_path:   "Path",
    filename:   str,
    routines:   List[Dict],
    ecu_name:   str,
    version:    str,
    timestamp:  str,
) -> None:
    """Generate routine_handlers.h or .c inline without a Jinja2 template."""
    lines: List[str] = []

    if filename == "routine_handlers.h":
        lines = [
            "// File: generated/routine_handlers.h",
            "// GENERATED — do NOT edit manually.",
            f"// ECU: {ecu_name}  v{version}  Generated: {timestamp}",
            "",
            "#ifndef ROUTINE_HANDLERS_H",
            "#define ROUTINE_HANDLERS_H",
            "",
            '#include "uds_types.h"',
            '#include "routine_database.h"',
            "",
            "/* Register all routines from diagnostics_config.yaml */",
            "uds_status_t routine_handlers_register_all(void);",
            "",
        ]
        for r in routines:
            lines.append(f"/* RID {r['id']} — {r['name']} */")
            lines.append(
                f"uds_status_t routine_start_{r['c_name']}("
                "const uint8_t *opt_buf, uint8_t opt_len, "
                "uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);"
            )
            if r["support_stop"]:
                lines.append(
                    f"uds_status_t routine_stop_{r['c_name']}("
                    "const uint8_t *opt_buf, uint8_t opt_len, "
                    "uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);"
                )
            if r["support_results"]:
                lines.append(
                    f"uds_status_t routine_results_{r['c_name']}("
                    "uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);"
                )
            lines.append("")
        lines += ["#endif /* ROUTINE_HANDLERS_H */", ""]

    elif filename == "routine_handlers.c":
        lines = [
            "// File: generated/routine_handlers.c",
            "// GENERATED — do NOT edit manually.",
            f"// ECU: {ecu_name}  v{version}  Generated: {timestamp}",
            "",
            '#include "routine_handlers.h"',
            '#include "routine_database.h"',
            '#include "uds_types.h"',
            "",
            "/* =============================================================",
            " * Routine callback stubs",
            " * Replace each stub body with your ECU application logic.",
            " * ============================================================= */",
            "",
        ]
        for r in routines:
            rid  = r["id"]
            name = r["name"]
            cn   = r["c_name"]
            # start callback
            lines += [
                f"/* RID {rid} — {name} : startRoutine stub */",
                f"uds_status_t routine_start_{cn}(",
                "    const uint8_t *opt_buf, uint8_t opt_len,",
                "    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)",
                "{",
                "    (void)opt_buf; (void)opt_len; (void)result_buf;",
                "    (void)result_buf_len;",
                "    *result_len = 0U;",
                "    /* TODO: implement routine logic */",
                "    return UDS_STATUS_OK;",
                "}",
                "",
            ]
            if r["support_stop"]:
                lines += [
                    f"/* RID {rid} — {name} : stopRoutine stub */",
                    f"uds_status_t routine_stop_{cn}(",
                    "    const uint8_t *opt_buf, uint8_t opt_len,",
                    "    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)",
                    "{",
                    "    (void)opt_buf; (void)opt_len; (void)result_buf;",
                    "    (void)result_buf_len;",
                    "    *result_len = 0U;",
                    "    /* TODO: implement stop logic */",
                    "    return UDS_STATUS_OK;",
                    "}",
                    "",
                ]
            if r["support_results"]:
                lines += [
                    f"/* RID {rid} — {name} : requestRoutineResults stub */",
                    f"uds_status_t routine_results_{cn}(",
                    "    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)",
                    "{",
                    "    (void)result_buf; (void)result_buf_len;",
                    "    *result_len = 0U;",
                    "    /* TODO: return routine results */",
                    "    return UDS_STATUS_OK;",
                    "}",
                    "",
                ]

        # register_all
        lines += [
            "/* Register all routines with routine_database */",
            "uds_status_t routine_handlers_register_all(void)",
            "{",
            "    uds_status_t status;",
            "    routine_entry_t entry;",
            "",
        ]
        for r in routines:
            cn = r["c_name"]
            lines += [
                f"    /* RID {r['id']} — {r['name']} */",
                "    entry.rid            = (uint16_t)" + str(r["id_int"]) + "U;",
                "    entry.support_flags  = (uint8_t)(" + r["support_flags_c"] + ");",
                "    entry.min_session    = (uint8_t)" + r["min_session"] + ";",
                "    entry.security_level = (uint8_t)" + str(r["security_level"]) + "U;",
                f"    entry.start_cb       = routine_start_{cn};",
                f"    entry.stop_cb        = " + (f"routine_stop_{cn};" if r["support_stop"] else "NULL;"),
                f"    entry.results_cb     = " + (f"routine_results_{cn};" if r["support_results"] else "NULL;"),
                f'    entry.description    = "{r["name"]}";',
                "    status = routine_database_register(&entry);",
                "    if (status != UDS_STATUS_OK) { return status; }",
                "",
            ]
        lines += [
            "    return UDS_STATUS_OK;",
            "}",
            "",
        ]

    out_path.write_text("\n".join(lines), encoding="utf-8")

# =============================================================================
# STEP 5 — Manifest
# =============================================================================

def write_manifest(
    written_files:  List[str],
    config_path:    Path,
    output_dir:     Path,
    safety_enabled: bool = False,
    asil_level:     str  = "QM",
) -> Path:
    """
    Write the generation manifest JSON into the output directory.

    Phase 3 additions:
      - "safety_wrappers": bool indicating if safety files were generated.
      - "asil_level": ASIL level string from --asil-level flag.
      - Manifest filename updated to generated_files_phase3.json when
        safety_enabled is True, otherwise generated_files_phase2A.json
        for backward compatibility.

    Args:
        written_files:  List of absolute paths written during generation.
        config_path:    Source YAML configuration file path.
        output_dir:     Output directory for generated files.
        safety_enabled: True if --safety-wrappers was specified.
        asil_level:     ASIL level string ("QM", "A", "B", "C", "D").

    Returns:
        Path to the written manifest file.
    """
    manifest_filename = (
        "generated_files_phase3.json"
        if safety_enabled
        else "generated_files_phase2A.json"
    )
    manifest_path = output_dir / manifest_filename

    def _relative(f: str) -> str:
        p = Path(f)
        try:
            return str(p.relative_to(output_dir.resolve().parent.parent.parent))
        except ValueError:
            return f

    manifest = {
        "phase":            "Phase 3" if safety_enabled else "Phase 2A",
        "generated_at":     _now_utc(),
        "config_source":    str(config_path.resolve()),
        "output_dir":       str(output_dir.resolve()),
        "safety_wrappers":  safety_enabled,
        "asil_level":       asil_level.upper(),
        "files": [_relative(f) for f in written_files],
        "generator":        str(Path(__file__).resolve()),
    }

    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8"
    )
    print(f"  [OK]     {manifest_path}")
    return manifest_path


# =============================================================================
# Entry point
# =============================================================================

def generate_gui_types(cfg: Dict[str, Any], gui_out_dir: "Path") -> List[str]:
    """
    Generate gui/src/generated/catalog.ts from diagnostics_config.yaml.

    Produces TypeScript constants DID_CATALOG and ROUTINE_CATALOG that mirror
    the YAML configuration, eliminating the manual sync between the YAML file
    and gui/src/types/index.ts (P2-2 fix).

    The generated file is imported by the GUI components instead of the
    hand-maintained constants in types/index.ts.

    Returns a list of written file paths.
    """
    meta    = cfg.get("metadata", {})
    ecu     = meta.get("ecu_name", "ECU")
    version = meta.get("version", "0.0.0")
    now     = _now_utc()

    dids     = _build_did_list(cfg)
    routines = _build_routine_list(cfg)

    # ── Build DID_CATALOG entries ────────────────────────────────────────────
    did_entries: List[str] = []
    for d in dids:
        access    = d.get("access", ["read"])
        writeable = "write" in access
        did_type  = "ascii" if d.get("data_type") == "ascii" else \
                    "numeric" if d.get("data_length", 1) <= 4 else "hex"
        # Heuristic: VIN / serial / part numbers → ascii
        name_lc = d.get("name", "").lower()
        if any(k in name_lc for k in ("vin", "serial", "part", "number", "name")):
            did_type = "ascii"

        min_sess = d.get("min_session", "default")
        # Normalise: strip any UDS_SESSION_ prefix that may come from older YAML
        if min_sess.startswith("UDS_SESSION_"):
            min_sess = min_sess[len("UDS_SESSION_"):].lower()
        sess_map = {"default": 1, "extended": 3, "programming": 2}
        sess_num = sess_map.get(min_sess, 1)

        fields: List[str] = [
            f"hex: '{d['id']}'",
            f"name: {json.dumps(d.get('name', d['id']))}",
            f"length: {d.get('data_length', 1)}",
            f"type: '{did_type}'",
        ]
        if writeable:
            fields.append("writeable: true")
        if sess_num > 1:
            fields.append(f"sessionRequired: {sess_num}")

        entry = "  { " + ", ".join(fields) + " },"
        did_entries.append(entry)

    # ── Build ROUTINE_CATALOG entries ────────────────────────────────────────
    routine_entries: List[str] = []
    for r in routines:
        support_list = r.get("support", ["start"])
        support_ts   = ", ".join(f"'{s}'" for s in support_list)
        r_min_sess = r.get("min_session", "default")
        if r_min_sess.startswith("UDS_SESSION_"):
            r_min_sess = r_min_sess[len("UDS_SESSION_"):].lower()
        fields = [
            f"id: '{r['id']}'",
            f"name: {json.dumps(r.get('name', r['id']))}",
            f"description: {json.dumps(r.get('description', ''))}",
            f"minSession: '{r_min_sess}'",
            f"securityLevel: {r.get('security_level', 0)}",
            f"support: [{support_ts}]",
        ]
        entry = "  { " + ", ".join(fields) + " },"
        routine_entries.append(entry)

    did_block     = "\n".join(did_entries)     if did_entries     else "  // No DIDs configured"
    routine_block = "\n".join(routine_entries) if routine_entries else "  // No routines configured"

    ts_content = (
        "// =============================================================================\n"
        "// GENERATED — DO NOT EDIT MANUALLY\n"
        "//\n"
        f"// Source  : diagnostics_config.yaml (ECU: {ecu}, version: {version})\n"
        "// Tool    : tools/codegen.py --gui-types\n"
        f"// Generated: {now}\n"
        "//\n"
        "// This file is regenerated automatically whenever diagnostics_config.yaml\n"
        "// changes. It replaces the hand-maintained DID_CATALOG and ROUTINE_CATALOG\n"
        "// constants in gui/src/types/index.ts (P2-2 automation fix).\n"
        "//\n"
        "// Import in components:\n"
        "//   import { DID_CATALOG, ROUTINE_CATALOG } from '../generated/catalog';\n"
        "// =============================================================================\n"
        "\n"
        "import type { DidInfo, RoutineInfo } from '../types';\n"
        "\n"
        "/**\n"
        " * DID catalogue derived from diagnostics_config.yaml.\n"
        " * Generated by codegen.py --gui-types. Do not edit manually.\n"
        " */\n"
        "export const DID_CATALOG: DidInfo[] = [\n"
        f"{did_block}\n"
        "];\n"
        "\n"
        "/**\n"
        " * Routine catalogue derived from diagnostics_config.yaml.\n"
        " * Generated by codegen.py --gui-types. Do not edit manually.\n"
        " */\n"
        "export const ROUTINE_CATALOG: RoutineInfo[] = [\n"
        f"{routine_block}\n"
        "];\n"
        "\n"
        "/** ECU metadata from diagnostics_config.yaml. */\n"
        "export const ECU_META = {\n"
        f"  name:    {json.dumps(ecu)},\n"
        f"  version: {json.dumps(version)},\n"
        "} as const;\n"
    )

    gui_out_dir.mkdir(parents=True, exist_ok=True)
    out_path = gui_out_dir / "catalog.ts"
    out_path.write_text(ts_content, encoding="utf-8")
    print(f"  [OK]     {out_path}")
    return [str(out_path)]


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="codegen.py",
        description=(
            "Xaloqi EDS — Code Generator (Phase 3).\n"
            "Generates C source from diagnostics_config.yaml.  "
            "Pass --safety-wrappers to also emit ASIL-B safe accessor files."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  # Standard generation (Phase 2A compatible):\n"
            "  python3 tools/codegen.py \\\n"
            "      --config examples/basic_ecu/diagnostics_config.yaml \\\n"
            "      --out    generated/\n"
            "\n"
            "  # Phase 3 — ASIL-B safety wrappers:\n"
            "  python3 tools/codegen.py \\\n"
            "      --config         examples/basic_ecu/diagnostics_config.yaml \\\n"
            "      --out            generated/ \\\n"
            "      --safety-wrappers \\\n"
            "      --asil-level     B\n"
            "\n"
            "  # Phase 4 — generate integration tests alongside C code:\n"
            "  python3 tools/codegen.py \\\n"
            "      --config         examples/basic_ecu/diagnostics_config.yaml \\\n"
            "      --out            generated/ \\\n"
            "      --safety-wrappers \\\n"
            "      --test-gen\n"
            "\n"
            "  # Run generated tests (no hardware required):\n"
            "  cd generated/tests && pytest . -v\n"
            "\n"
            "  # Dry-run (validate YAML only, no file output):\n"
            "  python3 tools/codegen.py \\\n"
            "      --config examples/basic_ecu/diagnostics_config.yaml \\\n"
            "      --dry-run\n"
        ),
    )

    # ── Existing flags (Phase 2A) ────────────────────────────────────────────
    parser.add_argument(
        "--config", "-c",
        required=True,
        metavar="YAML",
        help="Path to diagnostics_config.yaml",
    )
    parser.add_argument(
        "--out", "-o",
        required=False,
        default="generated",
        metavar="DIR",
        help="Output directory for generated files (default: generated/)",
    )
    parser.add_argument(
        "--template-dir", "-t",
        required=False,
        default=None,
        metavar="DIR",
        help=(
            "Jinja2 template directory "
            "(default: <this script's directory>/templates/)"
        ),
    )
    parser.add_argument(
        "--no-manifest",
        action="store_true",
        default=False,
        help="Skip writing the JSON manifest file.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
        help=(
            "Parse and validate config (including safety checks if "
            "--safety-wrappers is set), but do not write any output files."
        ),
    )

    # ── Phase 3 flags ────────────────────────────────────────────────────────
    parser.add_argument(
        "--safety-wrappers",
        action="store_true",
        default=False,
        help=(
            "Generate ASIL-B safety wrapper files in addition to standard "
            "outputs:  safety_config.h, did_safety_wrappers.h, "
            "did_safety_wrappers.c"
        ),
    )
    parser.add_argument(
        "--asil-level",
        required=False,
        default="B",
        metavar="LEVEL",
        choices=VALID_ASIL_LEVELS,
        help=(
            "ASIL target level for safety wrapper macros.  "
            f"One of: {', '.join(VALID_ASIL_LEVELS)}.  "
            "Default: B.  Has no effect without --safety-wrappers."
        ),
    )

    # ── Phase 4 flags ────────────────────────────────────────────────────────
    parser.add_argument(
        "--test-gen",
        action="store_true",
        default=False,
        help=(
            "Generate pytest integration tests into <out>/tests/. "
            "Produces conftest.py (ISO-TP transport fixture), test_services.py "
            "(SID 0x10/0x11/0x19/0x22/0x27/0x2E/0x3E), and one test_did_XXXX.py "
            "per configured DID. Tests run in simulator mode with no hardware. "
            "See generated/tests/README.md for all modes."
        ),
    )

    # ── P2-2: GUI TypeScript catalog generation ───────────────────────────────
    parser.add_argument(
        "--gui-types",
        action="store_true",
        default=False,
        help=(
            "Generate gui/src/generated/catalog.ts containing DID_CATALOG and "
            "ROUTINE_CATALOG TypeScript constants derived from the YAML config. "
            "Eliminates the manual sync between diagnostics_config.yaml and "
            "gui/src/types/index.ts. Requires --out to point to the project root "
            "or pass --gui-out to specify the GUI generated directory explicitly."
        ),
    )
    parser.add_argument(
        "--gui-out",
        default=None,
        metavar="GUI_GENERATED_DIR",
        help=(
            "Output directory for --gui-types. Defaults to <out>/../gui/src/generated/ "
            "when --gui-types is active. Override if your GUI lives elsewhere."
        ),
    )

    args = parser.parse_args()

    config_path  = Path(args.config).resolve()
    output_dir   = Path(args.out).resolve()
    template_dir = (
        Path(args.template_dir).resolve()
        if args.template_dir
        else Path(__file__).parent / "templates"
    )
    asil_level      = args.asil_level.upper()
    safety_wrappers = args.safety_wrappers
    test_gen        = args.test_gen
    gui_types       = args.gui_types

    # ── Banner ───────────────────────────────────────────────────────────────
    print("=" * 72)
    print("  Xaloqi EDS — Code Generator (Phase 4)")
    print("=" * 72)
    print(f"  Config   : {config_path}")
    print(f"  Templates: {template_dir}")
    print(f"  Output   : {output_dir}")
    if safety_wrappers:
        print(f"  Safety   : ENABLED  (ASIL-{asil_level})")
    else:
        print("  Safety   : disabled  (pass --safety-wrappers to enable)")
    if test_gen:
        print(f"  TestGen  : ENABLED  → {output_dir / 'tests'}")
    else:
        print("  TestGen  : disabled  (pass --test-gen to enable)")
    if gui_types:
        _gui_out_default = output_dir.parent / "gui" / "src" / "generated"
        _gui_out_dir = Path(args.gui_out).resolve() if args.gui_out else _gui_out_default
        print(f"  GUITypes : ENABLED  → {_gui_out_dir}")
    else:
        print("  GUITypes : disabled  (pass --gui-types to enable)")
    if args.dry_run:
        print("  Mode     : DRY RUN (no files will be written)")
    print()

    # ── License check ────────────────────────────────────────────────────────
    # _license.py is a private module delivered with Developer/Professional
    # license ZIPs. It is NOT present in the public repository.
    # If absent → print purchase URL and exit 1.
    # If present but key missing/invalid/expired → exit 1 (or warn on grace).
    #
    # CI BYPASS: set XALOQI_LICENSE_SKIP=1 in the CI environment to skip the
    # license check entirely. This allows the public repo CI jobs to run
    # codegen without a license key. In the public repo, _license.py is absent
    # and the templates are also absent — but CI generates from the committed
    # generated/ directory and skips the template step (DIAG_SKIP_CODEGEN=ON),
    # so codegen is run in test-harness mode where it is pre-configured.
    _license_skip = (
        os.environ.get("XALOQI_LICENSE_SKIP", "").strip().lower()
        in ("1", "true", "yes")
    )

    _lic_result = None
    if not _license_skip:
        try:
            import _license as _lic_mod
            _lic_result = _lic_mod.check()
        except ImportError:
            print(
                "╔══════════════════════════════════════════════════════════════════╗",
                file=sys.stderr,
            )
            print(
                "║  Xaloqi EDS — Commercial License Required                       ║",
                file=sys.stderr,
            )
            print(
                "╠══════════════════════════════════════════════════════════════════╣",
                file=sys.stderr,
            )
            print(
                "║  Code generation requires a Developer or Professional license.  ║",
                file=sys.stderr,
            )
            print(
                "║                                                                  ║",
                file=sys.stderr,
            )
            print(
                "║  Purchase at: https://xaloqi.com                                ║",
                file=sys.stderr,
            )
            print(
                "║                                                                  ║",
                file=sys.stderr,
            )
            print(
                "║  After purchase you will receive:                               ║",
                file=sys.stderr,
            )
            print(
                "║    • A license key (JWT)                                         ║",
                file=sys.stderr,
            )
            print(
                "║    • A ZIP containing the Jinja2 templates + _license.py         ║",
                file=sys.stderr,
            )
            print(
                "║                                                                  ║",
                file=sys.stderr,
            )
            print(
                "║  Activate: python3 tools/activate.py --key <YOUR_KEY>           ║",
                file=sys.stderr,
            )
            print(
                "╚══════════════════════════════════════════════════════════════════╝",
                file=sys.stderr,
            )
            sys.exit(1)

        # _license.py is present — act on the result.
        from _license import LicenseStatus

        if _lic_result.status == LicenseStatus.MISSING:
            print(f"\n✗  LICENSE KEY NOT FOUND\n\n{_lic_result.message}\n", file=sys.stderr)
            sys.exit(1)

        if _lic_result.status == LicenseStatus.INVALID:
            print(f"\n✗  INVALID LICENSE KEY\n\n{_lic_result.message}\n", file=sys.stderr)
            sys.exit(1)

        if _lic_result.status == LicenseStatus.EXPIRED:
            print(f"\n{_lic_result.message}\n", file=sys.stderr)
            sys.exit(1)

        if _lic_result.status == LicenseStatus.GRACE:
            print(_lic_result.message, file=sys.stderr)
            print()

        if _lic_result.status == LicenseStatus.OK:
            from _license import print_license_status
            print_license_status(_lic_result)
            print()

    # ── Step 1: Load ─────────────────────────────────────────────────────────
    cfg = load_config(config_path)
    did_count = len(cfg.get("dids", []))
    dtc_count = len(cfg.get("dtcs", []))
    print(f"[1/5] Config loaded  — {did_count} DID(s), {dtc_count} DTC(s).")

    # ── Step 2: Validate (Phase 2A) ──────────────────────────────────────────
    validate_config(cfg)
    print("[2/5] Base validation passed.")

    # ── Step 2B: Safety validation (Phase 3) ─────────────────────────────────
    if safety_wrappers:
        validate_safety_config(cfg, asil_level=asil_level)
        print(f"[2B]  ASIL-{asil_level} safety validation passed.")

    if args.dry_run:
        print("\n[3/5] DRY RUN — skipping standard file generation.")
        print("[4/5] DRY RUN — skipping safety wrapper generation.")
        print("[4B]  DRY RUN — skipping test generation.")
        print("[5/5] DRY RUN — skipping manifest.")
        print("\nDry run complete. Config is valid.")
        return

    # ── Step 3: Render standard files ────────────────────────────────────────
    print("[3/5] Rendering standard templates...")
    written = render_and_write(cfg, template_dir, output_dir)
    print(f"\n      {len(written)} standard file(s) written to {output_dir}")

    # ── Step 4: Render safety wrappers (Phase 3, optional) ───────────────────
    if safety_wrappers:
        print(f"[4/5] Rendering ASIL-{asil_level} safety wrapper files...")
        safety_written = render_safety_wrappers(
            cfg, template_dir, output_dir, asil_level=asil_level
        )
        written.extend(safety_written)
        print(f"\n      {len(safety_written)} safety file(s) written to {output_dir}")
    else:
        print("[4/5] Safety wrappers skipped (pass --safety-wrappers to enable).")

    # ── Step 4C: Routine handler generation ─────────────────────────────────
    routine_written = render_routine_handlers(cfg, template_dir, output_dir)
    written.extend(routine_written)
    if routine_written:
        routine_count = len(cfg.get('routines', []))
        print(f"[4C] {len(routine_written)} routine handler file(s) written "
              f"({routine_count} routine(s)).")
    else:
        print("[4C] No routines configured — routine_handlers skipped.")

    # ── Step 4B: Test generation (Phase 4, optional) ────────────────────────
    if test_gen:
        print("[4B] Generating integration tests (Phase 4)...")
        try:
            # Import testgen lazily — not required for C-only generation
            _tg_dir = Path(__file__).parent
            import importlib.util as _ilu
            _spec = _ilu.spec_from_file_location("testgen", _tg_dir / "testgen.py")
            _testgen = _ilu.module_from_spec(_spec)  # type: ignore[arg-type]
            _spec.loader.exec_module(_testgen)        # type: ignore[union-attr]
            test_written = _testgen.generate_tests(cfg, template_dir, output_dir)
            written.extend(test_written)
            print(f"\n      {len(test_written)} test file(s) written to {output_dir / 'tests'}")
            print(f"\n      Run: cd {output_dir / 'tests'} && pytest . -v")
        except Exception as exc:
            # [MED-2 FIX] --test-gen was explicitly requested; silently swallowing
            # this exception with _warn() allowed codegen to exit 0 even when zero
            # test files were produced.  CI then passed the 'Run code generation'
            # step while the subsequent 'Verify test files generated' step was the
            # only guard — a single-point-of-failure that a truncated traceback or
            # a missing testgen.py could defeat.
            #
            # Now _fatal() with code=3 (consistent with Jinja2 render failures):
            #   - Fails the codegen step itself, not a later verification step.
            #   - Prints the full exception so the root cause is immediately visible
            #     in the CI log without needing to inspect generated/ for missing files.
            #   - Prevents any C source files written in earlier steps from being
            #     committed or used without the accompanying test suite.
            #
            # RECOVERY: fix the exception reported below, then re-run codegen.
            # Common causes:
            #   - tools/testgen.py missing or has a syntax error
            #   - A Jinja2 test template (tools/templates/test_*.j2) is missing
            #   - An undefined variable in a test template (StrictUndefined)
            #   - generate_tests() raised due to an unexpected YAML field
            import traceback as _tb
            _tb.print_exc(file=__import__('sys').stderr)
            _fatal(
                f"[MED-2] Test generation failed with --test-gen explicitly requested. "
                f"Exception: {type(exc).__name__}: {exc}  "
                "No test files were written. "
                "Fix the error above and re-run codegen. "
                "See RECOVERY comment in tools/codegen.py Step 4B for common causes.",
                code=3,
            )
    else:
        print("[4B] Test generation skipped (pass --test-gen to enable).")

    # ── Step 4C: GUI TypeScript catalog (--gui-types) ─────────────────────────
    if gui_types and not args.dry_run:
        print("[4C] Generating GUI TypeScript catalog (--gui-types)...")
        _gui_out_default = output_dir.parent / "gui" / "src" / "generated"
        _gui_out_dir = Path(args.gui_out).resolve() if args.gui_out else _gui_out_default
        gui_written = generate_gui_types(cfg, _gui_out_dir)
        written.extend(gui_written)
        print(f"\n      {len(gui_written)} GUI catalog file(s) written to {_gui_out_dir}")
        print(f"\n      Import: import {{ DID_CATALOG, ROUTINE_CATALOG }} from '../generated/catalog';")
    elif gui_types and args.dry_run:
        print("[4C] GUI TypeScript catalog skipped (dry-run).")
    else:
        print("[4C] GUI TypeScript catalog skipped (pass --gui-types to enable).")

    # ── Step 5: Manifest ──────────────────────────────────────────────────────
    if not args.no_manifest:
        print("[5/5] Writing manifest...")
        write_manifest(
            written,
            config_path,
            output_dir,
            safety_enabled=safety_wrappers,
            asil_level=asil_level,
        )
    else:
        print("[5/5] Manifest skipped (--no-manifest).")

    print()
    print("=" * 72)
    print("  Generation complete.")
    print("=" * 72)
    if test_gen:
        print()
        print(f"  Run tests: cd {output_dir / 'tests'} && pytest . -v")


if __name__ == "__main__":
    main()
