# Security Notice — Seed Entropy Requirements (FreeRTOS / Bare-Metal Targets)

**Classification:** Required reading for all FreeRTOS integrators  
**Applies to:** Xaloqi EDS on FreeRTOS and any bare-metal target using `eds_platform_init()`  
**Does not apply to:** Zephyr targets using the built-in `uds_security_algo.c` TRNG integration

---

## Summary

The security of UDS SecurityAccess (SID 0x27) depends entirely on the quality of
the random seed produced by the `seed_generate_cb` you register with
`uds_security_cfg_t`. Xaloqi EDS provides the seed/key framework; **you are
responsible for the entropy source.** A weak or predictable seed breaks the entire
security access mechanism regardless of the strength of the AES-128-CMAC key
derivation algorithm.

---

## What the stack provides

`uds_security_request_seed()` calls your `seed_generate_cb` and then applies two
defences before returning the seed to the tester:

1. **All-zero rejection `[SEC-ENTROPY-01]`**: If your callback produces an all-zero
   seed (the most common symptom of an uninitialised RNG), the stack returns
   `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE` (NRC 0x24) and does not expose the seed to
   the tester. This prevents the most obvious misconfiguration from becoming a silent
   security hole.

2. **Constant-time key comparison `[P2-SEC-01]`**: Key verification uses a
   `volatile` accumulator loop to prevent timing side-channels. This is only
   meaningful if the seed itself is unpredictable.

These defences do not compensate for a structurally broken entropy source. A seed
of `{0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}` passes the zero check but
is still trivially predictable.

---

## Your responsibility

You must provide a `seed_generate_cb` backed by a **hardware random number
generator (HRNG / TRNG)**. The minimum requirements are:

### Minimum: FIPS 140-2 / FIPS 140-3 compliant RNG

