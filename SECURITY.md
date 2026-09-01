# Security Policy

## Supported versions

| Version | Security fixes |
|---|---|
| 1.13.x (current) | ✅ Yes |
| 1.11.x | ✅ Yes (critical only) |
| < 1.13 | ❌ No — please upgrade |

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

The key *response* sent back to the ECU is truncated to 4 bytes (the seed stays the full
8 bytes). This is conventional for UDS SecurityAccess — ISO 14229-1 does not mandate a
key length, and 2–4 byte responses are standard in production stacks. Two attacker
models follow from this, with different actual strengths, and they should not be
conflated:

- *Guessing a valid response online* (sending seed/key attempts to the ECU over the bus)
  is bounded by the 4-byte truncation — a 32-bit search space, not 128-bit. What makes
  this impractical is not AES-128's hardness but attempt-limiting: `UDS_SECURITY_MAX_ATTEMPTS`
  (3) consecutive failed attempts trigger a lockout whose state is persisted to NVM, so
  it survives a power cycle.
- *Deriving the level key* from captured `(seed, key)` pairs without brute-forcing online
  is a separate, much harder problem — this is what the AES-128-CMAC construction
  actually protects against, since CMAC is a secure PRF under the standard AES-128
  hardness assumption.

**Placeholder keys:** The repository ships with placeholder AES-128 keys
(`0x00..0x0F` / `0x10..0x1F`). Two independent gates now reliably stop accidental
deployment of placeholder keys, both derived from a single shared build-mode primitive,
`EDS_BUILD_IS_PRODUCTION` (`core/uds_security_algo.h`, `[SEC-BUILD-MODE-01]`, issue #84):

- **Compile-time gate** (`[SEC-KEY-GATE-01]` / CRIT-4, `core/uds_security_algo.c`):
  a production build (`EDS_BUILD_IS_PRODUCTION == 1`) now **actually refuses to
  compile** — an unconditional `#error` — while `core/uds_security_algo.c` still
  contains the compile-time placeholder key data. This is not conditional on
  detecting the specific placeholder byte pattern; it forces the integrator to
  affirmatively prove intent (inject real keys via `uds_security_algo_set_level_key()`,
  or edit the key array in a secure build) before a production flag is honoured.
- **Runtime guard** in the generated init sequence (Step 7.1): refuses to start
  (`UDS_STATUS_ERR_CONDITIONS_NOT_MET`) if `uds_security_algo_keys_are_placeholder()`
  still reports placeholder keys in a production build.

**Zephyr**: set `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n` in your production `prj.conf` to
declare a production build (unchanged from before this fix — what changed is that this
now actually works: previously, Zephyr's generated `autoconf.h` OMITS the Kconfig
symbol entirely for `n` rather than emitting `0`, which meant both gates silently did
nothing in exactly this configuration — see issue #84).

**FreeRTOS** (new, issue #84): FreeRTOS has no Kconfig of its own. Every FreeRTOS
example's `CMakeLists.txt` now exposes `-DEDS_PLACEHOLDER_KEYS_ONLY=<ON|OFF>` (default
`ON`, i.e. development). Set `-DEDS_PLACEHOLDER_KEYS_ONLY=OFF` to declare a production
FreeRTOS build. Before this fix, no FreeRTOS build — development or production — ever
defined `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY` at all, so both gates were silently inert
on every FreeRTOS build, including CI. See `docs/SECURITY_NOTICE.md` for entropy
requirements. The full OEM key injection procedure is included with the Professional
tier.

**TRNG:** Seed generation quality depends on the platform's hardware entropy source.
Production deployments must supply a TRNG-backed callback via
`uds_security_algo_set_rng_cb()`. Behaviour on loss of entropy is now **fail-closed
in production, and unchanged in development/CI** — see `[SEC-TRNG-FAILCLOSED-01]` in
`core/uds_security_algo.c`. `ALGO_ENTROPY_FAIL_CLOSED` is a plain alias for
`EDS_BUILD_IS_PRODUCTION` (`[SEC-BUILD-MODE-01]`, issue #84) — the same shared
build-mode primitive the placeholder-key gates above now use, so this and the
placeholder-key protection can no longer drift apart the way they did before #84:

- **Development/CI builds** (Zephyr with `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=y`,
  FreeRTOS with `-DEDS_PLACEHOLDER_KEYS_ONLY=ON` — the default on both, or a host
  unit-test build): if no TRNG is registered, or a registered TRNG callback fails at
  runtime, the stack falls back to a 16-bit software LFSR and logs a fault count. This
  is unchanged legacy behaviour, intentional for CI and simulator builds only.
- **Production builds** (Zephyr with `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n`, or
  FreeRTOS with `-DEDS_PLACEHOLDER_KEYS_ONLY=OFF`): the LFSR is never used to satisfy a
  seed request. If no TRNG is registered, or the registered TRNG callback fails,
  `SecurityAccess` seed generation is refused (`UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE`,
  surfaced to the tester as NRC 0x24) instead of silently degrading to a predictable
  seed. The fault is recorded in the `uds_safety` platform-violations counter either
  way; the two cases are distinguished by `last_violation_code`
  (`UDS_STATUS_ERR_PLATFORM` for the dev-mode soft fallback vs.
  `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE` for the production hard refusal).

**Retroactive fix (issue #84):** before this fix, no FreeRTOS build ever defined
`CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY`, so `ALGO_ENTROPY_FAIL_CLOSED` always fell through
to its own fail-closed default on FreeRTOS — a real FreeRTOS deployment with no TRNG
registered has always gotten the production hard refusal, with **no supported way to
opt into the development LFSR fallback**. This was a latent, CI-invisible gap in the
already-merged TRNG fail-closed fix (PR #85): the `freertos-qemu` CI job is build-only
(compiles, links, checks the ELF exists) and never exercises a live SecurityAccess
request, so the gap never surfaced there. `-DEDS_PLACEHOLDER_KEYS_ONLY=ON` (the new
FreeRTOS CMake default) now gives FreeRTOS the same supported development opt-in
Zephyr's Kconfig always had.

**ASIL-B DID access chain:** The 5-step validation chain is enforced at code generation
time. It cannot be disabled by runtime configuration. Any mechanism that allows DID
access to bypass one or more steps is considered a critical vulnerability.

---

## Acknowledgements

*This section will be updated as reports are received and resolved.*
