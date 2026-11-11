"""EDS#583: the MISRA result is a statement about one cppcheck version.

The version CI asserts, the docs that quote it and the analysis stubs must
agree. Needs no cppcheck.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CI = (ROOT / ".github/workflows/ci.yml").read_text()

PINNED = re.search(r'\[ "\$\(cppcheck --version\)" = "Cppcheck ([0-9.]+)" \]', CI).group(1)

# Files that state the version next to the zero-open claim.
DOCS = ["docs/Safety_Model.md", "misra_analysis.py"]

RTOS_SOURCES = ["transport/doip/freertos_lwip.c", "transport/doip/zephyr_lwip.c"]
RTOS_HEADER = re.compile(r'^#include\s+[<"]((?:lwip|zephyr)/[^>"]+|FreeRTOS\.h|task\.h)[>"]', re.M)


def test_ci_builds_and_asserts_the_same_version():
    assert f"cppcheck-{PINNED}-" in CI
    assert f"refs/tags/{PINNED}.tar.gz" in CI
    assert re.search(r"[0-9a-f]{64}  /tmp/cppcheck\.tar\.gz", CI)


def test_docs_name_the_pinned_version():
    for rel in DOCS:
        text = (ROOT / rel).read_text()
        assert f"cppcheck {PINNED}" in text, f"{rel} does not name cppcheck {PINNED}"
        stale = re.findall(r"cppcheck (\d+\.\d+(?:\.\d+)?)\b", text)
        old = {v for v in stale if v != PINNED and not v.startswith("2.6")}
        assert not old, f"{rel} names cppcheck {sorted(old)}, CI pins {PINNED}"


def test_stubs_cover_every_rtos_header_the_bindings_include():
    missing = []
    for rel in RTOS_SOURCES:
        for hdr in RTOS_HEADER.findall((ROOT / rel).read_text()):
            if not (ROOT / "tools/misra_stubs" / hdr).is_file():
                missing.append(f"{rel}: {hdr}")
    assert not missing, "no analysis stub for: " + ", ".join(missing)