Your RNG must meet or exceed the requirements of
[NIST SP 800-90A](https://csrc.nist.gov/publications/detail/sp/800-90a/rev-1/final)
(Recommendation for Random Number Generation Using Deterministic Random Bit
Generators). Specifically:

| Requirement | What it means for your implementation |
|---|---|
| **Hardware entropy source** | Use the MCU's on-chip TRNG peripheral (e.g. STM32 RNG, NXP TRNG, Renesas RSIP-E) — not a software PRNG, LFSR, or counter. |
| **Health tests** | Run the TRNG's built-in startup and continuous health tests. Many MCU TRNG peripherals perform these automatically; verify that failure detection is enabled. |
| **Seed freshness** | Each call to `seed_generate_cb` must produce a statistically independent output. Do not reuse seeds across sessions. |
| **Entropy estimate** | The output must have at least 64 bits of min-entropy for an 8-byte seed (`UDS_SECURITY_SEED_LEN = 8`). A 32-bit output XOR'd with a counter does not meet this requirement. |

### Concrete MCU examples

A copy-and-adapt reference implementing the STM32 example below against the
actual `uds_algo_rng_cb_t` contract (`uds_security_algo_set_rng_cb()`) lives
at [`platform/freertos/freertos_rng_example.c`](../platform/freertos/freertos_rng_example.c)
— it is not compiled by default; copy it into your application and adapt it
to your MCU, same as every shipped FreeRTOS example expects you to.

**STM32 (HAL):**
```c
static uds_status_t my_seed_generate(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len)
{
    (void)security_level;
    uint8_t i;

    /* RNG peripheral must be initialised before uds_generated_init(). */
    for (i = 0U; i < seed_buf_len; i += 4U) {
        uint32_t rnd;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &rnd) != HAL_OK) {
            return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
        }
        /* Copy up to 4 bytes per iteration. */
        uint8_t copy = (uint8_t)((seed_buf_len - i) < 4U ? (seed_buf_len - i) : 4U);
        (void)memcpy(&seed_buf[i], &rnd, copy);
    }

    *out_seed_len = seed_buf_len;
    return UDS_STATUS_OK;
}
```

**NXP (SDK TRNG):**
```c
static uds_status_t my_seed_generate(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len)
{
    (void)security_level;
    if (TRNG_GetRandomData(TRNG0, seed_buf, seed_buf_len) != kStatus_Success) {
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }
    *out_seed_len = seed_buf_len;
    return UDS_STATUS_OK;
}
```

---

## What to avoid

| Pattern | Why it is broken |
|---|---|
| `seed = (uint32_t)xTaskGetTickCount()` | Tick count is predictable and has low entropy after reset. An attacker who knows approximate system uptime can brute-force the seed space. |
| `seed = rand()` or `seed = srand(time(0))` | Software PRNGs are not cryptographically secure and produce predictable sequences. |
| Counter-based seed (e.g. `seed++` per request) | Fully predictable. An attacker who observes one seed response can predict all future seeds. |
| Constant seed (same value every boot) | Equivalent to no security. Always produces the same key from the same seed. |
| 32-bit TRNG XOR'd with counter | Only 32 bits of entropy for an 8-byte field. The high 32 bits are predictable. |
| Re-using the seed from the previous session | Seed must be freshly generated for each `requestSeed` call. |

---

## TRNG not available on your target?

Some low-cost MCUs do not have a hardware TRNG. In that case:

1. **Use a DRBG seeded from multiple physical sources.** Combine ADC noise,
   oscillator jitter, and a factory-provisioned device secret. This does not meet
   FIPS 140-2 Level 2 requirements but is significantly better than a counter.

2. **Consider whether security access is appropriate for your use case.** If the
   target has no entropy source, the seed/key exchange provides authentication
   theatre, not real protection. Consult your functional safety team before
   deploying.

3. **Do not ship placeholder keys.** The EDS repository includes `0x00..0x0F` /
   `0x10..0x1F` placeholder AES-128 keys in `uds_security_algo.c`. A compile-time
   gate (`CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY`) prevents deployment — do not bypass
   this gate.

---

## Relationship to ISO 26262 and UNECE WP.29

If your ECU targets ISO 26262 ASIL-B or UNECE WP.29 cybersecurity requirements:

- **ISO 26262-10:2018 §9.4.3** (Software security mechanisms) requires that random
  number generation used in security protocols uses hardware entropy sources.
- **ISO/SAE 21434:2021 §15** (Cybersecurity validation) requires that cryptographic
  operations are validated against their intended security properties — which
  includes the unpredictability of challenges.
- **UNECE WP.29 R155** requires that OEMs and suppliers demonstrate that software
  security controls (including diagnostic access) cannot be bypassed.

A software PRNG seed does not satisfy any of these requirements.

---

## Verification

Before shipping firmware, verify your seed entropy source:

```c
/* Quick entropy sanity check — run at startup, not in production loop. */
void eds_entropy_sanity_check(void)
{
    uint8_t seeds[8][8];
    uint8_t i, j;
    uint8_t dummy_len;
    bool    all_same;

    for (i = 0U; i < 8U; i++) {
        my_seed_generate(0x01U, seeds[i], 8U, &dummy_len);
    }

    /* Every seed should be different from every other. */
    for (i = 0U; i < 8U; i++) {
        for (j = (uint8_t)(i + 1U); j < 8U; j++) {
            all_same = true;
            uint8_t k;
            for (k = 0U; k < 8U; k++) {
                if (seeds[i][k] != seeds[j][k]) {
                    all_same = false;
                    break;
                }
            }
            if (all_same) {
                /* Duplicate seed detected — entropy source is broken. */
                /* Halt or log a critical fault here. */
                for (;;) { }
            }
        }
    }
}
```

---

## Failed Attempt Lockout Policy

SecurityAccess (0x27) implements a failed-attempt counter and lockout, per
ISO 14229-1's `requiredTimeDelay` mechanism. This section documents the
actual implemented semantics (`core/uds_security.c`, `core/uds_security.h`)
so you can decide whether the defaults fit your product before shipping.

| Parameter | Default | Configurable via |
|---|---|---|
| Max consecutive failed attempts before lockout | 3 | `uds_security_cfg_t.max_attempts` (`UDS_SECURITY_MAX_ATTEMPTS` if left at 0) |
| Lockout duration | 10,000 ms (10 s) | `uds_security_cfg_t.lockout_ms` (`UDS_SECURITY_LOCKOUT_MS` if left at 0) |

**The lockout duration is flat, not progressive.** Every time the counter
reaches `max_attempts`, the *same* configured duration applies — there is no
built-in exponential backoff (1s, 2s, 4s, 8s...). If your product needs
escalating delays, you must implement that yourself around the seed/key
exchange calls; EDS applies one fixed duration per lockout event.

**Counter reset:** the failed-attempt counter resets to 0 only on a
*successful* key validation. It does **not** reset when a lockout period
naturally expires — the counter stays at `max_attempts`, so the very next
failed attempt re-triggers lockout immediately with no partial credit. Only
a correct key clears it.

**Persistence is optional, not automatic.** By default (`nvm_load_cb` /
`nvm_save_cb` left `NULL` in `uds_security_cfg_t`) the counter and any
in-progress lockout live in RAM only and reset to zero on any reboot —
power-cycling the ECU is a full bypass. Wiring both callbacks makes the
counter and lockout residual survive a power cycle (loaded back in by
`uds_security_init()` on the next boot); this is the only way to close
the power-cycle bypass. If your product's threat model includes an
attacker with physical access to power, wire NVM persistence — the stack
does not require it, but it does not protect you without it either.

**NVM read faults: fail-open by default, fail-closed opt-in.** With
persistence wired, `uds_security_init()` calls `nvm_load_cb` at boot to
restore the counter and any residual lockout. Two outcomes are expected and
harmless: a successful load, and `UDS_STATUS_ERR_DID_NOT_FOUND` (first
boot — no record has been written yet). Any *other* return is a genuine NVM
fault: a corrupt record, or a platform-level read error. How EDS reacts to
that fault is controlled by `uds_security_cfg_t.nvm_load_fail_closed`:

| Setting | Behavior on a genuine NVM fault |
|---|---|
| `false` (default) | **Fail open.** `uds_security_init()` proceeds with the counter at zero and no lockout. Preserves diagnostic availability when NVM is unhealthy, at the cost of losing brute-force resistance across that one fault. Matches the stack's behavior before this flag existed — no change for any existing product. |
| `true` | **Fail closed.** SecurityAccess is locked out for the remainder of the current power cycle (`lockout_timer_ms` set to `UINT32_MAX` — effectively permanent for one boot, not a short timer that quietly expires). The only way out is a healthy reboot where `nvm_load_cb` succeeds. Prioritizes brute-force resistance over availability: an attacker cannot exploit a broken/corrupted NVM path to force an always-fresh attempt counter. |

Set `nvm_load_fail_closed = true` if your threat model treats "SecurityAccess
briefly unavailable" as strictly preferable to "SecurityAccess silently
un-throttled because storage is unhealthy" — e.g. products where an attacker
could plausibly induce NVM faults (corrupted sectors, glitching a flash
write) to reset the attempt counter. Leave it at the default if your product
prioritizes keeping diagnostics reachable even when NVM itself is degraded.
This flag is only consulted for a genuine fault; it never affects the
first-boot (`UDS_STATUS_ERR_DID_NOT_FOUND`) or successful-load paths.

---

## Questions

Contact **contact@xaloqi.com** for questions about entropy source validation,
key provisioning, or OEM cybersecurity requirements.

---

*This notice applies to Xaloqi EDS v1.x. Updated requirements will be published
in the changelog and this document when the security architecture changes.*
