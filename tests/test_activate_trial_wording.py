"""
tests/test_activate_trial_wording.py — activating a trial key must not say
"renewal recommended" (EDS-toolchain#270, 2026-11-13 validation campaign F10).

A trial is the start of a subscription: if it continues, the first payment
brings a full key by email, and nothing is renewed. activate.py printed
"(29 days remaining — renewal recommended)" when a 30-day trial key was
activated on day 1, and "Renew at …" for an expired one.

Runs activate.py against a stub _license (the real one is a commercial
deliverable, absent from this repo), so it runs in public CI.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

_ACTIVATE = Path(__file__).resolve().parent.parent / "tools" / "activate.py"


def _activate(tmp_path: Path, tier: str, days: float) -> str:
    tools = tmp_path / "tools"
    tools.mkdir()
    shutil.copy2(_ACTIVATE, tools / "activate.py")
    exp = int(time.time() + days * 86400)
    (tools / "_license.py").write_text(textwrap.dedent(f"""
        from pathlib import Path
        GRACE_PERIOD_DAYS = 14
        PURCHASE_URL = "https://example.com/buy"
        LICENSE_ENV_VAR = "XALOQI_LICENSE_KEY"
        LICENSE_FILE_PATH = Path({str(tmp_path / "home" / "license.key")!r})
        def _verify_jwt(key):
            return {{"tier": {tier!r}, "sub": "t@example.com", "exp": {exp}}}
    """), encoding="utf-8")
    r = subprocess.run([sys.executable, str(tools / "activate.py"), "--key", "k"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return r.stdout


@pytest.mark.parametrize("days", [29.5, 6.5])
def test_trial_activation_does_not_recommend_renewal(tmp_path, days):
    out = _activate(tmp_path, "trial", days)
    assert "renew" not in out.lower(), out
    assert "trial" in out.lower(), out


def test_expired_trial_activation_does_not_say_renew(tmp_path):
    out = _activate(tmp_path, "trial", -20.5)
    assert "renew" not in out.lower(), out
    assert "https://example.com/buy" in out, out


@pytest.mark.parametrize("days,word", [(20.5, "renewal recommended"), (-20.5, "Renew at")])
def test_developer_activation_keeps_renewal_wording(tmp_path, days, word):
    assert word in _activate(tmp_path, "developer", days)
