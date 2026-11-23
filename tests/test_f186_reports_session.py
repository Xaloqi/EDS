# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
"""
[EDS#636] DID 0xF186 ActiveDiagnosticSession must report the active session.

A `source: stub` DID gets a generated handler that returns a fixed backing
store, so F186 declared as a stub always read 0x00 (the safeboot examples
until EDS#636, while their READMEs said 0x01). Every example that declares
F186 must take it from the application (`source: app`), and the application
handler must read the session from the server.

Static check, not a runtime one: the safeboot examples have no native_sim
target. The stack serving F186 itself waits for the identification-DID
decision (Annex C check).
"""

import re
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parent.parent
EXAMPLES = sorted(p.parent for p in (ROOT / "examples").glob("*/diagnostics_config.yaml"))


def _f186(example: Path):
    cfg = yaml.safe_load((example / "diagnostics_config.yaml").read_text())
    for did in cfg.get("dids") or []:
        if str(did.get("id", "")).upper() == "0XF186":
            return did
    return None


DECLARING = [e for e in EXAMPLES if _f186(e) is not None]


def test_some_example_declares_f186():
    assert DECLARING, "no example declares 0xF186; drop this test or update it"


@pytest.mark.parametrize("example", DECLARING, ids=lambda p: p.name)
def test_f186_is_application_sourced(example):
    did = _f186(example)
    assert did.get("source", "stub") == "app", (
        f"{example.name}: 0xF186 is a generated stub, which always reads its "
        f"backing store (0x00), not the session (EDS#636)")


@pytest.mark.parametrize("example", DECLARING, ids=lambda p: p.name)
def test_f186_handler_reads_the_session(example):
    sources = "\n".join(p.read_text() for p in (example / "src").glob("*.c"))
    m = re.search(r"uds_status_t\s+did_read_activediagnosticsession\s*\([^)]*\)\s*\{(.*?)\n\}",
                  sources, re.S)
    assert m, f"{example.name}: no did_read_activediagnosticsession() in src/"
    assert "uds_session_get_active" in m.group(1), (
        f"{example.name}: the F186 handler does not read the active session")
