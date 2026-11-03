"""
tests/test_example_readmes_free_to_build.py — no example README may say a
licence is needed to build or run it.

EDS#528 fixed robot_joint_controller_ecu's Availability section; EDS#569 found
the same "Included with Developer and Professional licenses" / "Purchase at
xaloqi.com" claim in six more READMEs (ardep_ecu, bms_ecu, motor_controller_ecu,
safeboot_ecu, sensor_ecu, sensor_ecu_freertos). Every example is on main with
its YAML, src/ and committed generated/ and is free to build, run and study
(Apache-2.0 example, GPL v2 runtime); the root README's "Open-core" table says
so. A licence is needed only to REGENERATE from a changed YAML (codegen
templates) or to ship the runtime in proprietary firmware.

Two checks over every examples/*/README.md (enumerated, not sampled):

* no purchase / delivery-ZIP phrasing at all;
* any sentence that names a Developer/Professional licence must say what it
  is for (regenerating, templates, proprietary firmware, ...), not that it is
  needed to use the example.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_EXAMPLES = Path(__file__).resolve().parent.parent / "examples"
_READMES = sorted(_EXAMPLES.glob("*/README.md"))

_PURCHASE_CLAIMS = re.compile(
    r"purchase at|after purchase|delivery zip|your delivery|"
    r"(included|delivered|provided)\s+with\s+(the\s+)?(\*\*)?(developer|professional|licen)",
    re.IGNORECASE,
)

_LICENCE_MENTION = re.compile(
    r"(developer|professional)(\*\*)?\s+(and\s+(\*\*)?(developer|professional)(\*\*)?\s+)?"
    r"(licen[cs]e|tier|licensing)",
    re.IGNORECASE,
)
_LICENCE_PURPOSE = re.compile(
    r"regenerat|codegen|template|proprietary|commercial licen|state machine|"
    r"testgen|tools/|rollback|^[-# ]*## Developer tier",
    re.IGNORECASE,
)


def _sentences(text: str) -> list[str]:
    text = re.sub(r"```.*?```", " ", text, flags=re.DOTALL)
    flat = re.sub(r"\s+", " ", text)
    return re.split(r"(?<=[.!?:])\s+(?=[A-Z*\[`-])|\s\|\s|\s-\s(?=\*\*|`)", flat)


def test_examples_are_enumerated() -> None:
    assert len(_READMES) >= 14, "examples/*/README.md glob found too few READMEs"


@pytest.mark.parametrize("readme", _READMES, ids=lambda p: p.parent.name)
def test_no_purchase_claim(readme: Path) -> None:
    hits = _PURCHASE_CLAIMS.findall(readme.read_text(encoding="utf-8"))
    assert not hits, (
        f"{readme.relative_to(_EXAMPLES.parent)} says the example must be "
        "bought/delivered; it is free to build and run (EDS#569). Use the "
        "Availability wording from examples/robot_joint_controller_ecu/README.md."
    )


@pytest.mark.parametrize("readme", _READMES, ids=lambda p: p.parent.name)
def test_licence_mentions_state_their_purpose(readme: Path) -> None:
    bad = [
        s
        for s in _sentences(readme.read_text(encoding="utf-8"))
        if _LICENCE_MENTION.search(s) and not _LICENCE_PURPOSE.search(s)
    ]
    assert not bad, (
        f"{readme.relative_to(_EXAMPLES.parent)}: licence mentioned without "
        f"saying it is only for regenerating / proprietary shipping: {bad}"
    )
