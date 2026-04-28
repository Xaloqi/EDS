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

## Questions

Contact **security@xaloqi.com** for questions about entropy source validation,
key provisioning, or OEM cybersecurity requirements.

---

*This notice applies to Xaloqi EDS v1.x. Updated requirements will be published
in the changelog and this document when the security architecture changes.*
