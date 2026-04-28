# Security Policy

## Supported versions

| Version | Security fixes |
|---|---|
| 1.3.x (current) | ✅ Yes |
| 1.2.x | ✅ Yes (critical only) |
| < 1.2 | ❌ No — please upgrade |

Only the current release branch receives routine security fixes. Pre-release and
modified versions are not supported. Update to the latest tagged release before
reporting.

---

## Reporting a vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

EDS is an automotive diagnostics stack intended for deployment in ECU firmware.
A vulnerability in the UDS security access implementation, ISO-TP transport layer,
or ASIL-B safety wrappers could affect vehicle systems. Responsible disclosure
matters here more than in most software projects.

### How to report

Email **contact@xaloqi.com** with:

- Which component is affected — e.g. `core/uds_security.c`, `transport/isotp.c`,
  `tools/codegen.py`, or a specific generated template
- A description of the vulnerability and what an attacker could achieve
- Steps to reproduce, or a minimal proof-of-concept if you have one
- Your CVSS v3 severity estimate if possible
- Whether you have a proposed fix or patch

If the report is sensitive, use the
[GitHub Security Advisory](https://github.com/Xaloqi/EDS/security/advisories/new)
to keep the report private — this is the preferred channel for sensitive reports.

### Response timeline

| Action | Timeframe |
|---|---|
| Acknowledgement | Within 48 hours |
| Initial severity assessment | Within 5 business days |
| Fix or mitigation plan communicated to reporter | Within 15 business days |
| Patch released — critical or high severity | Within 30 days of confirmed report |
| Patch released — medium or low severity | Next scheduled release |
| Coordinated public disclosure | After patch ships, or 90 days from report |

We follow coordinated disclosure. We will notify you before any public statement
and credit you in the release notes unless you prefer to remain anonymous.

---

## Scope

**In scope:**

| Component | What to look for |
|---|---|
| `core/uds_security.c` | Seed/key state machine, lockout bypass, session escalation |
| `core/uds_security_algo.c` + `uds_aes_cmac.c` | AES-128-CMAC correctness, timing side-channels, key placeholder gate |
| `core/uds_safety.c` | 5-step ASIL-B chain bypass, safety self-test defeat |
| `transport/isotp.c` | Buffer overflow via malformed ISO-TP frames, state machine confusion |
| `tools/codegen.py` | Generated code that weakens security or safety invariants |
| Generated code in `generated/` | Defects traceable to a generator template |

**Out of scope:**

- Vulnerabilities in Zephyr RTOS itself → report to the [Zephyr security team](https://www.zephyrproject.org/security/)
- Vulnerabilities in third-party dependencies → report to the respective upstream project
- Issues requiring physical ECU access with no remote exploit path
- Theoretical attacks with no practical reproduce path against a default configuration

---

## Security architecture notes

**AES-128-CMAC:** SecurityAccess (UDS 0x27) uses AES-128-CMAC per RFC 4493 with an
8-byte seed embedding a TRNG nonce and a per-session sequence counter. The implementation
in `core/uds_aes_cmac.c` is table-free and cache-timing resistant. Key material is
scrubbed from stack memory after use.

**Placeholder keys:** The repository ships with placeholder AES-128 keys
(`0x00..0x0F` / `0x10..0x1F`). A compile-time gate (`CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY`)
and a runtime guard in the generated init sequence prevent accidental deployment of
placeholder keys. See `docs/SECURITY_NOTICE.md` for entropy requirements. The full OEM key injection procedure is included with the Professional tier.

**TRNG:** Seed generation quality depends on the platform's hardware entropy source.
Production deployments must supply a TRNG-backed callback via
`uds_security_algo_set_rng_cb()`. Without a registered TRNG, the stack falls back to
a 16-bit LFSR and logs a fault count — this is intentional for CI and simulator builds,
not for field firmware.

**ASIL-B DID access chain:** The 5-step validation chain is enforced at code generation
time. It cannot be disabled by runtime configuration. Any mechanism that allows DID
access to bypass one or more steps is considered a critical vulnerability.

---

## Acknowledgements

*This section will be updated as reports are received and resolved.*
