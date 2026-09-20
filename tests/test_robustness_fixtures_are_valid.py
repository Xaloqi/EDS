"""
tests/test_robustness_fixtures_are_valid.py — the robustness fixtures must
stay acceptable to codegen's own validation gates.

WHY THIS EXISTS (EDS#295)
-------------------------
`examples/basic_ecu/generated/tests/test_robustness_*.py` build YAML configs
inline and feed them to `tools/codegen.py`. They gate themselves on
`tools/templates/` being present — the commercial deliverable, **gitignored
and absent from a public checkout, present in every Developer/Professional
install**. CI checks out the public repo, so the codegen half never runs:

    CI (public checkout):          292 passed, 147 skipped   -> green
    Licensed install (templates):  21 FAILED

Every one of those 21 failed for a single reason: the fixtures declared no
`can:` section, and codegen has rejected that since the **#226 ASIL CAN-ID
gate** shipped in v1.13.4. Three releases of a suite that a paying customer
is invited to run, red on their machine and green on ours.

The quieter half was worse. Sixteen `assert rc != 0` negative tests in
`test_robustness_F_codegen_limits.py` were **false passes**: the CAN gate
rejected the config before the condition under test — `data_length: 4096`,
a duplicate DID id — was ever evaluated. Proven by giving the old fixture a
perfectly valid `data_length: 17` and watching codegen still exit 1. Those
assertions proved nothing about the limits they name (run-014).

WHAT THIS GUARDS
----------------
Every robustness fixture declares explicit CAN ids — uniformly, including in
the phases that do not invoke ASIL codegen today (K, L). A rule scoped to
"the files that currently use --safety-wrappers" would need a file list, and
that list would rot the moment someone adds an ASIL test to another phase;
a uniform rule needs no exemptions and cannot go stale.

It deliberately does **not** assert that every fixture passes
`validate_config()` outright. A first version did, and it was wrong: phase F
exists to prove codegen *rejects* `data_length: 4096`, a duplicate DID id and
so on, so those fixtures are invalid by design. A check that flags them would
be muted within a week, and a muted check is worth nothing.

This runs in a plain public checkout, because it reads the fixture text
rather than executing codegen — so CI sees it even though CI cannot see the
tests it protects. That asymmetry is the whole point.

Run:  XALOQI_LICENSE_SKIP=1 pytest tests/test_robustness_fixtures_are_valid.py -v
"""

from __future__ import annotations

import re
import textwrap
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
_ROBUSTNESS_DIR = _REPO_ROOT / "examples" / "basic_ecu" / "generated" / "tests"

#: Fixtures are written as textwrap.dedent("""\ ... """) blocks.
_FIXTURE_RE = re.compile(r'textwrap\.dedent\(\s*"""\\\n(.*?)"""\s*\)', re.S)

def _robustness_files() -> list[Path]:
    files = sorted(_ROBUSTNESS_DIR.glob("test_robustness_*.py"))
    assert files, f"no robustness files found under {_ROBUSTNESS_DIR}"
    return files


def _fixtures(path: Path) -> list[tuple[int, dict]]:
    """Every parseable YAML fixture in one file, with its line number."""
    text = path.read_text(encoding="utf-8")
    out: list[tuple[int, dict]] = []
    for m in _FIXTURE_RE.finditer(text):
        body = textwrap.dedent(m.group(1))
        if "schema_version" not in body:
            continue                      # not a diagnostics config
        try:
            doc = yaml.safe_load(body)
        except yaml.YAMLError:
            continue                      # deliberately malformed YAML fixture
        if isinstance(doc, dict):
            out.append((text[: m.start()].count("\n") + 1, doc))
    return out


def test_fixtures_are_discovered() -> None:
    """If the fixture shape changes, this guard must fail loudly, not pass
    vacuously by matching nothing."""
    total = sum(len(_fixtures(f)) for f in _robustness_files())
    assert total >= 15, (
        f"only {total} robustness fixtures parsed — the extraction regex has "
        "probably gone stale, and this guard would silently protect nothing"
    )


@pytest.mark.parametrize(
    "path", _robustness_files(), ids=lambda p: p.name
)
def test_every_fixture_declares_explicit_can_ids(path: Path) -> None:
    """#226: an ASIL build must declare CAN addressing explicitly.

    Without this, codegen exits 1 before reaching whatever the test is
    actually about — which turns positive tests red and negative tests into
    assertions that prove nothing.
    """
    problems = []
    for lineno, doc in _fixtures(path):
        name = doc.get("metadata", {}).get("ecu_name", "?")
        can = doc.get("can")
        if not isinstance(can, dict):
            problems.append(f"{path.name}:{lineno} ({name}): no can: section")
            continue
        for key in ("rx_can_id", "tx_can_id"):
            if not can.get(key):
                problems.append(f"{path.name}:{lineno} ({name}): can.{key} missing")

    assert not problems, "fixtures codegen will reject under ASIL-B:\n  " + \
        "\n  ".join(problems)
