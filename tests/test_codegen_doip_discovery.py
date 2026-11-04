"""ecu.doip.discovery: validation and template context (EDS#392).

Vehicle discovery is on by default for DoIP ECUs. codegen validates the
optional ecu.doip.discovery block and hands the templates a doip_discovery
context: None for a CAN-only ECU, otherwise enabled/announce, the VIN DID
(0xF190 by default, when it is readable and 17 bytes), EID/GID overrides and
the further-action byte.
"""
from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"
sys.path.insert(0, str(_REPO_ROOT / "tools"))

_BASE = textwrap.dedent(
    """\
    schema_version: 1
    metadata:
      ecu_name: "DiscoveryECU"
      version: "1.0.0"
    timing:
      p2_server_max_ms: 25
      p2_star_server_max_ms: 5000
      s3_server_timeout_ms: 5000
    ecu:
      transport: {transport}
      doip:
        logical_address: "0xE400"
    {discovery}
    dids:
      - id: "0xF190"
        name: "Vehicle Identification Number"
        data_length: {vin_len}
        access: ["read"]
        min_session: default
        read_security_level: 0
      - id: "0xF18C"
        name: "ECU Serial Number"
        data_length: 4
        access: ["read"]
        min_session: default
        read_security_level: 0
    """
)


def _cfg(transport: str = "doip", discovery: str = "", vin_len: int = 17) -> dict:
    disc = textwrap.indent(discovery, "    ") if discovery else ""
    return yaml.safe_load(_BASE.format(transport=transport, discovery=disc, vin_len=vin_len))


def _codegen(tmp_path: Path, cfg_text: str) -> subprocess.CompletedProcess:
    cfg = tmp_path / "diagnostics_config.yaml"
    cfg.write_text(cfg_text, encoding="utf-8")
    env = dict(os.environ, XALOQI_LICENSE_SKIP="1")   # public checkout, see test_codegen_write_access_reachable
    return subprocess.run(
        [sys.executable, str(_CODEGEN), "--config", str(cfg),
         "--out", str(tmp_path / "out"), "--dry-run", "--no-manifest"],
        capture_output=True, text=True, cwd=str(_REPO_ROOT), timeout=120, env=env,
    )


def _ctx(cfg: dict):
    import codegen
    return codegen.build_uds_init_context(cfg)["doip_discovery"]


def test_can_only_ecu_has_no_discovery_context() -> None:
    assert _ctx(_cfg(transport="can")) is None


def test_defaults_are_on_with_f190_as_vin() -> None:
    ctx = _ctx(_cfg())
    assert ctx == {
        "enabled": True, "announce": True,
        "vin_c_name": "vehicle_identification_number", "vin_did_id": "0xF190",
        "eid": None, "gid": None, "further_action": 0,
    }


def test_both_transports_also_get_discovery() -> None:
    assert _ctx(_cfg(transport="both"))["enabled"] is True


def test_no_vin_when_f190_is_not_17_bytes_and_not_named() -> None:
    assert _ctx(_cfg(vin_len=4))["vin_c_name"] is None


def test_opt_outs_and_overrides() -> None:
    ctx = _ctx(_cfg(discovery=textwrap.dedent("""\
        discovery:
          enabled: false
          announce: false
          vin_did: null
          eid: "02:00:5e:00:53:31"
          gid: "0A0B0C0D0E0F"
          further_action: "0x10"
        """)))
    assert ctx["enabled"] is False and ctx["announce"] is False
    assert ctx["vin_c_name"] is None
    assert ctx["eid"] == [0x02, 0x00, 0x5E, 0x00, 0x53, 0x31]
    assert ctx["gid"] == [0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F]
    assert ctx["further_action"] == 0x10


@pytest.mark.parametrize("discovery, message", [
    ("discovery: [enabled]", "'ecu.doip.discovery' must be a mapping"),
    ("discovery:\n  enable: false", "unknown 'ecu.doip.discovery.enable'"),
    ("discovery:\n  announce: sometimes", "'ecu.doip.discovery.announce' must be true or false"),
    ("discovery:\n  vin_did: \"0xF18C\"", "must name a readable DID with data_length 17"),
    ("discovery:\n  vin_did: \"0x1234\"", "must name a readable DID with data_length 17"),
    ("discovery:\n  eid: \"02:00:5e\"", "'ecu.doip.discovery.eid' must be 6 bytes"),
    ("discovery:\n  further_action: 300", "'ecu.doip.discovery.further_action' must be a byte"),
])
def test_malformed_discovery_is_rejected(tmp_path: Path, discovery: str, message: str) -> None:
    text = _BASE.format(transport="doip", discovery=textwrap.indent(discovery, "    "), vin_len=17)
    r = _codegen(tmp_path, text)
    out = r.stdout + r.stderr
    assert r.returncode == 1, f"expected exit 1, got {r.returncode}\n{out}"
    assert message in out, out


def test_well_formed_discovery_passes_validation(tmp_path: Path) -> None:
    text = _BASE.format(transport="doip",
                        discovery=textwrap.indent("discovery:\n  announce: false", "    "),
                        vin_len=17)
    r = _codegen(tmp_path, text)
    out = r.stdout + r.stderr
    assert "ecu.doip.discovery" not in out, out
