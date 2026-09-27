"""
tests/test_install_md_ci_recipe.py — INSTALL.md's "CI integration example"
must not silently abort itself on the exact build it exists to report on.

`tools/testlab.py report` (EDS-toolchain) deliberately exits 1 whenever any
run in its results failed — it can double as a simple CI gate on its own.
Chaining "Upload report" and a regression-gate `compare` step after it in a
GitHub Actions job, with no `continue-on-error`/`if: always()`, means a
single red run aborts the job at the report step: no artifact upload, no
regression gate. Confirmed by literally running the documented sequence as
a shell script and watching it stop after the report step (2026-09-27
campaign).

Run:
    XALOQI_LICENSE_SKIP=1 pytest tests/test_install_md_ci_recipe.py -v
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parent.parent
_INSTALL_MD = _REPO_ROOT / "INSTALL.md"


@pytest.mark.skipif(not _INSTALL_MD.is_file(), reason="INSTALL.md not found")
def test_ci_recipe_report_step_tolerates_a_failing_run():
    text = _INSTALL_MD.read_text(encoding="utf-8")
    m = re.search(
        r"### CI integration example.*?```yaml\n(.*?)\n```",
        text, re.S,
    )
    assert m, "CI integration example YAML block not found in INSTALL.md"
    steps = yaml.safe_load(m.group(1))
    assert isinstance(steps, list) and steps, "expected a list of GitHub Actions steps"

    by_name = {s.get("name"): s for s in steps if isinstance(s, dict)}

    report_step = by_name.get("Generate TestLab AI report")
    assert report_step is not None, "expected a 'Generate TestLab AI report' step"
    assert report_step.get("continue-on-error") is True, (
        "'Generate TestLab AI report' must set continue-on-error: true — "
        "tools/testlab.py report exits 1 whenever any run in the results "
        "failed (by design), so without this the job aborts here on every "
        "red build, before the report is uploaded or the regression gate "
        "below ever runs."
    )

    for name in ("Upload report", "Fail on regressions (compare with last baseline)"):
        step = by_name.get(name)
        assert step is not None, f"expected a {name!r} step"
        assert step.get("if") == "always()", (
            f"{name!r} must set if: always() — otherwise it's skipped "
            "whenever the (continue-on-error) report step above reported "
            "a failure, which is exactly the run you most need it to run on."
        )
