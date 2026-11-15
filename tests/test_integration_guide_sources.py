"""EDS#617: the INTEGRATION_GUIDE source list is what an integrator copies.

Every ${EDS_ROOT}/....c the guide names must exist, and every source under
core/ and core/uds_services/ must be named, either in the guide or in
cmake/eds_service_sources.cmake when the guide includes that file. A missing
source is an undefined reference at link time.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GUIDE = ROOT / "docs" / "INTEGRATION_GUIDE.md"
SERVICE_LIST = ROOT / "cmake" / "eds_service_sources.cmake"

# Commented-out alternatives count too: the wrong flash-ops path was in a comment.
NAMED = re.compile(r"\$\{_?EDS_ROOT\}/([\w/]+\.c)\b")


def _step3_block() -> str:
    text = GUIDE.read_text(encoding="utf-8")
    start = text.index("### Step 3")
    end = text.index("### Step 4")
    return text[start:end]


def _named(text: str) -> set[str]:
    return {m.group(1) for m in NAMED.finditer(text)}


def test_every_named_source_exists():
    named = _named(_step3_block())
    assert named, "no ${EDS_ROOT}/....c paths found in Step 3"
    missing = sorted(p for p in named if not (ROOT / p).is_file())
    assert not missing, f"INTEGRATION_GUIDE Step 3 names files that do not exist: {missing}"


def test_every_core_source_is_named():
    block = _step3_block()
    named = _named(block)
    if "cmake/eds_service_sources.cmake" in block:
        named |= _named(SERVICE_LIST.read_text(encoding="utf-8"))
    present = {p.relative_to(ROOT).as_posix()
               for d in ("core", "core/uds_services") for p in (ROOT / d).glob("*.c")}
    missing = sorted(present - named)
    assert not missing, f"INTEGRATION_GUIDE Step 3 does not name: {missing}"
