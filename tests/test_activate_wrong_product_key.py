"""
tests/test_activate_wrong_product_key.py — a TestLab key in EDS activate says so.

A Xaloqi TestLab key is signed with TestLab's key pair, so EDS verification
fails on the signature, and activate.py used to report "invalid signature ...
tampered with" (2026-11-13 validation campaign). Bundle buyers get two keys;
activate.py now says which product the key is for and where it goes. The claim
is read unverified and only words the error: the key is still refused.

Runs without the commercial tools/_license.py: activate.py is copied next to a
stub whose verification always fails the way the real one does on a foreign
signature.

Run:  pytest tests/test_activate_wrong_product_key.py -v
"""

from __future__ import annotations

import base64
import json
import shutil
import subprocess
import sys
from pathlib import Path

_ACTIVATE = Path(__file__).resolve().parent.parent / "tools" / "activate.py"

_STUB_LICENSE = '''
from pathlib import Path
PURCHASE_URL = "https://xaloqi.com"
LICENSE_ENV_VAR = "XALOQI_LICENSE_KEY"
LICENSE_FILE_PATH = Path(__file__).parent / "license.key"
GRACE_PERIOD_DAYS = 14
def _verify_jwt(token):
    raise ValueError("License key has an invalid signature. "
                     "The key may have been tampered with or is not a genuine Xaloqi key.")
'''


def _jwt(claims: dict) -> str:
    enc = lambda d: base64.urlsafe_b64encode(json.dumps(d).encode()).rstrip(b"=").decode()
    return f"{enc({'alg': 'EdDSA', 'typ': 'JWT'})}.{enc(claims)}.c2lnbmF0dXJl"


def _activate(tmp_path: Path, key: str) -> subprocess.CompletedProcess:
    tools = tmp_path / "tools"
    tools.mkdir(exist_ok=True)
    shutil.copy(_ACTIVATE, tools / "activate.py")
    (tools / "_license.py").write_text(_STUB_LICENSE, encoding="utf-8")
    return subprocess.run([sys.executable, str(tools / "activate.py"), "--key", key],
                          capture_output=True, text=True, timeout=60)


def test_testlab_key_is_named_and_refused(tmp_path: Path) -> None:
    r = _activate(tmp_path, _jwt({"product": "xaloqi-testlab", "tier": "pro"}))
    assert r.returncode == 1
    assert "This is a Xaloqi TestLab licence key, not an EDS key." in r.stderr
    assert "testlab-activate --key" in r.stderr
    assert "two keys" in r.stderr
    assert "tampered" not in r.stderr
    assert not (tmp_path / "tools" / "license.key").exists()


def test_other_bad_keys_keep_the_signature_message(tmp_path: Path) -> None:
    for key in (_jwt({"product": "xaloqi-eds"}), _jwt({"product": 7}), "a.%%%.c", "garbage"):
        r = _activate(tmp_path, key)
        assert r.returncode == 1
        assert "invalid signature" in r.stderr, (key, r.stderr)
        assert "TestLab" not in r.stderr
