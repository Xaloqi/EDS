"""wwh_obd.functional_group: validation and template context (EDS#602).

ReadDTCInformation sub-function 0x55 answers for one ECU-wide
FunctionalGroupIdentifier, declared as `wwh_obd: {functional_group: "0x33"}`.
codegen and config_parser accept "0x00".."0xFE" and nothing else; codegen hands
the templates an int (uds_init.c Step 5, dtc_config.h for the harness), or None
when the block is absent. The C side is tests/unit_runnable/test_service_0x19.c.

Run:  pytest tests/test_codegen_wwh_obd.py -v
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
_CODEGEN = _REPO_ROOT / "tools" / "codegen.py"
_TEMPLATES = _REPO_ROOT / "tools" / "templates"
sys.path.insert(0, str(_REPO_ROOT / "tools"))
import codegen  # noqa: E402
import config_parser  # noqa: E402

_BASIC = _REPO_ROOT / "examples" / "basic_ecu" / "diagnostics_config.yaml"


def _cfg(**wwh) -> dict:
    cfg = yaml.safe_load(_BASIC.read_text())
    if wwh:
        cfg["wwh_obd"] = wwh
    return cfg


@pytest.mark.parametrize("group,value", [("0x33", 0x33), ("0x00", 0x00), ("0xfe", 0xFE)])
def test_valid_group_reaches_the_templates(group, value) -> None:
    cfg = _cfg(functional_group=group)
    codegen.validate_config(cfg)
    assert config_parser.validate_config(cfg) == []
    assert codegen.build_uds_init_context(cfg)["wwh_obd_functional_group"] == value
    assert codegen.build_dtc_config_context(cfg)["wwh_obd_functional_group"] == value


def test_no_block_means_no_group() -> None:
    cfg = _cfg()
    assert codegen.build_uds_init_context(cfg)["wwh_obd_functional_group"] is None


@pytest.mark.parametrize("wwh", [
    {"functional_group": "0xFF"}, {"functional_group": "0x123"}, {"functional_group": "33"},
    {"functional_group": 51}, {}, {"functional_group": "0x33", "groups": []},
])
def test_invalid_group_is_refused(wwh, capsys) -> None:
    cfg = yaml.safe_load(_BASIC.read_text())
    cfg["wwh_obd"] = wwh
    with pytest.raises(SystemExit):
        codegen.validate_config(cfg)
    assert "wwh_obd" in capsys.readouterr().err
    assert any("wwh_obd" in e for e in config_parser.validate_config(cfg))


@pytest.mark.skipif(not (_TEMPLATES / "uds_init.c.j2").is_file(),
                    reason="needs the toolchain templates in tools/templates")
def test_uds_init_sets_the_group_only_when_declared(tmp_path: Path) -> None:
    def gen(cfg: dict, out: Path) -> str:
        yml = out.with_suffix(".yaml")
        yml.write_text(yaml.safe_dump(cfg, sort_keys=False))
        r = subprocess.run([sys.executable, str(_CODEGEN), "--config", str(yml), "--out", str(out),
                            "--template-dir", str(_TEMPLATES), "--safety-wrappers",
                            "--asil-level", "B", "--no-manifest"],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        return (out / "uds_init.c").read_text()

    assert "dtc_database_set_functional_group((uint8_t)0x33U)" in gen(
        _cfg(functional_group="0x33"), tmp_path / "with")
    assert "dtc_database_set_functional_group" not in gen(_cfg(), tmp_path / "without")
