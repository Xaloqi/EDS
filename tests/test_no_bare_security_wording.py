"""
tests/test_no_bare_security_wording.py — customer-visible text must not present
EDS as "secure", "hardened", "tamper-proof", "certified", "21434-compliant" or
"R155 compliant" (wording rule decided 2026-11-28, scoping note #435 §7; same shape as
the ASIL rule in tests/test_no_bare_asil_wording.py, and the toolchain's
tests/test_no_bare_security_wording.py enforces it on its side).

Say what was checked or what the code does ("implements SID 0x29 certificate exchange",
"refuses a lower image version (a software check)"), never what property EDS has.
"SecurityAccess", "security level", "Security boundary" are different words and stay.

A line that must keep a flagged word goes in ALLOWED_LINES with the reason beside it:
a term of art, a negation ("not a tamper-proof counter"), a product name, or an
instruction about the integrator's own system.

Not covered on purpose: the safety documents (docs/Safety_Model.md and the EDS-Safety
package have their own review) and C source comments under core/, transport/, config/,
platform/ (tracked separately), as for the ASIL rule.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCOPED = (
    "tools/codegen.py",
    "README.md",
    "INSTALL.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "docs/*.md",
    "examples/**/*",
)
EXCLUDED = ("docs/Safety_Model.md",)

# Case-insensitive.
FORBIDDEN = {
    "'secure' / 'secure boot' / 'secure OTA' presented as a property": r"\bsecure\b",
    "'hardened'": r"\bhardened\b",
    "'tamper-proof'": r"\btamper[- ]?proof\b",
    "claims ISO/SAE 21434 compliance":
        r"21434[- ]compliant|compl(?:iant|ies|iance)\s+(?:with|to)\s+(?:ISO[/ -]*(?:SAE\s+)?)?21434",
    "claims UN R155 compliance or readiness":
        r"\bR\s?155[- ](?:compliant|ready)|compl\w*\s+(?:with|to)\s+(?:UN(?:ECE)?\s+)?(?:WP\.29\s+)?R\s?155",
    "'certified'": r"\bcertified\b",
    "'production-grade security'": r"production[- ]grade (?:cyber)?security",
}
# Lines that legitimately contain a flagged word. Exact substrings, each with its reason.
ALLOWED_LINES = (
    "from secure boot",                  # generated uds_init.c: "call this from your secure boot stage", the integrator's own code
    "keys loaded from secure storage",   # canfd_ecu / robot_joint_controller_ecu main.c: where the integrator keeps keys
    "NOT SECURE.",                       # safeboot_ecu main.c: warns that the placeholder keys are not secure
    "CMAC is a secure PRF",              # SECURITY.md: cryptographic term of art
    "not cryptographically secure",      # SECURITY_NOTICE.md: why rand() is no seed source
    "a complete secure-update",          # INTEGRATION_GUIDE.md, ARCHITECTURE.md: what SafeBoot is NOT
    "a full secure-update platform",     # AI_CONTEXT.md: what SafeBoot is NOT
    "Secure Boot Assist Firmware",       # NXP's name for the S32K344 boot firmware
    "compatible-certified for",          # vehicle_state_ecu README: disclaimer, EDS is not AGL-certified
    "or certified ECU functionality",    # README.md: "not complete or certified ECU functionality", a disclaimer
    "before claiming certified ASIL-B",  # INSTALL.md disclaimer, as in the ASIL guard
    "not a tamper-proof counter",        # safeboot_ecu README: a residual limit of the software check
    "A tamper-proof",                    # safeboot_ecu README: the hardware counter MCUboot offers, which this SoC lacks
)


def _scoped_files():
    seen = set()
    for pattern in SCOPED:
        for p in ROOT.glob(pattern):
            rel = p.relative_to(ROOT).as_posix()
            if (p.is_file() and rel not in EXCLUDED and p.suffix not in {".png", ".ico", ".svg", ".bin", ".elf"}
                    and p not in seen):
                seen.add(p)
                yield p


def _hits(text: str, name: str):
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if any(ok in line for ok in ALLOWED_LINES):
            continue
        for why, rx in FORBIDDEN.items():
            if re.search(rx, line, re.IGNORECASE):
                out.append(f"{name}:{n}: {why}: {line.strip()[:110]}")
    return out


def test_wording_rule_no_forbidden_security_phrases():
    hits = []
    for path in _scoped_files():
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        hits += _hits(text, path.relative_to(ROOT).as_posix())
    assert not hits, "Security wording rule violated (see module docstring):\n" + "\n".join(hits)


def test_canary_planted_strings_are_caught():
    """The rule must bite: each planted claim is a hit, the allowed vocabulary is not."""
    for planted in ("EDS is a secure ECU stack.", "Secure boot for every ECU", "a hardened build",
                    "Tamper-Proof counter", "21434-compliant", "compliant with ISO/SAE 21434",
                    "R155 compliant", "R155-ready", "certified by us", "production-grade security"):
        assert _hits(planted, "canary"), planted
    for fine in ("SecurityAccess (0x27) with a security level", "Security boundary", "security_access"):
        assert not _hits(fine, "canary"), fine
