# Changelog

All notable changes to the Xaloqi EDS are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

---
## [Unreleased]

## [1.13.0] — 2026-09-01

### Added

- **`tools/templates/dtc_config.h.j2` (Professional-tier, in EDS-toolchain)
  is now wired into `codegen.py`'s `RENDER_PLAN`.** (#175, companion to
  #158) The template — a per-example DTC X-macro list `harness_ecu.c`
  consumes so it can register each example's own DTCs instead of two
  hardcoded `basic_ecu` literals — existed since #158/[EDS-toolchain#76](https://github.com/Xaloqi/EDS-toolchain/pull/76),
  but `RENDER_PLAN` lives in this (public) repo, so a real `codegen.py`
  run couldn't produce `dtc_config.h` yet; EDS-toolchain kept 8 examples'
  copies in sync by hand as a stopgap. Added `build_dtc_config_context()`
  (reuses the same `_build_dtc_list()` enrichment `uds_init.c.j2`'s Step 5
  registration loop already relies on, so the two stay in lockstep by
  construction) and added the template to `RENDER_PLAN` unconditionally —
  `render_and_write()` already skips a missing template with a warning
  rather than failing, so this is a no-op on a public checkout without the
  commercial `tools/templates/`. Verified against the real EDS-toolchain
  templates (`--template-dir`): regenerating `ardep_ecu` and `sensor_ecu`
  matches their committed `dtc_config.h` on the timestamp line only.
- **`build_safety_config_context()` now threads `tools/codegen.py`'s own
  `__version__` into the safety_config.h.j2 template context as
  `stack_version`.** (#163, follow-up to #149/#156 and
  Xaloqi/EDS-toolchain#70) `render_safety_wrappers()`'s Jinja2 environment
  uses `StrictUndefined`, so any template variable not present in the
  context dict is a hard `SystemExit(3)` on every codegen run — meaning
  EDS-toolchain's `safety_config.h.j2` had no way to reference a
  stack-version variable, and `UDS_STACK_VERSION` in generated
  `safety_config.h` could only be fixed by hand-editing already-generated
  output, or by hand-bumping the template's own literal on every release.
  The new `stack_version` key is distinct from the existing `version` key
  (the ECU config's own `metadata.version`) and does not collide with it.
  Unblocks the EDS-toolchain-side follow-up to switch
  `#define UDS_STACK_VERSION "1.12.0"` to
  `#define UDS_STACK_VERSION "{{ stack_version }}"`, closing the loop
  `__version__` was picked as #149's single source of truth for.
- **Mandatory ASan + UBSan CI job on the host unit tests.** (#151) There was
  no sanitizer build anywhere in this repo's CI — `grep -rn "fsanitize"`
  over `.github/workflows/` and `build_tests.sh` returned nothing — even
  though `core/`, `transport/`, `config/` and `platform/` are hand-managed C
  under a no-malloc/no-recursion policy. New `sanitizers` job runs
  `bash build_tests.sh --sanitize`, which builds and runs all 44 modules
  with `-fsanitize=address,undefined -fno-sanitize-recover=all`
  (`-fno-sanitize-recover` matters: UBSan's default is to print one line and
  let the process exit 0). `--sanitize` is also available locally.
  The gate is `scripts/verify_sanitizer_gate.sh`, written to the run-013
  lesson — it asserts positive success rather than absence of failure:
  - both a planted ASan violation and a planted UBSan violation are still
    caught on this runner, using the flags recovered from `build_tests.sh`
    itself, so the gate proves it can fail before it is allowed to pass;
  - every test binary that ran carries `__asan*`/`__ubsan*` symbols, checked
    in the ELF rather than taken on trust;
  - the module count matches `tests/unit_runnable/`, and the suite executed
    at least 800 assertions with zero failures.
  Verified by deliberately injecting a stack-buffer-overflow and a signed
  integer overflow into a test module (caught, with real reports), by
  running the gate against an uninstrumented build, an empty log, a
  short-count log and a short-module log (all rejected), and against clean
  code (44 modules / 894 cases / 0 failures / 0 diagnostics).
- **`cmake-ctest-build` now asserts CTest registered and ran every module.**
  (#151) `ctest` exits 0 on "all registered tests passed", which says
  nothing about how many were registered — a `tests/CMakeLists.txt` that
  drifted from `build_tests.sh` again (the O-26/#92 failure) would report
  100% on a subset. The count is derived from `tests/unit_runnable/` so it
  cannot go stale, with a floor so a wiped directory cannot make it vacuous.

### Changed

- **The C unit test build compiles the EDS stack once instead of 44 times.**
  (#151) `build_tests.sh` and `tests/CMakeLists.txt` both compiled all 47
  shared sources independently for each of the 44 test modules — ~1,900
  translation units for what is ~47 objects plus 44 small drivers. The stack
  is now compiled once per compile-time configuration:
  `build_tests.sh` produces `libeds_testable.a` and links it with
  `--whole-archive`; `tests/CMakeLists.txt` uses an OBJECT library
  (the portable equivalent on its CMake 3.16 baseline). Wall-clock:
  `build_tests.sh` **58.1s → 12.5s**, CMake configure+build+ctest
  **19.8s → 3.1s**. The source list and compile definitions stay mirrored
  1:1 between the two paths, as O-26/#92 requires.
  - `--whole-archive` / OBJECT library is not cosmetic: a plain static-archive
    link extracts only the members needed to resolve an already-undefined
    symbol. Measured on this codebase that silently drops 279 symbols,
    including `g_uds_service_table` and the entire DID handler set — and the
    affected module still reports PASS.
  - Two modules need the *shared* sources built in a different configuration
    (`test_trng_fail_closed` with `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0`,
    `test_uds_acl_permissive_opt_in` with
    `UDS_ACL_ALLOW_UNLISTED_SERVICES=1`; both macros change code in
    `core/uds_security_algo.c`, `core/uds_access_table.c` and
    `core/uds_server.c`). They get their own library variant. Confirmed
    load-bearing rather than decorative: linked against the default library
    instead, each drops from 4/4 to 1/4 passing. The variant id is derived
    from the flags themselves, so a third per-module `-D` automatically
    gets a third library rather than silently reusing the default one.
  - Verified content-identical, not merely "still green": every test
    binary's full Unity output was captured before and after and diffed —
    byte-for-byte identical, 44 modules / 894 cases / 0 failures, via
    `build_tests.sh`, via the `--whole-archive`-less fallback path, and via
    a real `cmake`/`ctest` run (100%, 44/44).
- **`build_tests.sh` now reports and asserts the number of test *cases* that
  ran**, not just how many modules exited 0. (#151, run-013) A module whose
  `run_all_tests()` executes nothing still exits 0 and was reported as PASS;
  the summary now carries a `Unit Test Cases: N run, M failed` line and the
  script fails if any module executed zero cases.
- **CI guard: `uds_msg_buf_t` can no longer be silently stack-allocated
  without a build failure.** (#152) `core/uds_types.h`'s
  `EDS_MSG_BUF_MAX_STACK_BYTES` `_Static_assert` documents its own
  limitation — it checks `sizeof(uds_msg_buf_t)` against a configured
  threshold, which is necessary but not sufficient: an integrator who
  raises the threshold for a host build (e.g.
  `-DEDS_MSG_BUF_MAX_STACK_BYTES=8192`) got zero protection from it. New
  `scripts/verify_no_stack_uds_msg_buf.sh`, wired into the
  `integration-tests` CI job, greps `core/`, `transport/`, `platform/`, and
  `config/` for non-static, non-comment `uds_msg_buf_t <name>;`
  declarations — the shape of a stack (automatic-storage) local — and fails
  the build if it finds one. Verified against a real positive case: a fake
  stack-declared `uds_msg_buf_t` was temporarily added to
  `platform/zephyr/zephyr_can.c`, confirmed to make the script fail with
  the exact line flagged, then removed and confirmed clean again.

### Documentation

- **Consolidated documentation-accuracy pass across `docs/ARCHITECTURE.md`,
  `docs/TESTING_STRATEGY.md`, and `docs/GETTING_STARTED.md`, plus a widened
  `scripts/verify_doc_counts.py` guard.** (#170, #164) `docs/ARCHITECTURE.md`'s
  repo-tree diagram claimed 36 unit-test modules (actual 44 — the line right
  below it in the same block already said 44), showed `build_tests.sh` /
  `build_harness.sh` nested under a `scripts/` directory that has never
  existed (both live at the repo root), and its `tests/` node named
  `integration/` and `harness/` subdirectories that don't exist while
  omitting the real `tests/runner/`; also fixed the `examples/` node (listed
  4 of 12 examples under two names — `motor_controller/`, `ardep/` — that
  don't match the real `motor_controller_ecu/`, `ardep_ecu/` directories).
  `docs/TESTING_STRATEGY.md` had two sections (§7 Integration Tests, §10
  System Tests) describing a `tests/integration/` suite and a `pytest
  --system` flag that were never built — replaced with the real generated
  per-example suite layout and an explicit note on §10 that it describes
  target coverage, not an implemented one; its §11 CI pipeline tree named 10
  of the 22 real `ci.yml` jobs with no indication the list was partial —
  added the 3 more substantive test layers it was missing
  (`robustness-tests`, `harness-tests`, `sovd-codegen`) and an explicit
  callout naming what's deliberately omitted (7 repetitive per-example
  smoke-build jobs, one extra board variant) rather than trying to keep it
  exhaustively hand-synced a third time. `docs/GETTING_STARTED.md`'s Step 8
  walkthrough pointed at a `tests/integration/test_uds_read_did.py` that
  never existed, with test names (`test_read_vin`, `test_read_odometer`)
  that don't exist anywhere in the repo — replaced with a real, verified-passing
  example (`test_did_f190.py`, the generated VIN-read suite) and corrected
  the surrounding narrative, which had implied the request would be sent to
  Step 7's `native_sim` process even though `--can-interface=simulator` is a
  separate, self-contained virtual bus that doesn't talk to it.
  `scripts/verify_doc_counts.py`'s two existing checks (#119) were both blind
  to ASCII tree diagrams — the dead `scripts/build_tests.sh` path was split
  across a `scripts/` tree node and a `build_tests.sh` leaf line, never on
  one line so the substring check never fired, and `(36 modules)` matched
  none of the prose count patterns. Widened both, and fixed a real bug found
  while doing so: the doc sweep wasn't excluding the gitignored
  `.claude/worktrees/` scratch directory, so a stray agent worktree with
  stale doc copies could fail the guard for a reason having nothing to do
  with the tracked repo. Verified per lessons/run-014: reverted just
  `docs/ARCHITECTURE.md` to confirm the widened guard still catches the
  original 3 problems it was built for, then restored the fix and confirmed
  clean.
- **`can_transport_transmit()`'s CONFIRMED-vs-queued contract was
  undocumented, even though ISO-TP's N_As/N_Ar timers depend entirely on
  it.** (#152) `transport/can_transport.h` said only that a successful
  return meant the frame "was accepted for transmission" — never whether
  that meant queued into the driver or physically confirmed on the bus.
  `transport/isotp.h`/`isotp.c` already treat `can_transport_transmit()`'s
  return as the N_As/N_Ar transmission-confirmation event, so the actual
  timing semantics depended on an unstated contract. Audited both platform
  HALs: `platform/zephyr/zephyr_can.c` satisfies a CONFIRMED contract today
  (`can_send()` with `callback=NULL` blocks until the controller confirms
  the frame or a `K_MSEC(25)` timeout elapses); `platform/freertos/freertos_can.c`
  is a thin, timing-agnostic pass-through to a customer-supplied
  `eds_can_send_fn_t`, so its actual behavior depends on that customer
  implementation — and `docs/INTEGRATION_GUIDE.md`'s own reference example
  used a bare `HAL_CAN_AddTxMessage()` call, which returns as soon as the
  frame is queued into a TX mailbox, silently defeating N_As/N_Ar for
  anyone who copied it as-is. No shipped runtime-stack behavior needed to
  change (the FreeRTOS shim itself is agnostic, not wrong), but the
  documented contract and the guide's example did. Now: `can_transport.h`'s
  `can_transmit_fn`/`can_transport_ops_t.transmit`/`can_transport_transmit()`
  doc comments state the CONFIRMED contract explicitly, `isotp.h`'s N_As/N_Ar
  notes cross-reference it, `platform_api.h`'s `eds_can_send_fn_t` doc states
  the same requirement for customer FreeRTOS implementations, and
  `INTEGRATION_GUIDE.md`'s example now polls for mailbox completion instead
  of returning on enqueue, with a prominent contract callout above it.

### Security

- **SecurityAccess stayed unlocked across an S3 inactivity timeout.** (#140)
  `uds_security_reset()` was only ever called from one site —
  `service_0x10.c`, when an explicit 0x10 DiagnosticSessionControl(default)
  request transitions the session to DEFAULT. The S3 timeout drives that
  exact same transition (`uds_session_tick_1ms()` forces the session back to
  DEFAULT on expiry), but through a path that never touched security. A
  level unlocked before S3 expiry stayed unlocked after the forced return to
  DEFAULT — any tester, not necessarily the one that performed the seed/key
  exchange, could re-enter a non-default session with a bare
  DiagnosticSessionControl request and inherit the previous unlock, no
  seed/key exchange of its own. `uds_server_tick_1ms()` now calls
  `uds_security_reset()` whenever `uds_session_tick_1ms()` reports the S3
  timeout, mirroring the explicit path exactly. Found by the 2026-08-31
  validation campaign; new unit test
  `test_s3_timeout_resets_security_unlock` in `test_uds_server.c`.

### Fixed

- **`test_step5_data_length_nrc_13_firmware`'s "too-short WriteDID" firmware
  test hardcoded the short byte count instead of deriving it from the DID's
  own `data_length`.** (#160, found while verifying #144) Every generated
  `test_firmware_services.py` computed `short_data` as
  `bytes([0xAA] * max(1, 1 - 1))` — a byte count that never referenced the
  actual DID at all. For `sensor_ecu` / `sensor_ecu_freertos`,
  `DID_CATALOGUE[5]` is DID 0xD010 with `data_length: 1`: the hardcoded
  expression always produces 1 byte, which is the DID's *full* length, not a
  short one, so the firmware accepted the write and returned a positive
  response (0x6E) where the test asserted a negative one — confirmed via
  `--firmware` against a locally built harness (`assert 110 == 127`). The
  other 10 examples' `DID_CATALOGUE` index used by this test happens to have
  `data_length >= 2` (e.g. `basic_ecu`'s DID 0xF187 is 11 bytes), so the same
  wrong expression coincidentally still produced a genuinely-short write —
  masking the bug everywhere except the two 1-byte-DID examples.
  Fixed to `bytes([0xAA] * max(0, DID_CATALOGUE[N]["data_length"] - 1))`,
  reading the DID's real length instead of a baked-in literal. Note the floor
  had to change from 1 to 0, not just the literal: the issue's own suggested
  `max(1, data_length - 1)` still evaluates to 1 for a `data_length=1` DID
  (`max(1, 1 - 1) == max(1, 0) == 1`), which is exactly the bug being fixed —
  writing "one byte short" of a 1-byte DID is a 0-byte write, and `max(0, …)`
  is needed to let it through as 0 while still flooring longer DIDs at a
  minimum of 0 bytes removed (never negative). Worked examples:
  `data_length=1` → `max(0, 1 - 1) = 0` bytes (was 1, wrongly matching the
  full length); `data_length=11` → `max(0, 11 - 1) = 10` bytes (was already
  10 via the old hardcoded literal for that example, so no behavior change
  there). Verified for both `sensor_ecu` and `sensor_ecu_freertos` against a
  freshly built local harness (Professional-tier `harness/` extracted for
  verification only, never committed — `harness/` stays gitignored per #67):
  `test_step5_data_length_nrc_13_firmware` reproducibly fails on the
  pre-fix code (`assert 110 == 127`) and passes after the fix; `basic_ecu`
  (`data_length=11`) still passes unchanged. All 10 affected
  `examples/*/generated/tests/test_firmware_services.py` files fixed here;
  `safeboot_ecu`'s copy of this test already `pytest.skip()`s (no
  write-capable DID in that configuration) and needed no change. Companion
  fix in the EDS-toolchain codegen template that produces this file tracked
  separately so future `--test-gen` runs emit the corrected expression.
- **`basic_ecu` robustness tests failed instead of skipping cleanly when
  `tools/templates/` is absent.** (#165) `codegen.py` runs its license check
  (import `_license`, commercial-only) before it ever loads or validates the
  config YAML, so on a clean public checkout — no `tools/templates/`, no
  `_license.py` — every subprocess invocation exits 1 with a "Commercial
  License Required" banner regardless of input. 40 tests across
  `test_robustness_A_codegen.py`, `test_robustness_F_codegen_limits.py`, and
  `test_robustness_K_error_quality.py` asserted on specific exit codes and
  stdout/stderr text for invalid-input cases (bad YAML, duplicate IDs, ASIL
  violations, etc.) and failed on the banner instead of the validation
  message they were exercising —
  `test_robustness_L_codegen_output_fidelity.py` already skipped cleanly in
  this situation via a `_TEMPLATES_OK` / `pytest.mark.skipif` guard, but A/F/K
  either didn't apply it consistently or (K) didn't have it at all. Applied
  the same guard to exactly the affected tests: A and F already had the
  `_TEMPLATES_OK` check defined but not used on every codegen-invoking test
  in `TestCodegenInvalidInputs` / `TestDuplicateAndReservedIDs`; K gained the
  guard from scratch. Left alone the tests in the same classes/files that
  only assert a bare non-zero exit code (or otherwise incidentally read
  correctly against the license banner) and were already passing, per-test
  rather than a blanket per-file skip, so genuinely template-independent
  coverage keeps running. Verified: reproduced the 40 FAILED on the pre-fix
  tree, confirmed all 40 now report SKIPPED with
  "Jinja2 templates not present (commercial-only, not in public repo)", and
  diffed the full list of passing tests before/after (`basic_ecu`'s
  `generated/tests/` suite: 17 passed → 17 passed, identical set) to confirm
  no previously-passing test was accidentally swept into the guard.
  CI's "Robustness Campaign" job runs all 12 phases (A–L) together rather
  than the single file checked above, and its own `passed >= 332` floor
  (`.github/workflows/ci.yml`) is a *different* measurement — confirmed the
  full campaign genuinely shifts from 332 passed / 107 skipped to 292
  passed / 147 skipped (439 total, 0 failed, unchanged both ways) once this
  fix is in the tree, run directly (not just inferred from the single-file
  diff). Updated the floor to 292 with an explanatory comment rather than
  leaving 40 tests' worth of headroom silently baked in.
- **Stack-buffer-overflow in `test_uds_security.c`.** (#168, found by #151's
  new ASan job on its first run) `test_uds_security_send_key__test_null_key`
  declared `uint8_t seed[4]` but passed it to `do_seed_request()`, which
  declares the buffer to the stack as `UDS_SECURITY_SEED_LEN` (8) bytes — so
  `core/uds_security.c:219`'s `memcpy` wrote 4 bytes past the end of a test
  stack frame on every run, silently, with the module still reporting PASS.
  The production stack is not at fault and is unchanged: it already rejects
  an honestly-declared short buffer with `UDS_STATUS_ERR_BUFFER_OVERFLOW`.
  The test buffer now uses the macro, as every other seed buffer in the
  suite already did.
- **The Python test suite had no canonical entrypoint — root-level `pytest`
  collection failed outright.** (#150) Every `examples/*/generated/tests/`
  directory's `conftest.py` declares `pytest_plugins = ["conftest_firmware"]`
  (added by #143), which pytest only allows in a conftest.py sitting at its
  own session rootdir; a bare `pytest` from the repo root tried to walk all
  12 example trees in one session and hard-errored on every one of them
  ("Defining 'pytest_plugins' in a non-top-level conftest is no longer
  supported"), collecting only 11 stray tests. A new root-level `pytest.ini`
  (`testpaths = tests`) scopes a bare root `pytest` run to this repo's own
  hand-written `tests/` suite — each example's `generated/tests/` already
  ships its own committed `pytest.ini`/`conftest.py` and is self-contained
  exactly as README.md and CI already document
  (`cd examples/<name>/generated/tests && pytest ...`); regenerating those
  12 generated trees to share a root conftest was not an option here — they
  are produced by `tools/testgen.py`, a commercial gitignored deliverable
  not present in a bare public checkout, and must never be hand-edited. New
  `run_python_tests.sh` (mirrors `build_tests.sh`'s role for the C suite) is
  the canonical "run everything" command: `tests/` plus every example, each
  scoped to its own directory, with a pass/`[ENV]`/fail summary per suite.
  Also fixed: `tests/test_license_expiry.py` hard-errored at collection
  (`ModuleNotFoundError: No module named '_license'`) in any checkout
  without the commercial `tools/_license.py` module — it now degrades to a
  `[ENV]`-tagged skip under pytest (and to a plain printed message, exit 0,
  when run standalone). Skip reasons for known environment gaps (missing
  `xaloqi-tester`, missing DoIP ECU binary, missing `_license` module) now
  carry a `[ENV]` prefix so they're greppable and visibly distinct from a
  real test failure, per the issue's second ask.

- **CI's "Integration Tests (generated, simulator mode)" job has executed
  zero tests since the initial public release.** (#141, #142) `conftest.py`'s
  `from xaloqi.tester import ...` failed at import time because the job
  never installed `xaloqi-tester`, so every generated test hit the
  `_XALOQI_AVAILABLE` skip guard — confirmed on the actual v1.12.0 release
  run's log: `136 skipped in 0.28s`. The pass/fail gate,
  `grep -q "failed" && exit 1 || true`, passed trivially on that all-skipped
  output. #142 installs `xaloqi-tester` from real PyPI and replaces the gate
  with an explicit passed/failed count (zero failed, at least 134 passed).
  Installing it for the first time surfaced two real, independent bugs,
  fixed in #141:
  - `test_request_results()`'s own docstring claimed "requestRoutineResults
    after start", but the generated test never called Start — the real UDS
    stack correctly rejects an un-started RequestResults with NRC 0x22
    (`conditionsNotCorrect`) via `service_0x31.c`'s `routine_started` gate.
    28 generated `test_routine_*.py` files across all 12 examples asserted a
    positive response instead. Template fix in
    [EDS-toolchain#66](https://github.com/Xaloqi/EDS-toolchain/pull/66);
    regenerated here.
  - `examples/basic_ecu/generated/tests/test_phase1_protocol_edge_cases.py`
    (hand-written, not template-generated) called ClearDiagnosticInformation
    (SID 0x14) directly in the default session, in three of its four tests.
    `core/uds_access_table.c` deliberately restricts 0x14 to non-default
    sessions, so two tests asserted `pdu[0] == 0x54` and got NRC 0x7F
    instead; a third discarded ClearDTC's response entirely and only
    happened to pass because there were never any DTCs to clear in the
    first place. All three now enter EXTENDED session first, and the third
    asserts ClearDTC actually succeeded before checking the post-clear DTC
    list.

- **`test_firmware_services.py`'s `firmware_bus` fixture was unreachable,
  and once reachable, pointed at the wrong repo root — INSTALL.md Step 6's
  "All tests should pass" was false for every firmware-backed test.** (#143)
  pytest only auto-loads `conftest.py`, never a sibling
  `conftest_firmware.py`, so `firmware_bus` errored with `fixture
  'firmware_bus' not found` in every example (104 setup errors per example).
  Fixing that (`pytest_plugins = ["conftest_firmware"]`, template fix in
  [EDS-toolchain#67](https://github.com/Xaloqi/EDS-toolchain/pull/67))
  surfaced a second, previously-unreachable bug: `_REPO_ROOT` was computed
  with too few parent hops from
  `examples/<ecu>/generated/tests/conftest_firmware.py`, landing on
  `examples/<ecu>` instead of the actual repo root — `build_harness()` never
  found `build_harness.sh`, so every firmware test cleanly skipped instead
  of erroring, and zero firmware tests could ever actually run, in any
  example, ever, until now. `pytest test_firmware_services.py --firmware`
  now builds the real C harness and runs 56 passed, 1 skipped for
  `basic_ecu` — the first genuine pass/fail signal this file has ever
  produced. (A separate, larger gap — `build_harness.sh` only ever builds
  one generic harness hardcoded to `basic_ecu`'s DIDs/routines/DTCs, so the
  other 11 examples' firmware tests still fail on protocol mismatch rather
  than a "fixture not found" — is tracked as
  [#144](https://github.com/Xaloqi/EDS/issues/144), not fixed here.)

- **`build_harness.sh` only ever built one generic harness, hardcoded to
  `basic_ecu`'s generated sources — every other example's firmware-backed
  tests failed on protocol mismatch, not a real defect.** (#144) Once #143
  made `firmware_bus` reachable, running `test_firmware_services.py
  --firmware` for any example besides the `basic_ecu` family compiled and
  launched a harness binary built from `examples/basic_ecu/generated/{did_
  handlers,did_safety_wrappers,routine_handlers}.c` regardless of which
  example's tests were actually running — e.g. `sensor_ecu` failed 20 of 60
  tests with wrong DID content, wrong routine IDs, and a wrong DTC set,
  because it was really talking to basic_ecu's firmware. `build_harness.sh`
  now takes a `--example <name>` flag (or `EXAMPLE` env var, default
  `basic_ecu`) and builds against that example's own `generated/` sources
  and include path; the default output binary is now
  `/tmp/harness_ecu_test_<example>` so builds for different examples never
  collide or clobber each other's cached binary. `conftest_firmware.py`'s
  `harness_binary()` / `build_harness()` derive their own example name from
  `Path(__file__)` (they already live inside
  `examples/<name>/generated/tests/`) rather than requiring a caller to
  specify it, and stay session-scoped — one build per example's own test
  session is still correct. Verified by building and running
  `--firmware` for `basic_ecu` (56 passed, 1 skipped, unchanged) and all 10
  other examples with a `tests/` dir: every DID/routine protocol-mismatch
  failure is gone. `sensor_ecu`, `motor_controller_ecu`, `ardep_ecu`,
  `bms_ecu`, `robot_joint_controller_ecu`, and `safeboot_ecu` each now
  fail only on DTC-registration assertions, traced to a separate,
  narrower pre-existing bug — `harness/harness_ecu.c` hardcodes DTC
  registration to `basic_ecu`'s two DTCs regardless of example — tracked as
  [#158](https://github.com/Xaloqi/EDS/issues/158); `sensor_ecu` and
  `sensor_ecu_freertos` additionally hit a hardcoded byte-count in a
  generated WriteDID test, tracked as
  [#160](https://github.com/Xaloqi/EDS/issues/160). Neither is fixed here.

All five found and fixed during the 2026-08-31 validation campaign; see
`xaloqi-knowledge/campaigns/2026-08-31-validation-campaign.md`.

- **SID 0x19 (ReadDTCInformation): sub-functions 0x0B and 0x19 implemented
  the wrong ISO 14229-1 sub-function.** (#148) `core/uds_services/service_0x19.c`
  defined `SVC_0x19_SUBFN_REPORT_FAULT_DETECTION_CTR` as `0x0BU` and
  `SVC_0x19_SUBFN_REPORT_DTC_PERMANENT_STATUS` as `0x19U`. Per ISO 14229-1
  Table 239, 0x0B is actually `reportFirstTestFailedDTC` (§11.3.11) and 0x19
  is `reportUserDefMemoryDTCExtDataRecordByDTCNumber` — neither implemented
  by this ECU; `reportDTCFaultDetectionCounter` is really sub-function 0x14
  (§11.3.20) and `reportDTCWithPermanentStatus` is really 0x15 (§11.3.25). A
  real UDS tester sending the standard 0x14/0x15 got "sub-function not
  supported," while 0x0B/0x19 — which the standard defines as entirely
  different services — got this ECU's reinterpretation of them. Fixed by
  renumbering the two existing (behaviorally correct) handlers to their
  ISO-correct codes 0x14 and 0x15; 0x0B and 0x19 now fall through to the
  same "sub-function not supported" response as every other unimplemented
  sub-function in the dispatch switch (reportFirstTestFailedDTC and
  reportUserDefMemoryDTCExtDataRecordByDTCNumber remain unimplemented — out
  of scope for this fix). Updated the hand-written
  `tests/unit_runnable/test_service_0x19.c` regression suite and the
  generated `test_report_fault_detection_counter_sub0b` /
  `test_report_dtc_permanent_status_sub19` tests (renamed to `..._sub14` /
  `..._sub15`) across all 11 `examples/*/generated/tests/test_services.py`
  files that carry them. Found while triaging #145. The private
  EDS-toolchain template that generates `test_services.py` needs the
  equivalent fix so future codegen doesn't regenerate the wrong values;
  filed as a follow-up note in the fix PR, not fixed here (different repo).
- **`test_robustness_D_customer_journey.py`'s per-example nested pytest
  subprocess had no `timeout=`, so how long it could hang was whatever the
  outer test runner happened to impose rather than anything deterministic.**
  (#153) `TestAllECUExamplesPytest.test_ecu_pytest_simulator_all_pass`
  spawns a nested `pytest ... --can-interface=simulator` per example via the
  `_run()` helper (a thin `subprocess.run(..., capture_output=True,
  text=True)` wrapper) with no bound at all — an external qualification
  evaluator's environment hung on this for the FreeRTOS sensor example even
  though that example's suite completed cleanly (`109 skipped`) when run
  directly. Measured real per-example runtime (`basic_ecu`'s suite driving
  all 11 examples sequentially: 98.71s / 11 ≈ 9s/example, individual runs
  5–9s) and added an explicit `timeout=90` (~10x the slowest observed
  example) to that call, catching `subprocess.TimeoutExpired` and failing
  with `"nested pytest timed out after 90s for {ecu}"` — a message that
  reads as a timeout, not a generic subprocess or assertion error.
- **Version identity was broken: several files still claimed old
  versions instead of the current 1.12.0.** (#149) `core/uds_types.h`'s
  `UDS_SUITE_VERSION_MAJOR/MINOR/PATCH` and `UDS_SUITE_VERSION_STRING`
  still read 1.10.0; `CMakeLists.txt`'s `project(... VERSION 1.10.0)`
  (and its header comment) likewise; `SECURITY.md`'s supported-versions
  table still listed "1.7.x (current)"; and all 12
  `examples/*/generated/safety_config.h` files still defined
  `UDS_STACK_VERSION "1.7.0"`. None of these matched README.md's version
  badge, CHANGELOG.md's top entry, or `tools/codegen.py`'s `__version__`
  (all 1.12.0), so a build, a security report, or a generated ECU could
  each report a different EDS version depending on which file was
  consulted. All now read 1.12.0.

### Documentation

- **The Requirements Traceability Matrix and MISRA Deviation Log counts in
  INSTALL.md were both wrong.** (#146) "X ASIL-B requirements, all COVERED"
  — of the RTM's 30 total rows, 16 are ASIL-B and 14 are QM. "38
  deviations" / "38 documented deviations" — the log actually contains 39
  records (10 in Rev 1.0, 28 in Rev 1.1, 1 — `DEV-MEM-01` — in Rev 1.2,
  whose revision-history entry was never reflected in the summary count).
  Fixed in both places INSTALL.md states each. Companion fix at the source:
  [EDS-Safety#4](https://github.com/Xaloqi/EDS-Safety/pull/4), which also
  fixes an unquoted comma in the RTM CSV's `REQ-FLASH-003` row that shifted
  every following column for any standard CSV parser. Found by the
  2026-08-31 validation campaign.

## [1.12.0] — 2026-08-30

### Changed

- **The `unit-tests` CI job no longer carries the module count in its display
  name, and the stale root-level `ci.yml` duplicate is gone.** (#119) The job
  was named `Unit Tests (43 modules)` and that exact string was a required
  status check in the repo ruleset. Bumping the count to 44 for #113 renamed
  the job, so nothing ever reported the required context again and the PR sat
  at "Expected — Waiting for status to be reported" with all 21 jobs green.
  The count lived in three places that had to move together — the job name,
  the `Verify test count` assertion, and repo settings — and only two of them
  are in version control. The job is now plain `Unit Tests`; the count is
  asserted inside the `Verify test count` step, where it belongs and where
  drift already fails CI loudly. Also deleted the repo-root `ci.yml`, an inert
  copy last touched in #50 that GitHub never read: it claimed "6 jobs", parsed
  to 7, and named its unit-test job `Unit Tests (39 modules)`, while
  CONTRIBUTING.md's new-service checklist said to update "`ci.yml`" without
  saying which of the two files it meant.

- **README and COMMERCIAL_NOTICE spell out the proprietary-firmware linking
  right explicitly.** (#106) The open-core section and COMMERCIAL_NOTICE
  already stated that a commercial license covers shipping closed-source
  firmware, but not the specific question a reviewer evaluating the license
  actually asks: can the GPL runtime (`core/`, `transport/`, `config/`,
  `platform/`) be statically or dynamically linked into that firmware and
  the binary distributed, with no GPL disclosure obligation for the
  licensee's own code? Both files now answer that plainly, sourced from
  `LICENSE_COMMERCIAL.txt` §5 — no change to the actual license terms.

### Fixed

- **ISO-TP: the N_Ar timer in `isotp_tick_1ms()` could drive `rx_state` to
  `ISOTP_STATE_ERROR` from any state, including `IDLE`.** (#135) The N_Ar
  branch added by #122 decremented `rx_ar_timer_ms` and fired unconditionally,
  unlike its TX-side mirror, N_As, which #111 deliberately guarded on
  `tx_state`. Latent under the single-diagnostics-task model — `isotp_send_fc()`
  arms and disarms `rx_ar_timer_ms` around one `can_transport_transmit()` call,
  so it is never still armed when a tick lands — but `can_transport_transmit()`
  is permitted to block, and in an integration that drives `isotp_tick_1ms()`
  from an independent timer context, a tick landing inside that window could
  expire N_Ar and set `rx_state = ISOTP_STATE_ERROR` concurrently with a
  caller's `isotp_reset()` / `isotp_reset_rx()` (#132), silently undoing the
  recovery. Guarded the branch on `rx_state == ISOTP_STATE_RX_WAIT_CF` — the
  only state in which an FC confirmation can be outstanding — and added the
  symmetric disarm-on-`IDLE` branch, mirroring #111's N_As treatment exactly.

- **ISO-TP: `isotp_get_state()` hid an RX error behind any concurrent transmit,
  and `isotp_reset()` could not recover one direction alone.** (#132)
  `isotp_ctx_t` runs two independent state machines, `rx_state` and `tx_state`,
  but the only public read path collapsed them: `isotp_get_state()` reported
  `tx_state` whenever it was not `ISOTP_STATE_IDLE` and `rx_state` only
  otherwise. An RX channel sitting in `ISOTP_STATE_ERROR` was therefore
  invisible to a polling caller for the entire duration of a transmit — and on
  a UDS server the transmit direction is busy for exactly as long as a
  segmented response is going out, which is precisely when an inbound request
  is being reassembled. Compounding it, `isotp_reset()` — the documented
  recovery from `ISOTP_STATE_ERROR` — cleared both directions unconditionally,
  so a caller that did observe an RX error and reset destroyed any in-flight
  TX with it. Per-direction error handling was not expressible at all. #122
  made this materially more reachable by adding a new way for `rx_state` to
  become `ERROR` (a rejected Flow Control transmit) while a TX is active.
  Added `isotp_get_rx_state()` / `isotp_get_tx_state()`, which report each
  direction faithfully, and `isotp_reset_rx()` / `isotp_reset_tx()`, which
  recover one direction while leaving the other's state, buffers and timers
  untouched — verified safe by a field-ownership audit showing the two
  directions share nothing mutable, only the read-only configuration and the
  bound `can_transport_t`. `isotp_get_state()` and `isotp_reset()` keep their
  exact previous behaviour for source compatibility; `isotp_get_state()`'s
  aliasing is now documented as a warning on its declaration, and
  `isotp_reset()` is composed from the two directional clears so a full reset
  can never drift out of step with the narrow ones. Also fixed the FreeRTOS
  ECU-reset snippet in `docs/INTEGRATION_GUIDE.md` §4.3, which called
  `isotp_get_state(tp, &rx_st, &tx_st)` — a three-argument form that has never
  existed in any released header, so the documented sequence could not compile;
  it now calls `isotp_get_tx_state()`, which is what it always meant.

- **11 examples' `generated/tests/test_services.py` was missing ReadDTCInformation
  sub `0x0B`/`0x19` coverage that the template gained months ago.** (#127)
  `EDS-toolchain` commit `2efba87` added `test_report_fault_detection_counter_sub0b`
  (ISO 14229-1 §11.3.11) and `test_report_dtc_permanent_status_sub19` (§11.3.25) to
  `tools/templates/test_services.py.j2`, but the committed `generated/` output for
  every example that carries a `tests/` directory (`ardep_ecu`, `basic_ecu`,
  `basic_ecu_doip`, `basic_ecu_doip_freertos`, `basic_ecu_freertos`, `bms_ecu`,
  `motor_controller_ecu`, `robot_joint_controller_ecu`, `safeboot_ecu`, `sensor_ecu`,
  `sensor_ecu_freertos` — `safeboot_freertos_ecu` is generated without `--test-gen`
  and was never affected) predated that commit. Not a functional defect — missing
  test coverage only — first found and deliberately excluded from #124's regeneration
  scope (logged as O-37) to keep that PR's diff to its actual subject. Regenerated all
  11 examples' full `generated/` output via `tools/codegen.py --test-gen` (plus each
  example's own `--safety-wrappers --asil-level B` / manifest flags, matched to what
  was already committed) and diffed every touched file against the previously
  committed version before staging: outside `test_services.py`, nothing changed but
  the expected per-generation timestamp fields (`Generated:` headers,
  `GEN_GENERATED_TIMESTAMP`, and — for the 3 examples that carry one — the manifest's
  `generated_at`/`config_source`/`output_dir`/`generator`); inside `test_services.py`,
  the only change in all 11 was the same timestamp line plus the two new test methods,
  verbatim.

- **ISO-TP: every Flow Control transmit failure was discarded, and there was no
  N_Ar timer at all.** (#122) Both `isotp_send_fc()` call sites in
  `transport/isotp.c` threw the return value away with `(void)` — the comment at
  the CTS site said so outright ("best-effort; ignore transmit errors"). If the
  CAN controller rejected the FC (bus-off, full TX mailbox, arbitration loss
  past the driver's own timeout), the receiver still transitioned to
  `ISOTP_STATE_RX_WAIT_CF` and returned `UDS_STATUS_OK` as though flow control
  had been granted. The sender never saw the FC and failed on its own N_Bs,
  while the ECU silently burned its full N_Cr (150 ms) waiting for consecutive
  frames that could not come — and the actual fault, a failed local transmit the
  platform layer *did* report, was visible nowhere. Separately, ISO 15765-2
  Table 5's **N_Ar** — the receiver-side mirror of N_As, the confirmation window
  for the FC the receiver transmits, budgeted at 25 ms — did not exist in any
  form: no `ISOTP_TIMEOUT_AR_MS`, no timer field. Both call sites now propagate
  the status: a rejected FC drives the RX channel to `ISOTP_STATE_ERROR` and
  returns `UDS_STATUS_ERR_TP_TX_FAILED`, matching how this file already handles
  an RX fault that has mutated context (the CF handler's SN, DLC and overflow
  paths) and how the TX path handles a failed `can_transport_transmit()` in
  `isotp_tx_pump()`. At the OVERFLOW site the transmit failure now takes
  precedence over `ERR_TP_OVERFLOW`, which would have reported the peer's
  protocol condition while hiding our own hardware fault. New
  `ISOTP_TIMEOUT_AR_MS` (25 ms) and `isotp_ctx_t.rx_ar_timer_ms` scope N_Ar to
  the single FC transmit exactly as #111 scoped N_As: armed immediately before
  `can_transport_transmit()`, stopped on its return, with a tick-handler branch
  (new `UDS_STATUS_ERR_TP_TIMEOUT_AR`, 0x38) as the enforcement point for ports
  whose transmit blocks — the Zephyr port blocks in `can_send()` for up to
  `K_MSEC(25)`. The window after the FC is N_Cr, never N_Ar. Found in the same
  pass as #111; the opposite defect class — #111 invented a spurious error on a
  valid exchange, this one suppressed a genuine one. **Rebase note:** #121
  (merged first) added a *third* `isotp_send_fc()` call site — the periodic
  block-boundary FC the CF branch sends once BlockSize handling is in place —
  which still discarded its status with `(void)` and an explicitly stale
  comment claiming to mirror the FF handler's (by-then-fixed) CTS. Brought to
  the same standard as the other two sites while reconciling the two branches,
  with its own regression test
  (`test_periodic_block_boundary_fc_tx_failure_is_reported`) proven to fail
  against the unreconciled code and pass after.

- **ISO-TP RX: the receiver advertised a BlockSize it never honoured — any
  `isotp_cfg_t.block_size > 0` stalled every inbound multi-frame transfer.**
  (#121) `isotp_init()` stored `cfg->block_size` into `ctx->local_block_size`
  and the First Frame handler sent exactly one FC CTS carrying it, but the
  Consecutive Frame branch never called `isotp_send_fc()` at all — both call
  sites were inside the FF handler (the initial CTS and the OVERFLOW
  rejection). ISO 15765-2 §9.6.5 requires the receiver to send a *further* FC
  after every BS consecutive frames before the sender may continue, so a
  tester that honoured the advertised BS sent BS frames, stopped, and waited
  for an FC the ECU would never transmit: the transfer stalled until the
  tester's own N_Bs expired while the ECU sat in `ISOTP_STATE_RX_WAIT_CF`
  until N_Cr (150 ms) fired. Latent only because `ISOTP_DEFAULT_BLOCK_SIZE` is
  0 (unlimited) and every bundled example leaves it there — it became a hard
  interop failure the moment an integrator set a non-zero block size, which is
  the normal configuration for flow-controlled bulk transfers (e.g. 0x36
  TransferData over a constrained RX path), precisely the case BlockSize
  exists to serve. The CF branch now tracks CFs per block in a new
  `rx_blocks_received` context field and emits a fresh FC CTS through the
  existing `isotp_send_fc()` once the count reaches `local_block_size`,
  mirroring the TX side's `tx_block_size`/`tx_blocks_sent` handling in
  `isotp_tx_pump()`. No FC follows the final CF — the PDU is already complete
  and the sender has nothing left to send. `block_size == 0` (unlimited)
  remains a no-op path, so the default configuration's emitted frame sequence
  is byte-for-byte unchanged; a dedicated non-regression test asserts exactly
  one FC frame for a five-CF transfer at BS=0, alongside a BS=2 test that
  byte-checks each follow-up FC (FS=CTS, BS and STmin echoed) and the
  reassembled payload.

- **A corrupted DTC mirror no longer aborts `uds_stack_init()` — boot now
  soft-degrades instead.** (#124) `dtc_mirror_load()`'s
  `UDS_STATUS_ERR_NVM_DATA_CORRUPT` return (added in #114/#118 for a
  persisted record that fails its integrity check) was not on the generated
  Step 5.5 non-fatal allowlist, so on all 12 bundled examples a corrupted DTC
  mirror propagated as a fatal `uds_stack_init()` failure, taking down the
  entire diagnostic stack's boot. `UDS_STATUS_ERR_NVM_DATA_CORRUPT` is now
  treated the same as an absent/unreadable mirror (`UDS_STATUS_ERR_NOT_INITIALIZED`
  / `UDS_STATUS_ERR_PLATFORM`): the corrupt record is discarded and boot
  continues with all DTC status bytes at 0x00 — the same blast radius as the
  original DTC-history-loss bug #114 fixed, not a new, larger one. Fixed in
  the private EDS-toolchain template
  ([EDS-toolchain#57](https://github.com/Xaloqi/EDS-toolchain/pull/57)); all
  12 `examples/*/generated/uds_init.c` regenerated here.

- **Stale counts and dead script paths across the prose docs, now guarded in
  CI.** (#119) Six places in `docs/` told the reader to run
  `scripts/build_tests.sh` or `scripts/build_harness.sh`; neither has ever
  existed — both scripts live at the repo root, and one of those instructions
  sits in the getting-started path, i.e. the first command a new user runs.
  Unit-test counts were stated as 42, 39 and 35 against an actual 44.
  `docs/ARCHITECTURE.md`'s CI section claimed 16 jobs and tabulated 18, ten of
  which (`firmware-integration-tests`, `gui-build`, `ardep-example`,
  `bms-example`, `bms-zephyr-native`, `mc-example`, `mc-zephyr-native`,
  `sensor-example`, `robotics-example`, `safeboot-example`) no longer exist,
  while omitting eight that do; that table is now generated from the live
  workflow and states no job count, following the precedent set in
  `.github/workflows/ci.yml` itself after #91. `docs/AI_CONTEXT.md` attributed
  the MCP server tests to a `mcp-server-tests` job in this repo's CI — they
  actually run as `validate-mcp` in the private EDS-toolchain repo, since the
  MCP server is commercial tooling not present here. New
  `scripts/verify_doc_counts.py`, wired into the `unit-tests` job, now fails CI
  if any doc states a unit-test count that disagrees with
  `tests/unit_runnable/test_*.c` or references a `scripts/build_*.sh` path.
  `CHANGELOG.md` and `docs/PHASE1_SECURITY_CHANGES.md` are exempt — both are
  historical records of what was true at the time.

- **DoIP: a UDS response of 4085–4095 bytes was silently dropped — no frame,
  not even a negative response.** (#108) `uds_msg_buf_t.data` is sized
  `UDS_MAX_PAYLOAD_LEN` (4095), eleven bytes above the DoIP frame budget
  (`DOIP_MAX_PDU_SIZE - DOIP_HEADER_LEN` = 4088, of which 4 are the response
  addressing header, leaving 4084 usable). A service handler that legitimately
  filled its response buffer into that range fell through
  `doip_handle_frame()`'s `DOIP_PT_DIAGNOSTIC_MSG` send guard entirely. Because
  the positive ack for the *request* had already gone out (ISO 13400-2 §9.5),
  the tester was left to time out on its own P2 timer with no protocol-level
  rejection — while the request-side equivalent, `DOIP_NACK_MSG_TOO_LARGE`,
  has always existed for oversized *requests*. An oversized response is now
  downgraded in place via `uds_server_build_negative_response()` to a UDS 0x7F
  carrying NRC 0x14 (`responseTooLong`, ISO 14229-1), which always fits, and
  sent through the existing encode/send path; the original request's SID is
  captured before the buffers are reused so the NRC echoes it correctly.
  Deliberately does not increment `ctx->negative_response_count` — that counter
  tracks *dispatch* outcomes, and here dispatch succeeded and the transport
  could not carry the result, a different failure class. Three regression tests
  cover the dead zone (4090 B), the unchanged boundary (4084 B, byte-exact
  positive response) and the new edge (4085 B), and were verified non-vacuous
  by mutating the fix's boundary condition. Fixed in #109; this entry was
  missed at merge time.

- **DTC NVM mirror: no integrity check — a truncated/corrupted record
  degraded to silent partial success.** (#114) `config/dtc_mirror.c`'s
  on-disk format was just a 2-byte entry count followed by fixed-size
  entries — no magic number, version, or CRC. On load, a truncated entry
  mid-loop simply `break`d out of the parse loop and the function still
  returned `UDS_STATUS_OK`, so a corrupted record was indistinguishable
  from a legitimately short one and whatever entries had been parsed so
  far were silently applied. The on-disk format is now
  `[magic:2][version:1][count:2][entries...][crc32:4]`, reusing the
  existing transfer-service CRC-32 engine
  (`uds_transfer_crc32_update`/`_finalise` in `core/uds_transfer_ctx.c`,
  already a global include for `config/` per the declared build-layer
  order). `dtc_mirror_load()` now validates magic, version, declared
  length, and CRC-32 *before* applying any entry, so a corrupt record can
  never partially apply — it is now reported distinctly via the new
  `UDS_STATUS_ERR_NVM_DATA_CORRUPT` status instead. A record that is too
  short or does not carry the new magic (including a legacy pre-fix
  record with the old headerless format) is deliberately treated as "no
  mirror yet" and discarded — a documented, one-time migration trade-off
  on first boot after upgrading to this fix. `dtc_mirror_flush_all()` and
  `dtc_mirror_clear_all()` now share one writer for the new format. New
  regression tests in `tests/unit_runnable/test_dtc_mirror.c` cover a
  valid round-trip, a CRC-mismatched entry, a truncated record, a legacy
  (no-magic) record, and an unsupported version — all reported as the
  correct one of the three distinct cases (no-mirror / valid / corrupt).

- **ISO-TP: the N_As timer was armed once at First Frame and never rearmed or
  stopped, aborting any multi-frame TX that took longer than 25 ms.** (#111)
  `tx_as_timer_ms` was set to `ISOTP_TIMEOUT_AS_MS` (25 ms) when the FF went
  out and was then never rearmed on FC CTS reception and never rearmed per
  consecutive frame in `isotp_tx_pump()`, so it counted down across the entire
  transfer — `TX_WAIT_FC` *and* `TX_SEND_CF` — instead of across one frame.
  `isotp_tick_1ms()` checks it before the STmin pump and returns immediately on
  expiry, so any multi-frame transmission whose total wall-clock time from FF
  to completion exceeded 25 ms went to `ISOTP_STATE_ERROR` mid-transfer on a
  fully valid protocol exchange. That is trivially true for any peer
  requesting a non-trivial STmin (a 20 ms STmin errors out after the second
  CF) or for any transfer of more than a handful of CFs; an FC WAIT sequence,
  which restarted N_Bs but not N_As, tripped it too. Root cause: N_As
  (ISO 15765-2 §6.7.2, Table 5 — the confirmation window of a *single*
  transmitted frame) had been collapsed into what was effectively a
  whole-transfer watchdog spanning windows that belong to N_Bs and STmin/N_Cs.
  N_As is now armed immediately before each `can_transport_transmit()` call on
  the segmented TX path (both FF paths and every CF) and stopped on that
  call's return, which is the transmission-confirmation point at this layer —
  it therefore measures exactly the one-frame window it is defined to measure
  and never accumulates. The FC wait is bounded by N_Bs (75 ms) as
  ISO 15765-2 intends, so a peer that never sends an FC is still caught, now
  with `UDS_STATUS_ERR_TP_TIMEOUT_BS` at 75 ms rather than
  `UDS_STATUS_ERR_TP_TIMEOUT_AS` at 25 ms. Existing tests missed this because
  they inject TX state directly instead of driving the public FC path, and
  never tick `isotp_tick_1ms()` across a realistic elapsed interval — one of
  them had to explicitly zero `tx_as_timer_ms` to stop N_As from pre-empting
  the N_Bs timeout it was actually testing. Three new regression tests drive a
  true 1 ms tick cadence through a full multi-CF transfer at STmin = 20 ms
  (80 ms total, well past N_As) via a real FC CTS frame and assert the
  transfer completes with the payload byte-identical on the wire, that N_As is
  not left armed across the FC wait, and that an FC WAIT followed by a late
  CTS still completes.

- **DoIP: `tcp_send()` short writes were treated as a complete send, silently
  truncating frames on real Ethernet.** (#105) Both call sites in
  `transport/doip/doip_server.c` accepted any positive return from
  `tcp_send()` as "fully sent" — but POSIX `send()` (and the LwIP/FreeRTOS
  backends behind `eds_doip_platform_ops_t`) may legally transmit fewer
  bytes than requested. The remainder was then silently dropped, producing
  a truncated DoIP frame on the wire. This never reproduced on
  loopback/`native_sim`, where a single `tcp_send()` call virtually always
  accepts the whole buffer — it surfaces on real Ethernet under congestion,
  with large diagnostic responses (up to ~4 KB), or against a stricter TCP
  stack. Added `doip_send_all()`, a bounded retry loop used at both call
  sites that keeps calling `tcp_send()` until the full buffer is on the
  wire. The bound counts only *consecutive no-progress* calls (128 in a
  row returning `<= 0`) against a give-up threshold, not total call count —
  a merely slow connection that keeps delivering some bytes every call,
  however few, is never killed by this bound and always completes; only a
  genuinely wedged peer (zero bytes accepted, repeatedly) does. (An earlier
  iteration of this fix counted every call — progress or not — against one
  flat 128-attempt cap, which could itself have dropped a perfectly good
  response under real Ethernet congestion — exactly the scenario this issue
  was filed for; caught in review before merge.) When `doip_send_all()`
  does give up, `doip_handle_frame()` now reports the failure so
  `eds_doip_server_run()` closes the connection via its existing cleanup
  path, instead of leaving a peer waiting on a partial/desynced frame that
  will never fully arrive. New unit tests give `mock_tcp_send()` a
  configurable per-call byte cap (and a dedicated "always stalls" mode) to
  simulate short writes, and assert: a large response reassembles
  byte-identical under a comfortable 64B/call cap; a *slow but always
  progressing* 24B/call send still succeeds even though it needs more than
  128 calls; and a truly stalled peer is bounded and reported as a failure
  the caller must close the connection over.

- **TransferData (0x36): an oversized block was silently clamped to
  `bytes_remaining` instead of being rejected.** (#112) When a tester sent
  more payload than the remaining declared transfer size (e.g. a
  `RequestDownload` size=100, 98 bytes already received via one block, then
  a 20-byte block with only 2 bytes remaining), `service_0x36.c` truncated
  `payload_len` down to `bytes_remaining`, fed the truncated data into the
  running CRC-32, wrote it to flash, and advanced the block sequence
  counter as if the block were well-formed — silently discarding the
  excess bytes rather than treating the malformed block as an error. Now
  the handler rejects the block with NRC 0x31 (`requestOutOfRange`) and
  aborts the transfer context via `uds_transfer_ctx_reset()`, mirroring how
  `service_0x37.c` already handles the symmetric REQ-DL-002 case (too few
  bytes at `RequestTransferExit`). A subsequent 0x36 after the abort now
  correctly sees no active transfer (NRC 0x24) instead of resuming a
  half-torn-down context. Added regression tests reproducing the exact
  issue scenario and asserting the post-abort context is a clean reset,
  not a partially-updated one; the pre-existing test that encoded the old
  clamp-and-continue behavior as correct has been rewritten to assert the
  reject-and-abort behavior instead.

- **DTC NVM mirror could exceed the shared NVM record cap before
  `UDS_MAX_DTC_COUNT` was reached — silent persistence loss at worst-case
  fault load.** (#123) `config/dtc_mirror.h`'s `DTC_MIRROR_MAX_BYTES` was
  sized from `UDS_MAX_DTC_COUNT` (128): `5 + 128*4 + 4 = 521` bytes, 9 bytes
  over `platform/nvm_store.h`'s `NVM_MAX_RECORD_BYTES` (512) — a cap shared
  with unrelated NVM consumers (security counters, session stats), so
  widening it was out of scope (flagged but deliberately deferred when
  #114 added the 9-byte integrity header/CRC that narrowed the safe
  ceiling from 127 to 125). Once the live DTC table grew past 125 entries,
  `nvm_store_write()`'s own length guard silently rejected the mirror
  write with a non-OK status; both call sites already treat that as
  best-effort/non-fatal, so nothing crashed — but ConfirmedDTC persistence
  silently stopped updating exactly when a device was closest to its
  worst-case fault load, the scenario #114's integrity metadata exists to
  make trustworthy. Fixed by capping what the mirror promises to persist,
  not by raising the shared NVM cap: new `DTC_MIRROR_MAX_PERSISTED_DTCS`
  (125, the largest count that fits `NVM_MAX_RECORD_BYTES` alongside the
  header and CRC trailer) now bounds both the serializer's iteration and
  `DTC_MIRROR_MAX_BYTES`'s own derivation, enforced by a build-time
  `_Static_assert` in `dtc_mirror.c` so any future change to the sizing
  constants fails the build instead of failing silently at runtime.
  `dtc_database` still holds up to `UDS_MAX_DTC_COUNT` (128) DTCs; entries
  beyond the persisted cap are a documented limitation — simply not
  written, never truncated or corrupted. New regression tests: filling to
  exactly the new cap round-trips every entry through a flush/power-cycle/
  load; filling to the full `UDS_MAX_DTC_COUNT` (128) still flushes OK,
  with entries beyond the cap correctly absent (not corrupted) after
  reload — this is the one that fails against unmodified pre-fix source
  (`dtc_mirror_flush_all()` returns a non-OK status instead of OK); and a
  standalone arithmetic test recomputes the old pre-#123 formula directly
  from the raw wire-format constants to prove the defect was real,
  independent of today's cap.

### Security

- **BREAKING: UDS access-control table now fails CLOSED for any service_id
  with no explicit ACL row — audit your deployment before merging.** (#113)
  `core/uds_access_table.c`'s `uds_access_table_lookup()` /
  `uds_access_table_enforce()`, and `core/uds_server.c`'s
  `srv_check_access_rights()`, treated a service_id absent from the active
  access table (the built-in default table, or an OEM-supplied custom one
  via `uds_server_cfg_t.access_table`) as "no restriction" — fully
  reachable, in every session, with no security check, purely by omission.
  A new SID added to `service_registration.c` without a matching ACL row
  became silently unauthenticated. The default is now fail-closed: an
  unlisted service_id is DENIED (NRC 0x7F, serviceNotSupportedInActiveSession)
  unless the new compile-time opt-in `UDS_ACL_ALLOW_UNLISTED_SERVICES`
  (Kconfig: `CONFIG_UDS_ACL_ALLOW_UNLISTED_SERVICES`, default `n`/`0`) is
  explicitly set. **Every service_id the built-in default table ships today
  keeps its exact prior behaviour** — including SID 0x31 RoutineControl,
  the one registered service that previously had no ACL row at all and now
  has an explicit, deliberately permissive one (see the row's comment in
  `core/uds_access_table.c`). The behaviour change applies to: (1) any
  future SID added without a matching ACL row, and (2) any OEM-supplied
  custom `access_table` that does not cover every service_id it registers —
  audit your custom table's coverage, or set the opt-in, before upgrading a
  production deployment past this change. See the PR for the full SID
  audit and regression tests (fake SID 0x99 denied by default across
  sessions; opt-in verified to restore the old behaviour end-to-end).

## [1.11.0] — 2026-08-29

### Security

- **BREAKING: unified build-mode detection closes a silent bypass of both the
  CRIT-4 placeholder-key gate and the TRNG fail-closed gate — on Zephyr
  production builds and on EVERY FreeRTOS build.** (#84)
  `[SEC-BUILD-MODE-01]` Two independently-authored copies of "is this a
  production build?" existed in this codebase — the CRIT-4 placeholder-key
  `#error` in `core/uds_security_algo.c`, and the Step 7.1 runtime guard
  generated into every `examples/*/generated/uds_init.c` — and both used the
  same broken idiom: `defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY) &&
  !CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY`. Zephyr's generated `autoconf.h` emits
  `#define CONFIG_X 1` for a Kconfig bool set to `y`, and OMITS the symbol
  entirely for `n` — it never emits `#define CONFIG_X 0`. So on a real Zephyr
  **production** build (the bool set to `n`, exactly what the docs told
  integrators to do), `defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY)` was FALSE,
  and both gates silently did nothing — the CRIT-4 `#error` never fired and
  the Step 7.1 runtime refusal never triggered, in precisely the
  configuration they exist to protect.
  Both gates now derive from one shared primitive, `EDS_BUILD_IS_PRODUCTION`
  (`core/uds_security_algo.h`), so they cannot drift apart again. The CRIT-4
  gate is `EDS_BUILD_IS_PRODUCTION && !defined(UNIT_TEST)` (the
  `!defined(UNIT_TEST)` carve-out is unchanged from before this fix — it lets
  a host unit test force `EDS_BUILD_IS_PRODUCTION=1` to exercise a different
  gate's production behaviour, e.g. `test_trng_fail_closed.c`, without also
  tripping the placeholder-key `#error` against a test binary that was never
  going to have real keys). `ALGO_ENTROPY_FAIL_CLOSED`
  (`[SEC-TRNG-FAILCLOSED-01]`, added in #85) is now a plain alias for
  `EDS_BUILD_IS_PRODUCTION`.
  **FreeRTOS landmine, fixed the same change:** FreeRTOS has no Kconfig of
  its own, and no FreeRTOS example ever defined
  `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY` anywhere in its build — so under the
  OLD idiom, FreeRTOS was *always* unprotected by both gates (a second
  instance of the same bug), and under a naive fail-closed-by-default fix it
  would have flipped to refusing to build or initialise on *every* FreeRTOS
  build, including CI, with no way to declare itself a development build.
  All 4 FreeRTOS example `CMakeLists.txt` files
  (`examples/basic_ecu_freertos`, `basic_ecu_doip_freertos`,
  `sensor_ecu_freertos`, `safeboot_freertos_ecu`) now expose
  `-DEDS_PLACEHOLDER_KEYS_ONLY=<ON|OFF>` (default `ON`, i.e. development —
  mirroring Zephyr Kconfig's own `default y`), which feeds
  `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY` the same defined-and-1-or-0 signal
  Zephyr's Kconfig always gave.
  **This also retroactively closes a latent gap in the already-merged
  `ALGO_ENTROPY_FAIL_CLOSED` fix (PR #85, #90) for FreeRTOS specifically:**
  because that gate already had the corrected fail-closed-by-default
  polarity, and FreeRTOS never defined the Kconfig symbol, a real FreeRTOS
  build has always silently resolved to the production TRNG behaviour (hard
  refusal on TRNG loss) with **no supported way to opt into the development
  LFSR fallback** — a gap invisible to CI because the `freertos-qemu` job is
  build-only and never exercises a live SecurityAccess request. It is fixed
  by the same `-DEDS_PLACEHOLDER_KEYS_ONLY` CMake option.
  **Breaking change:** any FreeRTOS build, and any Zephyr production build
  that was silently bypassing the CRIT-4 key gate and/or the TRNG
  fail-closed gate before this fix, will now correctly refuse to *compile*
  (CRIT-4, if placeholder keys are still present) or *initialise* (Step 7.1
  runtime guard) unless it explicitly declares itself a development build
  (`CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=y` on Zephyr,
  `-DEDS_PLACEHOLDER_KEYS_ONLY=ON` on FreeRTOS — both are the default). This
  is the intended remediation, but it is a real behaviour change for anyone
  who was unknowingly relying on the previously-broken, silently-inert
  gates. See `SECURITY.md` for the corrected dev-vs-production tables.
  All 12 committed `examples/*/generated/uds_init.c` files carry the fixed
  Step 7.1 guard (`#if EDS_BUILD_IS_PRODUCTION` in place of the broken
  idiom) — applied as a targeted patch of that one block rather than a full
  codegen regeneration, since `tools/templates/uds_init.c.j2` (private
  EDS-toolchain repo) currently carries unrelated drift from what's
  committed here (an ISO 26262 citation update, and a CommunicationControl
  init step present in the template but missing from 4 of the 12 examples)
  — tracked separately, not bundled into this security fix
  ([EDS-toolchain#50](https://github.com/Xaloqi/EDS-toolchain/issues/50)).
  **`/code-review` caught and this fix corrects one real regression before
  merge:** `ALGO_ENTROPY_FAIL_CLOSED`'s alias definition
  (`#define ALGO_ENTROPY_FAIL_CLOSED EDS_BUILD_IS_PRODUCTION`) had no
  `#if !defined(ALGO_ENTROPY_FAIL_CLOSED)` guard, so the documented
  `-DALGO_ENTROPY_FAIL_CLOSED=1` command-line override (the CRIT-4
  deviation escape hatch) was silently discarded — verified by compiling
  with the override and `gcc -dM -E`, which showed the macro still
  resolving to `EDS_BUILD_IS_PRODUCTION`'s value, with only a
  "redefined" warning as the only trace. The guard is restored; also
  fixed a naming typo in the CRIT-4 comment (`k_level_keys[]` →
  `s_level_keys[]`, the actual identifier, in both new and pre-existing
  prose), deduplicated `build_tests.sh`'s CRIT-4 negative-compile test
  onto the existing shared `INCLUDES` array instead of a second
  hand-maintained copy, removed a fully redundant regression-guard
  compile (already proven by the ~42 other test modules built earlier in
  the same script run), and centralized the 4 FreeRTOS examples'
  `EDS_PLACEHOLDER_KEYS_ONLY` option into `cmake/eds_build_mode.cmake`
  rather than duplicating it per example — mirroring this codebase's own
  established convention (`cmake/eds_service_sources.cmake`) for exactly
  this class of drift risk. Re-verified against a real ARM cross-compile
  toolchain (`arm-none-eabi-gcc` + a fresh `FreeRTOS-Kernel` clone): every
  object file compiles clean through link, including the patched
  `uds_init.c`; the only remaining failure is the pre-existing, unrelated
  callback-signature bug tracked as
  [#96](https://github.com/Xaloqi/EDS/issues/96).

- **SecurityAccess entropy fallback now fails closed in production builds.**
  `[SEC-TRNG-FAILCLOSED-01]` When no TRNG callback was registered (or the
  registered callback failed at runtime), `algo_get_random()` in
  `core/uds_security_algo.c` silently fell back to a deterministic 16-bit
  Galois LFSR and SecurityAccess seeds kept being issued. This is now a
  **production-only** behaviour change: development/CI builds keep the
  unchanged LFSR fallback, but production builds (Zephyr build with
  `CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n`) refuse the seed request instead
  (`UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE`, NRC 0x24) and never run the LFSR.
  A refused request no longer advances the anti-replay sequence counter.
  The two failure modes are distinguished in the `uds_safety` platform
  violation record via `last_violation_code`
  (`UDS_STATUS_ERR_PLATFORM` for the dev-mode soft fallback vs.
  `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE` for the production hard refusal).
  See `SECURITY.md` for the full dev-vs-production behaviour table.

### Fixed

- **`freertos_platform_api.c`'s ISO-TP RX-complete callback had the wrong
  signature.** (#96) `eds_on_isotp_rx_complete()`'s `length` parameter was
  `uint16_t`, but `transport/isotp.h`'s `isotp_rx_complete_cb` typedef
  declares it `uint32_t` — a function-pointer type mismatch at the
  `isotp_process_rx_frame()` call site. CI's pinned `gcc-arm-none-eabi`
  (~10.3.1) only warned; a real `arm-none-eabi-gcc` 14.2.1 cross-compile
  rejects it outright with `-Wincompatible-pointer-types`, which is how this
  was found — verifying #84's FreeRTOS fix stopped a full link one object
  short. Widened the parameter to `uint32_t` to match the typedef exactly;
  `s_poll_req_buf.length` (`uint16_t`) is unaffected — the existing
  `UDS_MAX_PAYLOAD_LEN` (4095) bounds check already rejects anything that
  wouldn't fit before the narrowing assignment, now made explicit. Verified
  with a full real cross-compile + link of both FreeRTOS examples that use
  this file (`basic_ecu_freertos`, `safeboot_freertos_ecu`) — both now reach
  a complete `.elf`, and the fixed function compiles with zero warnings
  even under `-Wconversion -Wsign-conversion`, stricter than this build
  actually enforces.

- **All 12 example ECUs regenerated from the current codegen template,
  closing long-standing drift.** (EDS-toolchain#50) Two of the four
  examples added at "Initial public release" — `ardep_ecu`, `bms_ecu`,
  `motor_controller_ecu`, `sensor_ecu_freertos` — carried committed
  `generated/` output that predated two real template fixes made months
  earlier and never propagated: an ISO 26262 citation correction
  (`§9.4.3` → `:2018 §7.4.12`, present in the other 8 examples already)
  and `uds_comm_control_init()` Step 5.8, without which every SID 0x28
  (CommunicationControl) and 0x85 (ControlDTCSetting) request on those 4
  ECUs returned NRC 0x22 (`conditionsNotCorrect`) — a real functional
  bug, not just stale documentation. Also picked up `UDS_STACK_VERSION`
  catching up from a stale `1.0.0`/`1.6.0` to the current `1.7.0` on the
  same 4 examples (the ECU's own version, e.g. `ARDEP_IOController
  v1.0.0`, is unaffected — that's a separate field). The other 8
  examples only picked up the ISO citation fix (they already had
  CommunicationControl). Regenerated with the license-check bypass
  (`XALOQI_LICENSE_SKIP=1`) and diffed every file in every example
  against what was committed before touching anything — confirmed the
  only differences anywhere are the two fixes above, the expected
  per-generation timestamp, and the version bump; no DID/routine/safety
  wrapper logic changed for any example. None of the 4 affected examples
  are built by any CI job today (only `basic_ecu`, `basic_ecu_doip`,
  `basic_ecu_freertos`, and `safeboot_freertos_ecu` are) — verified
  correctness with a full host-side link of each against the real stack
  instead (zero undefined references besides the expected missing
  `main()`, which these init-sequence files don't define).

- **`uds_safety_reset_counters()` now clears `platform_violations`.** (#86)
  `platform_violations` was added to `uds_safety_ctx_t` by the `[HIGH-2]` fix
  after `uds_safety_reset_counters()` was already written, and was never
  wired into it — every other counter (`null_check_violations`,
  `session_check_violations`, `security_check_violations`,
  `bounds_check_violations`, `total_checks_performed`,
  `last_violation_code`) was reset, but `platform_violations` survived the
  call. `uds_safety_init()` already zeroed it correctly; only the
  test-harness-only reset path was affected — this function must not be
  called in production firmware. `tests/unit_runnable/test_uds_safety.c`
  gained a regression test, and `tests/unit_runnable/test_trng_fail_closed.c`
  no longer needs to work around the bug with delta-based counter
  assertions.

- **30 dormant unit test cases are now actually executed.** (#87)
  `ZTEST(suite, name)` cases in `tests/unit_runnable/test_phase5_security_algo.c`
  (28 cases) and `tests/unit_runnable/test_uds_security.c` (2 cases) compiled
  successfully but were never registered with a matching `RUN_TEST(...)` call
  in their module's `run_all_tests()`, so they silently never ran — the module
  still reported PASS. All 30 are now wired in.
  While fixing this, `tc031_seed_embeds_security_level` was found to assert a
  contract the implementation had deliberately abandoned: that
  `uds_security_algo_generate_seed()` embeds `security_level` at
  `UDS_ALGO_SEED_LEVEL_OFFSET`. It does not — domain separation is via a
  distinct AES key per level (see the `[P1-SEC]` comment in
  `core/uds_security_algo.c`). The case is rewritten as
  `tc031_seed_does_not_encode_security_level`, asserting the current contract:
  the byte at `UDS_ALGO_SEED_LEVEL_OFFSET` is identical across security levels
  at the same sequence-counter value and is fully explained by the sequence
  counter, not by `security_level`.

### Fixed — CI / build system

- **`tests/CMakeLists.txt` (the CTest build path) now actually configures,
  builds, and passes — and is CI-enforced.** (#88) Previously broken at three
  independent levels, none of which CI caught because CI only ran
  `bash build_tests.sh` and never invoked CMake:
  - Configure failed outright: `add_executable()` pointed generated sources
    at `<repo-root>/generated/`, which does not exist. Generated sources live
    under `examples/basic_ecu/generated/`, same as `build_tests.sh`.
  - Every target then failed `core/uds_types.h`'s `_Static_assert` on
    `uds_msg_buf_t` size: the compile definitions `build_tests.sh` passes
    (`UNIT_TEST`, `NVM_STORE_HOST_MOCK`, `ISOTP_ENABLE_CAN_FD`,
    `ISOTP_TX_PADDING`, `EDS_MSG_BUF_MAX_STACK_BYTES=8192`) were missing from
    the CMake path entirely.
  - Each `add_diag_test()` call carried its own hand-maintained, drifted
    `SOURCES` list instead of linking the full stack, so targets that did
    link failed with undefined-reference errors the moment one dependency
    was added.

  Fixed by introducing a single `DIAG_STACK_SRCS` list that mirrors
  `build_tests.sh`'s `STACK_SRCS` array file-for-file and is linked into
  every test executable, pointing generated-source references at
  `examples/basic_ecu/generated/`, and adding the missing compile
  definitions directory-wide. Also added the three targets that existed in
  `build_tests.sh`'s `TESTS` array but had no CMake equivalent
  (`test_service_0x2F`, `test_service_0x35`, `test_service_0x23_0x3D`),
  bringing the CMake path to parity: 43 modules, matching `build_tests.sh`.

  A new CI job, `"CMake/CTest Build (parity with build_tests.sh)"`, runs
  `cmake -S tests -B build && cmake --build build -j4 && ctest --test-dir
  build --output-on-failure` on every PR so this path cannot silently
  diverge from `build_tests.sh` again without CI going red.

### Added

- **CI coverage for the 7 example ECUs that had none.** (#98) Only
  `basic_ecu`, `basic_ecu_doip`, `basic_ecu_freertos`, and
  `safeboot_freertos_ecu` were ever built or checked by any CI job —
  `ardep_ecu`, `bms_ecu`, `motor_controller_ecu`,
  `robot_joint_controller_ecu`, `safeboot_ecu`, `sensor_ecu`, and
  `sensor_ecu_freertos` had zero CI signal. This is exactly how the O-27
  drift (a missing `uds_comm_control_init()` on 4 of these 7, causing
  every SID 0x28/0x85 request to return NRC 0x22) went unnoticed for
  months. Added 7 new jobs — `example-ardep`, `example-bms`,
  `example-motor`, `example-sensor`, `example-sensor-frtos`,
  `example-robot`, `example-safeboot` — ported from the equivalent,
  already-working jobs in the private EDS-toolchain repo's own CI and
  adapted for this repo's checkout layout. Deliberately does not add a
  `west build`/Zephyr SDK cross-compile per example — that would
  duplicate what `zephyr-native`/`zephyr-stm32`/`freertos-qemu` already
  prove for `basic_ecu`, at real toolchain-install cost, for a class of
  bug (missing an init call, config/header drift) that YAML validation
  plus generated-file assertions catch just as well. Also adds one
  assertion the source EDS-toolchain jobs don't have: a direct check for
  `uds_comm_control_init()` in each example's generated `uds_init.c` —
  the exact marker whose absence was O-27, verified to actually catch it
  (confirmed all 7 pass today, after O-27's fix).

- **Anti-drift guard for dormant unit tests.** (#87) `build_tests.sh` now
  fails the build if any `ZTEST(suite, name)` case in `tests/unit_runnable/*.c`
  has no matching `RUN_TEST(...)` call in that file's `run_all_tests()`,
  naming the exact case(s) and file(s). Prevents the class of bug fixed above
  from recurring silently.

### Documentation

- **Stale CI job-count comments corrected.** (#91) `.github/workflows/ci.yml`'s
  header comment said "8 jobs" and listed 8 by name — the actual count had
  grown to 14 and the list was missing 6 jobs entirely. `CONTRIBUTING.md` said
  "13 jobs" in one place and "10 CI jobs" in another, neither matching. All
  three unit-test-count mentions in `CONTRIBUTING.md` still said "42" (now
  43). Rather than replace one stale number with another that will drift the
  next time a job is added or split, the job-count references now point at
  `.github/workflows/ci.yml`'s `jobs:` block as the single authoritative
  list instead of hardcoding a count.

- **`uds_security_algo.h`'s ALGORITHM OVERVIEW comment corrected.** (#94) It
  described seed byte 6 as holding `security_level`, a layout the
  implementation no longer uses — domain separation is via a distinct AES
  key per level, not a seed byte, freeing the full 16-bit sequence counter
  across bytes 6-7. `UDS_ALGO_SEED_LEVEL_OFFSET` is noted as a
  byte-compatible alias for `UDS_ALGO_SEED_SEQ_HI_OFFSET`, not a distinct
  field — matching what `tests/unit_runnable/test_phase5_security_algo.c`'s
  `tc031` already tests (fixed in #93's PR, #92).

- `SECURITY.md` / `README.md`: state the SecurityAccess key *response*
  truncation (4 bytes; the seed stays 8 bytes) explicitly, and distinguish
  online-guess hardness (bounded by the 4-byte truncation, defended by the
  3-attempt NVM-persisted lockout) from key-derivation hardness (128-bit,
  via AES-128-CMAC). Closes the gap an external security review's 4/10
  rating was largely driven by — the public docs previously said only
  "AES-128-CMAC" with no mention of the truncation.

---

## [1.10.1] — 2026-08-27

### Fixed

- **`build_harness.sh` now explains itself on a community clone.** (#68, #70)
  The harness sources are a Professional-tier deliverable; running the script
  without them used to die with raw `cc1: fatal error: harness_main.c: No such
  file or directory`. It now exits early with a clear message pointing at the
  Professional ZIP, INSTALL.md Step 2, and `build_tests.sh` (which needs no
  commercial files).
- **codegen banner and SOVD `generatedBy` stamp said v1.7.0.** (#71)
  `__version__` bumped to 1.10.0, the SOVD stamp now derives from it, and a
  CI step asserts `__version__` matches the top entry of this file so the
  next release bump cannot miss it.

### Changed

- **Safety Manual reference updated to Rev 1.3.** (#75) INSTALL.md's tier
  description and document table pointed at the superseded Rev 1.1; the
  Professional ZIP has shipped Rev 1.3 since the 2026-07-07 rebuild.
- **codegen's "no templates" error now explains the license boundary.** (#77)
  Running `codegen.py` without the Developer/Professional templates used to
  fail with a bare "template directory not found." The error now states
  plainly that the runtime stack and example outputs are GPL and free to use,
  while the code generator requires a license — with a pointer to
  `COMMERCIAL_NOTICE.md`.
- **README and COMMERCIAL_NOTICE spell out the open-core boundary up front.**
  (#78) A new "what's free, what's licensed" section in the README, and a
  corrected COMMERCIAL_NOTICE (it previously referenced non-existent
  `docs/EDS_Safety_Manual_*` paths and undercounted the license tiers),
  so the free/paid line is explicit before a user hits it as an error.

---

## [1.10.0] — 2026-07-02 *(tag updated 2026-07-07 — see Fixed below)*

### Security

- **[P4-SEC-01] SecurityAccess: wrong-level key now counts as a failed
  attempt.** (#64, #65) `uds_security_send_key()` previously returned
  `REQUEST_OUT_OF_RANGE` immediately when the submitted key's security level
  did not match the pending seed level, without incrementing
  `failed_attempts` or clearing `seed_pending`. This allowed unbounded
  probing of level/sub-function pairings without ever tripping the lockout
  counter — an ASIL-B audit finding.

  Fix: on level mismatch the seed is consumed (`seed_pending = false`),
  `failed_attempts` is incremented, NVM persistence is called, and lockout
  is engaged if `max_attempts` is reached. The return code remains
  `REQUEST_OUT_OF_RANGE` so callers can distinguish a level mismatch from
  a wrong-key submission. Two regression tests added (TC-SEC-KEY-007,
  TC-SEC-KEY-008).

### Fixed

- **Committed symlinks broke the commercial ZIP install.** (#67, #69)
  `tools/templates`, `tools/testgen.py`, `tools/_license.py`, and `harness`
  were committed as symlinks into a sibling `EDS-toolchain` checkout. In every
  clone they dangled, and `unzip -o` per INSTALL.md Step 2 could not replace a
  dangling directory symlink — Developer customers never received the codegen
  templates, Professional customers never received the 68-test harness. The
  paths are now untracked and gitignored; INSTALL.md gained a migration note
  for clones made before 2026-07-06.
- **`build_harness.sh` missed 5 service files and `uds_periodic` in
  `STACK_SRCS`.** (#66) The harness now links the full v1.10.0 service set
  (0x23/0x3D, 0x2A, 0x2F, 0x35).

> **Tag note:** the `v1.10.0` tag was re-pointed on 2026-07-07 to include the
> three fixes above. If you fetched the tag before that date, run
> `git fetch --tags --force` and check out again.

### Added

- **Board support: NXP MR-CANHUBK3 (S32K344, Cortex-M7).**
  Adds `boards/mr_canhubk3/` with a Device Tree overlay and Kconfig fragment
  enabling EDS on the NXP S32K344 automotive SoC (and S32K312/S32K396 variants
  with minor clock adjustments). FlexCAN0 is configured at 500 kbit/s / 87.5%
  sample point using the on-board TJA1443 transceiver (PTA6/PTA7). Flash layout:
  IVT header (256 B, offset 0x0) + image-0 1792 KB (0x2000) + image-1 1792 KB
  (0x1C2000) + diag_nvs 472 KB / 59 × 8 KiB sectors (0x382000) = 4048 KB total.
  Application code starts at 0x402000 (after the S32K3 IVT region at 0x400000).
  A new `zephyr-nxp-s32k` CI job verifies compilation on every pull request.
  See `docs/INTEGRATION_GUIDE.md` §6.5 for the wiring reference and build
  command. (Closes #58)

- **SID 0x23 — ReadMemoryByAddress** (ISO 14229-1:2020 §14.9).
  Allows a tester to read directly from an ECU memory address without a DID
  or a full 0x35/0x36/0x37 upload transfer sequence.  Primary use cases:
  calibration constant inspection, configuration register readback, RAM variable
  monitoring during bench testing.

  - **`core/uds_services/service_0x23.c`** — handler: validates ALFID, parses
    `memoryAddress` + `memorySize` fields (big-endian, 1–4 bytes each), rejects
    requests that would overflow the response buffer, validates the address range
    against the new `readable` memory map flag (REQ-FLASH-003), calls `read_cb`,
    returns `[0x63, dataRecord...]`.  Session gate: Programming + Level 1 unlock
    (ACL table entry [13]).

  - **New `readable` flag in `uds_flash_region_t`** (`platform/uds_flash_ops.h`).
    Calibration ROM and read-only configuration areas can now be exposed for 0x23
    without being declared writable in the DFU map.  Existing designated-initialiser
    tables that omit `readable` default to `false` (no change in DFU behaviour).

  - **`uds_transfer_validate_readable_range()`** added to
    `core/uds_services/service_transfer_common.h` (mirrors
    `uds_transfer_validate_memory_range()` but checks `.readable`).

- **SID 0x3D — WriteMemoryByAddress** (ISO 14229-1:2020 §14.10).
  Allows a tester to write directly to an ECU memory address — calibration
  constant patching, post-production ECU configuration — without a full DFU cycle.

  - **`core/uds_services/service_0x3D.c`** — handler: same ALFID/address/size
    parsing as 0x23, validates the full data payload is present in the request,
    rejects if address+size is not in the writable memory map (REQ-FLASH-002),
    calls `write_cb`, echoes `[0x7D, ALFID, memoryAddress, memorySize]`.
    Session gate: Programming + Level 1 unlock (ACL table entry [14]).

  - **ASIL-B safety note**: `uds_transfer_validate_memory_range()` is the sole
    gate between the tester and arbitrary flash writes.  The check is mandatory
    and not conditional on configuration.

- **`uds_transfer_parse_alfid()`** extracted to `service_transfer_common.h`
  (static inline).  `service_0x34.c` and `service_0x35.c` refactored to use it,
  eliminating ~12 lines of duplicated mask/shift/validation logic.

- **39 new unit tests** (`tests/unit_runnable/test_service_0x23_0x3D.c`):
  20 for 0x23 (null ctx, all ALFID error paths, range validation, readable vs.
  writable region discrimination, read_cb failure, response format) and 19 for 0x3D
  (same coverage plus data-payload length validation, write_cb data verification,
  writable vs. readable region discrimination, response echo correctness).

- Platform `readable = true` added to `platform/zephyr/harness_flash_mock.c`
  and `platform/freertos/freertos_flash_ops.c` existing flash region tables.

- MISRA deviation log updated (DEV-MEM-01; DEV-MULT-01 and DEV-CONV-01 file
  lists extended) — `EDS-Safety/MISRA_DEVIATION_LOG.md` revision 1.2.

- Closes [#53](https://github.com/Xaloqi/EDS/issues/53).

### Fixed

- `CMakeLists.txt`: `core/uds_services/service_0x35.c` was missing from the
  `DIAG_CORE_SOURCES` list (SID 0x35 was registered and working via the harness
  build but not included in the primary CMake target).  Added alongside 0x23 and
  0x3D.

- **SID 0x36 / 0x37 — wrong NRC on sequence error (BUG-01).**
  `service_0x36.c` and `service_0x37.c` both returned
  `UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION` when no transfer was active,
  which `srv_status_to_nrc()` maps to NRC 0x7F
  (serviceNotSupportedInActiveSession). ISO 14229-1 §14.4.2 requires NRC 0x24
  (requestSequenceError) in this case. Fixed by returning
  `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE`, which maps to NRC 0x24 per the existing
  table in `uds_server.c`. Unit tests TC-0x36-003 and TC-0x37-003 updated to
  assert the correct status code.

### Changed

- **Version constants synced to v1.9.0** — four files were left at v1.7.0 after
  the v1.9.0 release: `core/uds_types.h` (version macros + string),
  `CMakeLists.txt` (`project(VERSION ...)`), `INSTALL.md` (header, git tag, and
  zip filenames), `core/uds_safety.h` (header comment). All updated to v1.9.0.

---
## [1.9.0] — 2026-06-26

### Added

- **SID 0x2A — ReadDataByPeriodicIdentifier** (ISO 14229-1 §11.5).
  The ECU now pushes DID data autonomously at tester-configured rates without
  waiting for repeated requests. One-time subscription from the tester; the ECU
  streams `[0x6A, periodicId, dataRecord...]` frames until session returns to
  Default or the tester sends `transmissionMode = stopSending (0x04)`.

  - **New module `core/uds_periodic.h/.c`** — stateless tick-driven scheduler.
    Static subscription table `s_subs[UDS_PERIODIC_MAX_SUBSCRIPTIONS]` (default 8).
    Three push rates: `SLOW` (1000 ms), `MEDIUM` (100 ms), `FAST` (10 ms).
    `uds_periodic_tick_1ms()` advances all counters; `uds_periodic_pop_due()` returns
    at most one ready frame per call. No malloc/free anywhere.

  - **New `core/uds_services/service_0x2A.c`** — subscription handler.
    Validates `transmissionMode` (NRC 0x12 for 0x00/0x05+), looks up each
    `0xF2xx` DID via the 5-step safety chain, checks `DID_ACCESS_READ`, and
    registers or removes subscriptions. Positive response: `[0x6A]` (1 byte).
    `stopSending` is a no-op if the ID was not subscribed.

  - **Integration** — all 12 examples and `platform/freertos/freertos_platform_api.c`
    updated with `uds_periodic_tick_1ms()` + drain loop and session-change callback
    that calls `uds_periodic_cancel_all()` on return to Default session.
    DoIP-only examples (no ISO-TP transport) get `init` + session callback only.

  - **26 new unit tests** (`tests/unit_runnable/test_uds_periodic.c`,
    `tests/unit_runnable/test_service_0x2A.c`): scheduler lifecycle
    (TC-PERIODIC-001–014) and handler branches (TC-0x2A-001–012).

  - Closes [#48](https://github.com/Xaloqi/EDS/issues/48).

- **SID 0x35 RequestUpload** — ECU-to-tester data readback over the existing 0x36/0x37 transfer state machine. Symmetric counterpart to 0x34 RequestDownload. Supports calibration data readback, NVM log extraction, and flash image verification readout. Requires Programming session + Level 1 security unlock. `read_cb = NULL` is backward-compatible and returns NRC 0x22. Closes #46.

- `extras/wireshark/eds.lua`: Wireshark Lua dissector — UDS service decode (all 14 SIDs), full NRC table, ISO-TP PCI frame types, DoIP payload types (0x0005–0x0008, 0x8001–0x8003) — closes #44

- SID 0x2F InputOutputControlByIdentifier: actuator and I/O control for EOL and bench testing. `returnControlToECU` / `resetToDefault` / `freezeCurrentState` / `shortTermAdjustment` (CR-010) — closes #47

### Changed

- **`cmake/eds_service_sources.cmake`** (new file) — canonical list of all 17 UDS service handler source files. All 20 example `CMakeLists.txt` files (EDS + EDS-toolchain) now `include()` this list instead of enumerating service files individually. Adding a new SID requires one line here; no other `CMakeLists.txt` edits. See `CONTRIBUTING.md` — *Adding a new UDS service handler*.

---
## [1.8.3] — 2026-06-24

### Added

- **NXP FRDM-MCXN947 board support** (`boards/frdm_mcxn947/`).
  Adds `frdm_mcxn947.overlay` and `frdm_mcxn947.conf` so `examples/basic_ecu`
  builds and runs on the NXP MCX N947 (Cortex-M33, 150 MHz, FlexCAN0).
  Zephyr board target: `frdm_mcxn947/mcxn947/cpu0`.
  - FlexCAN0 (PIO1_10/PIO1_11) with `sample-point = <875>` (87.5%, CiA 601)
  - `can0` alias wired to `&flexcan0`; pinctrl from base board DTSI
  - NVS partition: last 256 KB of 2 MB internal flash (`diag_nvs`, 32 × 8 KB sectors)
  - Flash partition table rewritten (896 KB image-0 + 896 KB image-1 + 256 KB NVS)
  - Watchdog: WWDT0 (`nxp,lpc-wwdt`) already enabled in board DTS
  - Console: J-Link onboard via LPUART4 (flexcomm4_lpuart4, 115200 baud)
  - Stack protection: Cortex-M33 SPLIM via `BUILTIN_STACK_GUARD` (hardware, superior
    to software canaries; board defconfig `HW_STACK_PROTECTION=y` auto-selects it)
  - CI: new `zephyr-nxp` job compiles `basic_ecu` for this target on every PR



- **SID 0x19/0x0B — `reportDTCFaultDetectionCounter`** (ISO 14229-1 §11.3.11).
  Returns a 4-byte record `[dtcHB, dtcMB, dtcLB, faultDetectionCounter]` for each
  DTC where `testFailed == 0` and `fault_detection_counter < 0xFF`. No
  `DTCStatusAvailabilityMask` byte in the response (per-spec format differs from
  0x01/0x02/0x0A). Empty list response (2-byte header) is valid and returned when no
  DTCs qualify.

- **SID 0x19/0x19 — `reportDTCWithPermanentStatus`** (ISO 14229-1 §11.3.25).
  Returns `[0x59, 0x19, availabilityMask, {dtcHB, dtcMB, dtcLB, statusByte}…]` for
  each DTC marked permanent. Permanent DTCs are not cleared by SID 0x14 — only by
  the application via `dtc_database_set_permanent(dtc_code, false)` after a
  successful drive-cycle healing sequence.

- **`dtc_entry_t` — two new fields** (`config/dtc_database.h`):
  - `uint8_t fault_detection_counter` — debounce counter managed by the application
    via `dtc_database_set_fault_counter(dtc_code, counter)`. Range 0x00–0xFE;
    0xFF is reserved (confirmed, excluded from 0x0B response). Initialised to 0x00.
  - `bool is_permanent` — managed via `dtc_database_set_permanent(dtc_code, bool)`.
    When true, SID 0x14 preserves this DTC's status byte. Initialised to false.

- **Three new `dtc_database` API functions** (`config/dtc_database.h`):
  - `dtc_database_set_fault_counter(dtc_code, counter)` — set debounce counter.
  - `dtc_database_set_permanent(dtc_code, permanent)` — mark/unmark permanent.
  - `dtc_database_clear_non_permanent()` — clear status bytes for all non-permanent
    DTCs (the production path for SID 0x14).

- **11 new unit tests** (TC-0x19-026 through TC-036) covering both sub-functions,
  the no-availability-mask format of 0x0B, the 0xFF counter exclusion rule, and the
  0x14 ↔ permanent DTC interaction.

### Changed

- **SID 0x14 now calls `dtc_database_clear_non_permanent()`** instead of
  `dtc_database_clear_all()`. Behaviour is identical when no DTCs are permanent.
  `dtc_database_clear_all()` is retained unchanged (used by test resets).

- **`dtc_database_register()`** initialises `fault_detection_counter = 0x00` and
  `is_permanent = false` for every new entry — no YAML or codegen change required.

### Fixed

- **`uds_comm_control_init()` never called in generated init sequence** — all
  `0x28` (CommunicationControl) and `0x85` (ControlDTCSetting) requests returned
  NRC 0x22 (conditionsNotCorrect) in every generated build since launch. Root cause:
  `uds_comm_control.c` gates on `s_initialized`; `uds_comm_control_init()` was only
  called in unit tests (`test_service_0x28.c`, `test_service_0x85.c`), masking the
  gap. Fix: added Step 5.8 to `uds_init.c.j2` — `uds_comm_control_init()` with NULL
  platform callbacks (state tracking works immediately; OEM integration comment
  explains how to wire real CAN-filter and DTC-gate callbacks). All 8 examples
  regenerated. Closes [#28](https://github.com/Xaloqi/EDS/issues/28).

- **`safeboot.platform: freertos` had no effect — wrong flash ops header generated**
  — `uds_init.c.j2` hardcoded `#include "zephyr_flash_ops.h"` and
  `zephyr_flash_ops_init()` in the safeboot block, ignoring the `safeboot_platform`
  context variable that `codegen.py` already passed correctly. Announced as working
  in v1.8.0 (CHANGELOG §`safeboot.platform` key) but any FreeRTOS safeboot build
  produced a compile error (`zephyr_flash_ops.h: No such file or directory`). Fix:
  template now uses `{{ safeboot_platform }}_flash_ops.h` and
  `{{ safeboot_platform }}_flash_ops_init()`. `safeboot_ecu` (Zephyr) and
  `safeboot_freertos_ecu` (FreeRTOS) both regenerated correctly.

---
## [1.8.2] — Bug fix (closes #37)

### Fixed
- All 8 example `on_isotp_rx_complete()` callbacks now handle `pending_reset_type`
  after dispatch: flush NVM, wait 50 ms for ISO-TP TX to reach the wire, then call
  `zephyr_port_ecu_reset()` / `eds_platform_ecu_reset()`. Previously the ECU sent the
  `0x11` positive response but never reset (closes #37).

---
## [1.8.1] — Bug fixes (closes #34, #35)

### Fixed

- **`platform/nvm_store.h` — header relocated to platform-neutral directory**
  (`platform/zephyr/nvm_store.h` → `platform/nvm_store.h`, closes [#34](https://github.com/Xaloqi/EDS/issues/34)).
  `nvm_store.h` is a shared interface used by both the Zephyr and FreeRTOS
  backends. Placing it inside `platform/zephyr/` caused build failures when
  integrating `freertos_nvm.c` into a custom build system without the provided
  `CMakeLists.txt`. The file already documented its intended location
  (`FILE: platform/nvm_store.h`) — it is now there. Removes the `platform/zephyr`
  workaround include from all three FreeRTOS example `CMakeLists.txt` files.

- **`campaigns/safeboot_freertos_dfu.yaml` — OTA campaign expanded to full
  ISO 14229 production sequence** (8 steps → 15 steps, closes [#35](https://github.com/Xaloqi/EDS/issues/35)).
  Previous campaign omitted `CommunicationControl` (0x28) and `ControlDTCSetting`
  (0x85) — both required by OEM production toolchains (BMW, VAG). Added:
  - Step 4: `0x28 0x03` disableRxAndTx before download
  - Step 5: `0x85 0x02` DTCSettingOff before download
  - Steps 13–15 (post-reset): extended session → `0x85 0x01` DTCSettingOn → `0x28 0x00` enableRxAndTx
  Both services have always been fully implemented in the EDS UDS stack.

---
## [1.8.0] — FreeRTOS OTA DFU example (closes #28)

### Added

- **`examples/safeboot_freertos_ecu/`** — FreeRTOS UDS OTA DFU example on
  STM32H743ZI. FreeRTOS companion to `examples/safeboot_ecu/` (Zephyr + MCUboot).
  Demonstrates the complete 0x34 / 0x36×N / 0x37 / 0x11 firmware download pipeline
  without MCUboot dependency:

  | Layer | File |
  |---|---|
  | Flash HAL | `platform/freertos/freertos_flash_ops.c` |
  | Flash interface | `platform/freertos/freertos_flash_ops.h` |
  | FreeRTOS main | `examples/safeboot_freertos_ecu/src/main.c` |
  | TestLab campaign | `examples/safeboot_freertos_ecu/campaigns/safeboot_freertos_dfu.yaml` |

  Flash layout on STM32H743ZI (2 MB dual-bank):

  | Region | Address | Size | Purpose |
  |---|---|---|---|
  | Bank 1 | 0x08000000 | 1 MB | Running application |
  | Bank 2 — OTA | 0x08100000 | 896 KB | OTA staging area |
  | Bank 2 — NVM | 0x081E0000 | 128 KB | UDS NVM (reserved) |

- **`platform/freertos/freertos_flash_ops.h/.c`** — STM32H743ZI dual-bank flash
  ops implementing `uds_flash_ops_t` (erase / write / verify).
  - Real hardware: STM32H743 HAL (`HAL_FLASHEx_Erase` / `HAL_FLASH_Program`)
    activated when `STM32H7xx` is defined.
  - CI / QEMU: RAM stub activated automatically when `STM32H7xx` is not defined.
    Compile succeeds, CRC verification works, writes are not persistent.
  - Flash driver design based on dual-bank driver contributed by chenyurong22
    (Siemens) in [#28](https://github.com/Xaloqi/EDS/issues/28). Thank you.

- **New `diagnostics_config.yaml` key**: `safeboot.platform: freertos` causes
  codegen to generate `freertos_flash_ops_init()` at Step 5.7 of `uds_init.c`
  instead of `zephyr_flash_ops_init()`.

- **New routine RID 0xFF01 — `VerifyOTASlotIntegrity`** (replaces Zephyr-specific
  `VerifyBootloaderIntegrity` in the FreeRTOS example): reads the first 8 bytes of
  Bank 2, validates that a valid ARM Cortex-M7 vector table is present (stack pointer
  in SRAM range, Reset_Handler with Thumb bit set).

- **CI job `freertos-safeboot`** — compile-only build of `safeboot_freertos_ecu/`
  against QEMU Cortex-M4 with RAM stub flash. No STM32 HAL required in CI.

### Note on bank swap

This example writes a new image to Bank 2 and verifies CRC via 0x37.
The boot-time bank switch (Bank 2 → Bank 1) is the customer's bootloader
responsibility. The EDS **Developer tier** adds the A/B swap state machine
with metadata sector, boot flag, and N-boot rollback counter.

Reported by chenyurong22 (Siemens) in [#28](https://github.com/Xaloqi/EDS/issues/28).

---
## [1.7.4] — ISO-TP TX frame padding (closes #29)

### Added

- **`ISOTP_TX_PADDING` — configurable TX frame padding** (ISO 15765-2 Annex B).
  Opt-in compile-time flag (default off — zero behaviour change for existing
  integrations). When enabled, unused bytes in all transmitted frames are
  filled with `ISOTP_TX_PADDING_BYTE` (default `0xCC`) and DLC is extended:

  | Frame | Padding off | Padding on |
  |---|---|---|
  | Classic CAN SF | DLC = length+1 | DLC = 8, tail = 0xCC |
  | CAN FD SF | DLC = length+2 | DLC = next valid FD DLC |
  | Classic CAN FF | DLC = 8 (unchanged) | DLC = 8 (already full) |
  | CAN FD FF escape | DLC = 6+data | DLC = next valid FD DLC |
  | Classic CAN CF | DLC = bytes+1 | DLC = 8, tail = 0xCC |
  | FC | DLC = 3 | DLC = 8, bytes [3..7] = 0xCC |

  Enable in Zephyr: `CONFIG_ISOTP_TX_PADDING=y`.
  Enable in FreeRTOS / bare-metal: `-DISOTP_TX_PADDING=1`.
  See [docs/ISOTP_PADDING.md](docs/ISOTP_PADDING.md) for full reference.

- **6 new unit tests** in `test_isotp_padding` suite, all passing with
  `ISOTP_TX_PADDING=1` and `ISOTP_ENABLE_CAN_FD=1`.

- `ISOTP_TX_PADDING` and `ISOTP_TX_PADDING_BYTE` added to root `Kconfig`.

- **`sbom.json`** — CycloneDX 1.4 Software Bill of Materials at repo root.
  Lists all runtime and test dependencies (Zephyr 3.7.0, FreeRTOS Kernel,
  LwIP 2.2.0, Unity test framework) with SPDX license identifiers and PURLs.

- **DoIP feature matrix** — `docs/ARCHITECTURE.md` §6.2 now contains a
  precise 18-row ISO 13400-2:2019 feature matrix with payload type codes,
  implemented/not-implemented status, and standard clause references.

- **AI-assisted development policy** — `CONTRIBUTING.md` now documents how
  Claude Code tooling is used, human review gates, CI requirements, and the
  8 safety-critical files requiring explicit sign-off. Maintainer
  responsibility statement added. DCO sign-off added to PR checklist.

Reported by chenyurong22 in [#29](https://github.com/Xaloqi/EDS/issues/29).

---
## [1.7.3] — OTA bootloader example for nucleo_h743zi

### Added — safeboot_ecu: complete UDS DFU + MCUboot pipeline on Zephyr

Closes [#24](https://github.com/Xaloqi/EDS/issues/24). The `safeboot_ecu`
example now ships as a complete, runnable OTA update reference for the
STM32H743ZI (Nucleo-H743ZI2).

The protocol side (service handlers 0x34/0x36/0x37, `zephyr_flash_ops.c`,
transfer context, generated `uds_init.c` with `zephyr_flash_ops_init()`) was
already complete since v1.7.0. This release completes the application layer
so the example compiles and runs end-to-end.

| File | Change |
|---|---|
| `src/main.c` | MCUboot image confirmation via `boot_is_img_confirmed()` / `boot_write_img_confirmed()` on first post-swap boot; non-fatal if it fails (MCUboot rollback is the correct safety net) |
| `boards/nucleo_h743zi/nucleo_h743zi.conf` | `CONFIG_BOOTLOADER_MCUBOOT=y` + `CONFIG_MCUBOOT_IMG_MANAGER=y` (required for MCUboot image manager API) |
| `generated/did_handlers.c` | All 5 DID backing stores filled: VIN `XALQ1EDS00SFBT001`, ECU serial `SFB00001`, app SW ident `v1.0.0`, active session `0x01`, supplier `XALOQI    ` |
| `generated/routine_handlers.c` | `0xFF00 CheckProgrammingPreconditions` (PASS + result cache); `0xFF01 VerifyBootloaderIntegrity` (reads `image_0`, checks MCUboot magic `0x96f3b83d`, returns 2-byte status + sub-code) |
| `README.md` | Full guide: hardware wiring (FDCAN1 PD0/PD1 + TJA1051), flash layout, build/sign/flash, TestLab campaign YAML, manual UDS byte sequence, post-DFU DID check, ASIL-B properties |

Reported in: https://github.com/Xaloqi/EDS/issues/24

---
## [1.7.2] — CAN FD platform HAL + strict session gate

### Added — CAN FD HAL for Zephyr and FreeRTOS

Completes the CAN FD story started in v1.7.1. The ISO-TP transport layer
already handled FD frames correctly; this release wires the platform HALs
so FD frames flow through on real hardware.

All changes are guarded by `ISOTP_ENABLE_CAN_FD` — Classic CAN builds
(the default) are byte-for-byte identical: `eds_can_frame_t` stays 8 bytes,
no FD code is compiled in.

| File | Change |
|------|--------|
| `platform/platform_api.h` | `EDS_CAN_FRAME_MAX_DLEN` (8 Classic / 64 FD); `eds_can_frame_t.data[]` uses it; `is_fd` field added under `#if ISOTP_ENABLE_CAN_FD` |
| `platform/zephyr/zephyr_can.c` | TX: sets `CAN_FRAME_FDF` + `can_bytes_to_dlc()`; RX: propagates `CAN_FRAME_FDF → is_fd` + `can_dlc_to_bytes()`; init: `CAN_MODE_FD` when FD enabled |
| `platform/freertos/freertos_can.c` | TX: raises DLC limit to 64; passes `is_fd` to customer send callback; RX: propagates `is_fd` instead of hardcoding `false` |

**Zephyr integration:** add `CONFIG_CAN_FD_MODE=y` to Kconfig and compile with `-DISOTP_ENABLE_CAN_FD=1`.

Reported in: https://github.com/Xaloqi/EDS/issues/16

### Added — Strict programming session gate

OEM diagnostic toolchains (BMW, VAG) require the tester to enter **Extended
Diagnostic Session before requesting Programming Session** — a direct
Default → Programming jump is rejected. This policy is now configurable
at runtime without touching codegen or YAML.

ISO 14229-1 §7.4.2.3 does not normatively prohibit Default→Programming,
so the **default remains permissive** — zero behaviour change for existing
integrations.

New API in `core/uds_session.h`:

```c
uds_status_t uds_session_set_strict_programming(
    uds_session_ctx_t *ctx,
    bool               strict);
```

Call after `uds_session_init()` / `uds_generated_init()`:

```c
uds_session_set_strict_programming(&session_ctx, true);
/* Now: Default → Programming → UDS_STATUS_ERR_SESSION_TRANSITION  */
/* But: Default → Extended → Programming → UDS_STATUS_OK           */
```

New field `bool strict_programming` in `uds_session_ctx_t` (zero-initialised
by `uds_session_init()` → default permissive, no struct-size regression for
callers that zero-init).

4 new unit tests in `ZTEST_SUITE(test_uds_session_strict)`.

Reported in: https://github.com/Xaloqi/EDS/issues/22, https://github.com/Xaloqi/EDS/issues/23

### Fixed — cosmetic: confusing `} else\n#endif\n{` pattern in `isotp_transmit`

Replaced with an early `return UDS_STATUS_OK` in the FD branch. No logic
change; 37/37 unit tests unchanged. Reporter mistook the construct for a
missing brace.

Reported in: https://github.com/Xaloqi/EDS/issues/18

---
## [1.7.1] — CAN FD ISO-TP support (ISO 15765-2 §9.8)

### Added — CAN FD extensions to ISO-TP layer

Four missing CAN FD paths from ISO 15765-2 §9.8, implemented in the GPL runtime.
All paths are gated behind `isotp_cfg_t.use_fd = true` — existing Classic CAN
configurations are unaffected (zero-initialised struct keeps `use_fd = false`).

**API change**: `isotp_rx_complete_cb` length parameter and `isotp_transmit` length
parameter widened from `uint16_t` to `uint32_t` for consistency with FD escape
sequence ff_dl (32-bit). Internal state fields `rx_expected_len`, `rx_received_len`,
`tx_total_len`, `tx_sent_len` also widened to `uint32_t`.

| Gap | Frame type | Spec reference | Status |
|-----|------------|----------------|--------|
| SF RX escape  | `frame->is_fd && data[0]==0x00` → read `data[1]` as SF_DL (1–62 bytes) | §9.8.2 | Fixed |
| FF RX escape  | `ff_dl==0` on FD frame → parse bytes 2–5 as 32-bit big-endian FF_DL | §9.8.3 | Fixed |
| SF TX escape  | `use_fd && length ≤ 62` → emit `data[0]=0x00, data[1]=length, is_fd=true` | §9.8.2 | Fixed |
| FF TX escape  | `use_fd && length > 4095` → emit `data[0..1]=0x10 0x00`, bytes 2–5 = 32-bit FF_DL | §9.8.3 | Fixed |

New constants in `transport/isotp.h`:
- `ISOTP_ENABLE_CAN_FD (0)` — compile-time opt-in; set to 1 to enable FD paths. Default 0: Classic CAN only, no FD code in the binary.
- `ISOTP_FD_SF_MAX_PAYLOAD_LEN (62U)` — max SF payload on CAN FD (only defined when `ISOTP_ENABLE_CAN_FD=1`)
- `ISOTP_RX_BUF_LEN` — overridable compile-time RX buffer size (default: `UDS_MAX_PAYLOAD_LEN`)

Added `bool use_fd` to both `isotp_cfg_t` and `isotp_ctx_t`. The Zephyr and FreeRTOS
platform HAL bindings still target Classic CAN only; users enabling `use_fd=true`
must supply a CAN FD-capable platform binding (tracked as a follow-up).

### Added — 8 CAN FD unit tests

New `ZTEST_SUITE(test_isotp_canfd)` in `tests/unit_runnable/test_isotp.c`:

| Test | What it covers |
|------|---------------|
| `test_fd_sf_rx_10_bytes` | FD SF 10-byte payload RX |
| `test_fd_sf_rx_62_bytes` | FD SF max 62-byte payload RX |
| `test_fd_sf_rx_zero_dl` | SF_DL=0 → `UDS_STATUS_ERR_TP_FRAME_INVALID` |
| `test_fd_sf_tx_10_bytes` | FD SF TX: byte 0=0x00, byte 1=10, `is_fd=true` |
| `test_fd_ff_escape_rx_fits` | FF escape RX (ff_dl=100): RX_WAIT_CF + FC CTS sent |
| `test_fd_ff_escape_rx_overflow` | FF escape RX (ff_dl=5000 > buffer) → FC OVFLW |
| `test_fd_ff_escape_classic_can_rejected` | FF_DL=0 on Classic CAN → FRAME_INVALID |
| `test_fd_ff_escape_tx` | FF escape TX (ff_dl=5000): bytes 0–5 encoding verified |

### Changed — example callbacks

All 7 example `on_isotp_rx_complete` callbacks updated:
`uint16_t length` → `uint32_t length`; overflow guard updated from `(uint16_t)` to
`(uint32_t)UDS_MAX_PAYLOAD_LEN`. Affects `basic_ecu`, `bms_ecu`, `sensor_ecu`,
`sensor_ecu_freertos`, `ardep_ecu`, `robot_joint_controller_ecu`, `safeboot_ecu`.

Reported in: https://github.com/Xaloqi/EDS/issues/14

---
## [1.7.0] — Robustness Campaign + SOVD Bridge

### Fixed — Protocol compliance: suppress-response bit (ISO 14229-1 §7.5.3)

Four UDS services in the generated inline simulator were returning positive
responses instead of `None` when the suppress-response bit (sub-function byte
bit 7 = `0x80`) was set by the tester.

| SID  | Service            | Bug description                                              |
|------|--------------------|--------------------------------------------------------------|
| 0x11 | ECUReset           | Returned `b'\x51\xNN'` instead of `None`                    |
| 0x28 | CommunicationControl | Returned `b'\x68\xNN'` instead of `None`                  |
| 0x31 | RoutineControl     | Also masked sub_fn incorrectly (`pdu[1]` not `pdu[1] & 0x7F`), causing NRC 0x12 on valid suppress-response requests |
| 0x85 | ControlDTCSetting  | Returned `b'\xC5\xNN'` instead of `None`                    |

Fix applied to all 11 ECU `generated/tests/conftest.py` files and to the
Jinja2 template (`tools/templates/conftest.py.j2`) that generates them:

- `examples/basic_ecu/generated/tests/conftest.py`
- `examples/basic_ecu_doip/generated/tests/conftest.py`
- `examples/basic_ecu_doip_freertos/generated/tests/conftest.py`
- `examples/basic_ecu_freertos/generated/tests/conftest.py`
- `examples/bms_ecu/generated/tests/conftest.py`
- `examples/motor_controller_ecu/generated/tests/conftest.py`
- `examples/ardep_ecu/generated/tests/conftest.py`
- `examples/robot_joint_controller_ecu/generated/tests/conftest.py`
- `examples/safeboot_ecu/generated/tests/conftest.py`
- `examples/sensor_ecu/generated/tests/conftest.py`
- `examples/sensor_ecu_freertos/generated/tests/conftest.py`

### Added — 439-test robustness campaign (Phases A–L)

Complete protocol conformance and simulator fidelity campaign across 12 phases.
All phases run in `--can-interface=simulator` mode — no hardware required.
Total: **439 tests** in `examples/basic_ecu/generated/tests/`.

| Phase | File                              | Tests | What it validates |
|-------|-----------------------------------|-------|-------------------|
| A     | `test_robustness_A_codegen.py`    | 22    | Generated file presence, C marker correctness, test file presence, GCC syntax |
| B     | `test_robustness_B_protocol.py`   | 42    | Session transitions, TesterPresent, ECUReset, all 14 service NRCs |
| C     | `test_robustness_C_security.py`   | 21    | CMAC SecurityAccess unlock/lockout, replay, all session levels |
| D     | `test_robustness_D_customer_journey.py` | 30 | Full customer workflow (fresh YAML → codegen → pytest), all 11 ECU examples |
| E     | `test_robustness_E_data_integrity.py` | 35 | DID read/write data integrity, DTC lifecycle, session isolation |
| F     | `test_robustness_F_codegen_limits.py` | 54 | Max DID/DTC/routine counts, GCC syntax gate for all 11 ECU C files |
| G     | `test_robustness_G_resilience.py` | 47    | Malformed PDU handling, CMAC end-to-end, suppress-response bit (all 6 services), YAML ↔ simulator metadata consistency |
| H     | `test_robustness_H_protocol_precision.py` | 41 | DSC timing byte precision (P2=25ms=0x0019, P2\*=5000ms=0x1388), multi-DID RDBI batching, DTC record format (3-byte code + 1-byte status), routine lifecycle |
| I     | `test_robustness_I_nrc_wdbi_sa.py` | 34   | NRC format/SID echo for every service, WDBI check ordering (session→security→length), SecurityAccess level isolation (lockout, mismatch, independent state) |
| J     | `test_robustness_J_sovd_cda.py`    | 43   | SOVD CDA semantic fidelity: top-level structure, DID/DTC/routine counts and field values, hex normalisation, semantic session names, DoIP fields present/absent, idempotency |
| K     | `test_robustness_K_error_quality.py` | 35 | Codegen error message quality: every bad YAML exits non-zero with an actionable keyword (field name, hex value, or standard reference) in stderr |
| L     | `test_robustness_L_codegen_output_fidelity.py` | 35 | Codegen output fidelity: generated C files contain correct DID decimal IDs, data lengths, access flags, timing constants, DTC severity bytes, and routine support flags |

**Phase G** (`test_robustness_G_resilience.py`, 47 tests):
- `TestMalformedPDUResilience` (13): empty PDU → NRC 0x13, unknown SID → NRC 0x11,
  truncated PDU per service, 256-SID fuzz
- `TestEndToEndCMACFlow` (11): requestSeed → CMAC → sendKey round-trip, wrong key →
  NRC 0x35, 3-failure lockout → NRC 0x36, seed randomness, L1+L2 independent
- `TestSuppressResponseBit` (11): all 6 services with sub-function, verifies `None`
  response when bit 7 set (exposed the 4 compliance bugs above)
- `TestYAMLSimulatorConsistency` (12): parse `diagnostics_config.yaml` and verify
  `_dids_meta` and `_routines_meta` in the inline simulator match exactly

**Phase H** (`test_robustness_H_protocol_precision.py`, 41 tests):
- `TestDSCTimingPrecision` (8): P2/P2\* big-endian byte values exact across all
  3 session types; re-entry timing unchanged
- `TestMultiDIDReadByIdentifier` (11): 2/3/4/5 DID batch read, echo order, NRC on
  unknown DID in batch, WDBI→RDBI round-trip, session-gated DID
- `TestReadDTCPrecision` (11): sub 0x01 layout (6 bytes, count field), sub 0x02
  4-byte record format, DTC code byte order (`0xC00100` → `[0xC0, 0x01, 0x00]`)
- `TestRoutineControlLifecycle` (11): stop-before-start → NRC 0x22,
  results-before-start → NRC 0x22, security-gated, programming session, re-start

**Phase I** (`test_robustness_I_nrc_wdbi_sa.py`, 34 tests):
- `TestNRCFormatAndSIDEcho` (13): for every service, NRC response is exactly 3 bytes
  `[0x7F, requestSID, NRC_code]` — verifies SID echo and length (ISO 14229-1 §7.5.2)
- `TestWDBICompleteness` (12): correct WDBI succeeds, 1-byte short/long/zero → NRC 0x13,
  read-only DID → NRC 0x31, unknown DID → NRC 0x31; check ordering: session gate fires
  before security gate, security gate fires before length gate
- `TestSecurityAccessProtocolEdges` (9): level mismatch (seed L1, key L2) → NRC 0x24,
  pending seed cleared after successful unlock, 1 wrong key then correct unlocks,
  L1 lockout does not block L2, L1 unlock does not grant L2

**Phase J** (`test_robustness_J_sovd_cda.py`, 43 tests):
- `TestCDAStructure` (8): required top-level keys, sovdVersion=1.0.0, generatedBy mentions
  Xaloqi, generatedAt non-empty, ecuIdentification name/version match YAML, JSON round-trip
- `TestCDADIDFidelity` (10): count, hex-normalised IDs, names, dataLengthBytes, access
  lists, semantic minSession strings, None writeSecurityLevel for read-only DIDs,
  int writeSecurityLevel for read-write DIDs, readSecurityLevel match
- `TestCDADTCFidelity` (6): count, hex codes, descriptions, valid severity set, severity match
- `TestCDARoutineFidelity` (7): count, hex IDs, names, semantic sessions, securityLevel,
  supportedSubFunctions match YAML support list
- `TestCDATransportAndDoIP` (12): CAN → ISO-TP, no logicalAddress/port for CAN; DoIP →
  DoIP protocol, port=13400, logicalAddress=0xE400, sourceAddress present; transport=both →
  DoIP; 14 diagnosticServices each with sid+name; two-call idempotency

**Phase K** (`test_robustness_K_error_quality.py`, 35 tests):
- `TestSanity` (2): valid YAML exits 0 and prints dry-run-complete message
- `TestMetadataErrors` (5): missing metadata section, missing ecu_name, missing version,
  schema_version mismatch (99 → rejected with "schema_version" keyword),
  schema_version wrong type ("one" → rejected with "schema_version" keyword)
- `TestDIDErrors` (13): id wrong hex length, invalid hex chars, no 0x prefix, duplicate DID
  (asserts "already declared"), reserved id 0x0000, missing name, missing access,
  invalid access value ("execute"), unknown min_session ("factory"),
  read_security_level out of range (300 → asserts "255"), data_length zero,
  data_length over max (9999 → asserts "4095"), write DID missing data_length
  (asserts "REQ-SAFE-006")
- `TestDTCErrors` (5): code wrong hex length, missing 0x prefix, invalid chars,
  missing code field, duplicate DTC codes (asserts "already declared")
- `TestTimingErrors` (3): p2 > p2_star, p2 zero, timing wrong type ("fast")
- `TestRoutineErrors` (3): routine id wrong hex length, invalid chars, duplicate routine ids
- `TestParseErrors` (3): unclosed bracket YAML, empty file, non-mapping root (list)

Also added to `codegen.py` `validate_config()`:
- `schema_version` validation: must be an integer equal to `SUPPORTED_SCHEMA_VERSION` (1).
  Non-integer or unsupported version exits with actionable error naming the field.
- Write-DID safety gate (REQ-SAFE-006): writable DIDs without an explicit `data_length`
  are now rejected with a message referencing REQ-SAFE-006 and the 0x2E handler.

**Phase L** (`test_robustness_L_codegen_output_fidelity.py`, 35 tests):
- `TestSanity` (2): codegen exits 0, all expected files generated
- `TestGeneratedConfigH` (8): p2/p2*/s3 timing values, DID/DTC counts, ECU name,
  version, and include guard all match the controlled test YAML
- `TestDIDHandlersH` (5): handler count macro (2U), read declarations for both DIDs,
  write declaration for read-write DID, no spurious write declaration for read-only DID
- `TestDIDHandlersC` (10): 0xF190→61840U, 0xF187→61831U, data_length 17U/11U,
  DID_ACCESS_READ|WRITE flag for F187, exactly one WRITE flag in file, session constants,
  write_cb function name set for F187, write_cb = NULL exactly once (F190)
- `TestDTCUDSInit` (5): 0xC00100→12583168UL, severity 0x20U, description text,
  exactly one `dtc_database_register` call site, no 0x40U (maintenance_only absent)
- `TestRoutineHandlersC` (5): 0xFF00/0xFF01 present, ROUTINE_SUPPORT_START|RESULTS
  for FF00, UDS_SESSION_PROGRAMMING for FF01, no results stub for start-only routine

### Changed — CI

`.github/workflows/ci.yml` `robustness-tests` job updated:
- Phase count: 6 phases / 245 tests → 12 phases / 439 tests
- Added individual `pytest` steps for Phases G, H, I, J, K, L with short descriptions
- Final assertion: `439 passed`
- Phase comments updated with per-phase test counts A(22) B(42) C(21) D(30) E(35)
  F(54) G(47) H(41) I(34) J(43) K(35) L(35)

`.github/workflows/ci.yml` `integration-tests` job: added `--ignore` flags for
`test_robustness_G_resilience.py`, `test_robustness_H_protocol_precision.py`,
`test_robustness_I_nrc_wdbi_sa.py`, `test_robustness_J_sovd_cda.py`,
`test_robustness_K_error_quality.py`, `test_robustness_L_codegen_output_fidelity.py`
(they run in the dedicated `robustness-tests` job).

`test_robustness_D_customer_journey.py` `TestAllECUExamplesPytest`: added `--ignore`
for Phases G, H, I, J, K, L to prevent recursive collection when running all 11 ECU examples.

### Added — SOVD CDA codegen output

`tools/codegen.py --sovd`: new optional flag that generates a valid OpenSOVD 1.0
Capability Description and Advertisement (CDA) JSON file (`sovd_cda.json`) alongside
the standard C output. Pure-Python implementation — no Jinja2 template required.
`build_sovd_cda(cfg)` builds the CDA dict directly; `render_sovd_cda()` writes it
with `json.dumps(indent=2)`.

The CDA captures the full ECU diagnostic profile from `diagnostics_config.yaml`:

- All configured DIDs with `id`, `name`, `dataLengthBytes`, `access`, `minSession`,
  `readSecurityLevel`, `writeSecurityLevel`
- All configured DTCs with `code`, `description`, `severity`
- All configured routines with `id`, `name`, `minSession`, `securityLevel`,
  `supportedSubFunctions`
- Static list of all 14 EDS UDS services (`diagnosticServices`)
- `transportInfo.protocol`: `"DoIP"` or `"ISO-TP"` derived from `ecu.transport`
- `ecuIdentification.logicalAddress`, `ecuIdentification.sourceAddress`,
  `transportInfo.port`: present only when `transport` is `doip` or `both`

Session names use semantic strings (`"default"`, `"extended"`, `"programming"`) —
not internal C constants — so the JSON is directly readable by SOVD clients and
Eclipse SDV tooling.

### Changed — CI

`.github/workflows/ci.yml`: added `sovd-codegen` job (job 14 of 14). Imports
`build_sovd_cda` and `load_config` directly from `tools/codegen.py` — no template
checkout required. Validates CDA structure, transport protocol, DID/DTC/routine
counts, and presence/absence of `logicalAddress` for CAN vs DoIP ECUs.

### Fixed — Pre-launch audit (v1.7.0 patch)

- `tools/codegen.py`: added `__version__ = "1.7.0"` module-level constant.
  Banner previously printed "Phase 4" (internal development label); now prints
  `"Xaloqi EDS — Code Generator v1.7.0"` using the constant.
- `tools/codegen.py` `build_sovd_cda()`: `generatedBy` field corrected from
  `"Xaloqi EDS codegen v1.6.0"` to `"Xaloqi EDS codegen v1.7.0"`.
- `INSTALL.md`: template count corrected 14 → 17; expected codegen output block
  replaced with the actual `[1/5]…[5/5]` step format.
- `tools/testlab.py` (`__version__`): synced from `"1.2.0"` to `"1.4.0"` to match
  the current TestLab product version.

---

## [1.6.0] — DoIP (ISO 13400-2) transport

### Added — DoIP server for Zephyr and FreeRTOS

transport/doip/doip_server.h + doip_server.c: ISO 13400-2 ECU server.
Platform-agnostic core via eds_doip_platform_ops_t. No malloc, no recursion,
static buffers. Covers: Routing Activation (Default type), DiagnosticMessage
dispatch → uds_server_process_request(), Positive/Negative Ack, Alive Check.
Symmetric with xaloqi-tester DoipBus (TestLab v1.1.0) — byte-for-byte frame
format compatibility.

transport/doip/zephyr_lwip.h + zephyr_lwip.c: Zephyr BSD-socket binding.
Implements eds_doip_platform_ops_t via zsock_*. K_THREAD_DEFINE creates the
DoIP server thread automatically at startup.

transport/doip/freertos_lwip.h + freertos_lwip.c: FreeRTOS + LwIP binding.
Implements eds_doip_platform_ops_t via lwip_socket API. xTaskCreate() creates
the DoIP server task; configurable stack size and priority.

platform/zephyr/platform_doip.h + platform_doip.c: Zephyr DoIP registration
shim. Exposes eds_doip_platform_start() for application main.c.

platform/freertos/platform_doip.h + platform_doip.c: FreeRTOS DoIP registration
shim. Exposes eds_doip_platform_start_freertos() for application main.c.

examples/basic_ecu_doip/: New Zephyr example ECU — same 5 DIDs / 2 DTCs /
3 routines as basic_ecu, served over DoIP on native_sim (loopback networking).
EDS_DOIP_ONLY_BUILD=1 disables ISO-TP init; uds_generated_init(NULL, 0, 0).

examples/basic_ecu_doip_freertos/: New FreeRTOS + LwIP example ECU — same
schema, FreeRTOS platform, LwIP TCP. LwIP stub headers for CI compile testing.

tests/unit_runnable/test_doip_server.c: 24 host-side Unity tests covering
header encode/decode, routing activation, alive check, diagnostic message
dispatch, NACK generation, boundary conditions, and NULL-pointer guards.

tests/test_doip_integration.py: 10 pytest end-to-end integration tests
(skipped automatically when xaloqi-tester not installed).

### Changed — CI

.github/workflows/ci.yml: Added doip-integration job (native_sim build +
unit tests smoke check + pytest integration tests with graceful skip on
missing xaloqi-tester). Exit code 5 (all tests skipped) treated as success.

misra_analysis.py: DEV-GOTO-01 (Rule 15.1 — goto in connection teardown),
DEV-FD-01 (Rule 11.6 — void*/int fd cast, same pattern Zephyr and FreeRTOS
bindings), plus extensions to DEV-MULT-01, DEV-PREC-01, DEV-CAST-02,
DEV-MCRO-01, DEV-ACCS-01, DEV-LOOP-01, DEV-GEN-01, DEV-TYPE-03 for all
new transport/doip and platform/freertos/platform_doip files.

### Schema change — diagnostics_config.yaml (additive, backward-compatible)

Optional ecu.transport field: "can" (default), "doip", or "both".
Optional ecu.doip block: logical_address, source_address, port.
Existing configs without these fields continue to build unchanged.

---

##  [1.5.0] — TestLab integration + testgen refactor
### Added — testlab_config.yaml standalone mode (TestLab)

xaloqi/tester/_config.py: full input validation with precise error messages
for every bad input — CAN ID out of range, unknown session names, negative
data_length, invalid DTC severity, missing required DID/DTC fields.
Error messages include field name and array index (dids[1]: 'min_session' must be one of ...).
testlab_config.yaml: documented template file at the TestLab repo root.
Copy-paste starting point for customers not using Xaloqi EDS.
campaigns/standalone_validation.yaml: four ready-to-run campaign jobs
(basic_validation, eol_production_check, security_audit,
regression_check) for non-EDS users.
load_testlab_config() and load_eds_config() now cross-reject each other
with a clear error when the wrong format is passed.
runner.py --config help text updated to mention both EDS YAML and
standalone testlab_config.yaml formats.

### Changed — testgen.py conftest refactor (EDS-toolchain + EDS)

tools/templates/conftest.py.j2 reduced from 870 lines to 456 lines.
Inline ISO-TP framing (300 lines), AES-128 S-box + CMAC (200 lines), and
ECU simulator (300+ lines) replaced by xaloqi-tester imports:
UdsTester.raw_request(), aes_cmac(), derive_key().
Public API of the generated conftest.py is unchanged — all test files
(test_did_*.py, test_services.py, test_routine_*.py) work without
modification after the template update.
All generated conftest.py files in all 7 specialist examples regenerated.
requirements_testgen.txt now lists xaloqi-tester>=1.0.0.
Bug fixes in ISO-TP or AES-CMAC applied to xaloqi-tester now propagate
automatically to all generated test suites — no more diverging inline copies.

### Added — PCAN/Kvaser hardware backends (TestLab)

xaloqi/tester/transport/hardware.py: HardwareBus, PcanBus, KvaserBus.
Wraps any python-can >= 4.0 adapter by bustype string. PCAN and Kvaser are
named convenience subclasses with driver-specific error messages and
troubleshooting hints.
Supports all python-can hardware: PCAN USB/PCIe, Kvaser USB, IXXAT,
Vector CANalyzer/CANoe, SLCAN, and bustype="auto" for auto-detection.
PcanBus("PCAN_USBBUS1") and KvaserBus(0) pass directly as the
interface argument to UdsTester.
tests/test_hardware.py: 25 unit tests (fully mocked, plus
@pytest.mark.hardware markers for tests requiring physical adapters).
tests/conftest.py: --hardware CLI flag registers
@pytest.mark.hardware skip logic. Hardware tests are excluded from CI
automatically and re-enabled with pytest --hardware.

### Added — production audit fixes (TestLab)

License enforcement in UdsTester.__aenter__() now actually executes —
previously a pass stub. Raises LicenseError with purchase URL when no
key is found.
Bare assert in seven service methods replaced with TransportError —
AssertionError is suppressed by Python's -O flag and gives no diagnostic.
SocketCanBus.__aenter__() / __aexit__() added — sim.py used
async with SocketCanBus(...) which would crash without these.
Dead isotp_recv() / isotp_send() functions removed from docker/ecu_sim/sim.py.
SPDX headers added to all 16 source files in xaloqi/, tools/, docker/.
xaloqi/__version__ = "1.0.0" added.
LICENSE_COMMERCIAL.txt created.
[project.urls] added to pyproject.toml.

## [1.4.0] — Job Engine + CI expansion

### Added — Job Engine (IDEA-032)

- `tools/jobrunner.py` — executes YAML-defined multi-step UDS workflow jobs
  against a real ECU (SocketCAN) or simulated ECU (harness binary). The same
  job definition runs identically in CLI, pytest, CI pipeline, and AI agent.
- `jobs:` top-level block in `diagnostics_config.yaml` — optional, backward
  compatible. Existing configs without `jobs:` continue to work unchanged.
- 15 action types: `session`, `security_access`, `read_did`, `write_did`,
  `read_dtc`, `clear_dtc`, `routine`, `foreach_did`, `assert`, `ecu_reset`,
  `tester_present`, `delay`, `request_download`, `transfer_data`,
  `request_transfer_exit`.
- Variable interpolation: `save_as` stores response bytes; `${name}` references
  them in subsequent steps. Used for firmware size in flash workflows.
- JSON output (`--json`): structured result file with `schema_version: 1`.
  Stable interface contract for CI reporting and TestLab AI (roadmap).
- `tools/job_library/` — 5 pre-built job templates: `flash_and_verify`,
  `eol_production_check`, `field_diagnostic_read`, `calibration_sequence`,
  `security_lockout_reset`.
- `tools/config_parser.py`: `jobs:` block is now validated structurally
  (unknown actions, missing `steps`, invalid `on_failure` values).

### Added — sensor_ecu example

- `examples/sensor_ecu/generated/` — all C/H generated files now committed
  (`uds_init.c`, `did_handlers.c`, `did_safety_wrappers.c`, `safety_config.h`,
  `generated_config.h` + full test suite).
- `examples/sensor_ecu/diagnostics_config.yaml` — 5 working Job Engine jobs:
  `field_diagnostic_read`, `sensor_health_check`, `calibration_reset`,
  `calibration_write`, `dtc_clear_and_verify`.

### Added — CI

- EDS-toolchain CI expanded from 7 to 16 jobs.
- 7 new example validation jobs (one per specialist example): YAML schema,
  generated file presence, safety markers (`uds_safety_self_test`,
  `keys_are_placeholder`), DID count verification, test file presence.
- `gui-build` job: TypeScript typecheck + Vite production build.
- `validate-harness` job: Python tester import validation + `derive_key` smoke test.
- `validate-jobrunner` job: dry-run all example configs + job library schema
  validation + 43 unit tests (mock UdsTester, no hardware required).

### Added — scripts

- `scripts/verify_did_counts.py` added to EDS-toolchain Developer ZIP.
  Previously only in the public EDS repo.

### Fixed — GUI

- `gui/package-lock.json` regenerated with full sha512 integrity hashes.
  Previous lockfile was missing hashes for 48/49 packages, causing `npm ci`
  to install incomplete packages and fail at runtime.
- `gui/package.json`: added `react-refresh@0.14.0` as explicit devDependency
  (peer dep of `@vitejs/plugin-react@4.2.1`).

### Documentation

- `INSTALL.md` — Job Engine section with full CLI reference and job template table.
- `docs/INTEGRATION_GUIDE.md` — Section 6: Job Engine full reference (actions
  table, variable interpolation, CLI examples, JSON schema).
- `docs/AI_CONTEXT.md` — `jobs:` YAML schema with all 15 actions documented;
  `jobrunner.py` and `job_library/` in repo structure.
- All docs bumped to v1.4.0.

---

## [1.3.0] — Platform housekeeping + FreeRTOS API

### Fixed — Platform structure

- Removed 15 duplicate files from `platform/` root — all were byte-for-byte
  identical to their `platform/zephyr/` counterparts. CMakeLists already
  compiled from `platform/zephyr/`; root copies were dead code.
- Moved `transport/zephyr_can.c/.h` to `platform/zephyr/` — Zephyr-specific
  CAN driver belongs in the platform layer, not the RTOS-agnostic transport layer.
- Removed stale `transport/zephyr_port.c/.h` — canonical copy already in
  `platform/zephyr/` with updated include guard and missing declaration added.
- Updated `CMakeLists.txt` to compile `zephyr_can.c` from `platform/zephyr/`.
- Removed `scripts/sync_shadow_copies.sh` — stub with no function.

### Fixed — Stack safety

- `core/uds_types.h`: added `_Static_assert` that fires at compile time if
  `sizeof(uds_msg_buf_t)` exceeds `EDS_MSG_BUF_MAX_STACK_BYTES` (default 256).
  Catches accidental stack allocation of the 4097-byte message buffer on embedded
  targets. Suppress with `-DEDS_MSG_BUF_MAX_STACK_BYTES=8192` for host/sim builds.
- `core/uds_safety.c`: replaced two stack-allocated `uds_msg_buf_t` instances in
  `uds_safety_self_test()` with module-level statics (`s_self_test_req_a/b`).
  Previous code allocated ~8194 bytes on the stack — more than typical task stacks.

### Fixed — Security

- `core/uds_security.c`: `[SEC-ENTROPY-01]` — `uds_security_request_seed()` now
  rejects all-zero seeds with `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE`. Prevents
  silent security failure from uninitialised TRNG peripherals.
- `platform/freertos/freertos_platform_api.c`: removed `vTaskDelay(2)` before
  NVIC reset. Replaced with TX CONFIRMATION CONTRACT comment specifying the
  correct caller sequence: `isotp_transmit()` → poll until TX IDLE →
  `eds_platform_nvm_flush()` → `eds_platform_ecu_reset()`.

### Added — FreeRTOS integration API

- `platform/freertos/freertos_platform_api.c` + `platform/platform_api.h`:
  new `eds_freertos_start()` API encapsulates the UDS poll task creation, ISO-TP
  RX callback, and static task storage. FreeRTOS integrators now call four
  functions instead of copying 80 lines of boilerplate.
- `examples/basic_ecu_freertos/src/main.c`: simplified using `eds_freertos_start()`.
  This example is now the canonical FreeRTOS integration reference.

### Added — Tests

- `tests/unit_runnable/test_isotp_concurrent.c`: 6 new test cases covering
  ISO-TP concurrent request scenarios — SF interrupting multi-frame, new FF
  restarting reassembly, CF-without-FF, wrong SN recovery, N_Cr timeout recovery.

### Added — Tooling and validation

- `tools/config_parser.py`: `schema_version` field validation — missing field
  emits a deprecation warning; version mismatch is a hard error.
- `data_length` now required for all write-capable DIDs (REQ-SAFE-006 enforcement).

### Added — Documentation

- `docs/SECURITY_NOTICE.md`: FreeRTOS seed entropy requirements with STM32/NXP
  TRNG code examples and ISO 26262 / UNECE WP.29 references.
- `docs/INTEGRATION_GUIDE.md`: section 4 rewritten with full Five-Step FreeRTOS
  integration using `eds_freertos_start()`; ECU reset TX confirmation contract.
- `platform/platform_api.h`: mutex interface split documented with rationale.

---

## [1.3.0] — SafeBoot + Sensor + Robotics Examples

**Status: All 16 CI jobs green. Three new examples. SafeBoot codegen automation complete.**

### Added — SafeBoot (MCUboot DFU over UDS)

- `safeboot:` YAML block in `diagnostics_config.yaml`. Set `enabled: true` to
  generate `zephyr_flash_ops_init()` automatically into `uds_init.c`. No manual
  flash ops registration required in application code.

- `tools/codegen.py` — `build_uds_init_context()` reads `safeboot.enabled`,
  `safeboot.platform`, `safeboot.max_block_length` and passes them to the template.

- `tools/templates/uds_init.c.j2` — Step 5.7 now generates conditionally:
  - `safeboot.enabled: true` → `#include "zephyr_flash_ops.h"` + `zephyr_flash_ops_init()`
    call with full REQ-FLASH-001/002/003 safety comments.
  - `safeboot.enabled: false` (default) → documentation comment explaining how to
    enable, no code generated. Existing behaviour fully preserved.

- `tools/config_parser.py` — `safeboot:` block added to `CONFIG_SCHEMA`.

- `examples/safeboot_ecu/` — complete MCUboot DFU example targeting
  STM32 Nucleo-H743ZI2. Includes 7-step DFU sequence documentation and
  `dfu_flash.py` Python script using `udsoncan`.

- `.github/workflows/ci.yml` — `safeboot-example` job (job 15 of 16):
  verifies `zephyr_flash_ops_init()` is generated when enabled, and that
  `basic_ecu` (disabled) does not regress.

### Added — SensorECU example (IDEA-036/037)

- `examples/sensor_ecu/` — zone controller demonstrating the complete
  sensor → DID → DTC pattern using the Zephyr sensor API.
  Temperature (DID 0xD001) and supply voltage (DID 0xD002) read via
  `sensor_sample_fetch()` / `sensor_channel_get()` every 100 ms.
  DTCs 0xD00101/102 (over/under temperature) and 0xD00201/202
  (over/under voltage) set and cleared automatically by `sensor_monitor.c`.
  Writable calibration thresholds via DID 0xD010/0xD011.

- `examples/sensor_ecu/src/sensor_monitor.c` — dedicated 100 ms monitoring
  thread. Calls `dtc_database_set_status()` on threshold violations.
  DID handlers read from a mutex-protected `sensor_state_t` cache —
  never block the 1 ms UDS poll loop.

- `.github/workflows/ci.yml` — `sensor-example` job (job 13 of 16).

### Added — Robot Joint Controller example (IDEA-021)

- `examples/robot_joint_controller_ecu/` — joint controller ECU for a
  single-axis servo robot. 10 DIDs (position, velocity, torque, temperature,
  status, calibration limits), 5 DTCs (over-temperature, over-current,
  encoder loss, soft limit exceedances), 3 routines (home axis, apply
  calibration, clear fault history).

- README written for robotics engineers: explains why UDS is used in robotics
  (standard toolchain, ISO-TP handles multi-byte payloads, DTC persistence),
  includes Python `udsoncan` snippet for reading live joint state.

- `.github/workflows/ci.yml` — `robotics-example` job (job 14 of 16).

### Changed — CI

- CI now has 16 jobs (was 13 at v1.2.0). New jobs: `sensor-example`,
  `robotics-example`, `safeboot-example`.

- CI header updated with full 16-job index.

---

## [1.2.0] — FreeRTOS Platform Support

**Status: All 13 CI jobs green. FreeRTOS HAL complete. Zephyr builds unaffected.**

### Added — FreeRTOS platform HAL

- `platform/platform_api.h` — platform-neutral interface implemented by both HALs.
  Declares `eds_platform_ecu_reset()`, `eds_platform_nvm_flush()`,
  `eds_platform_uptime_ms()`, `eds_platform_init()`, `eds_platform_can_input()`,
  and the `eds_can_frame_t` / `eds_nvm_ops_t` / `eds_platform_cfg_t` types.

- `platform/freertos/freertos_platform_api.c` — implements `platform_api.h` for
  FreeRTOS. Customer provides a `can_send` callback and optional NVM ops via
  `eds_platform_init()`. Built-in RAM NVM stub used when no flash driver is provided
  (development / CI). ECU reset via direct SCB AIRCR write (all Cortex-M variants).
  Optional customer reset hook via `eds_platform_set_reset_cb()`.

- `platform/freertos/freertos_can.c/.h` — implements `can_transport_ops_t` over a
  static `xQueueCreateStatic` RX queue (8 frames, no heap). `freertos_can_input()`
  is ISR-safe via `xQueueSendFromISR`. `freertos_can_set_bus_off()` called from
  customer CAN error interrupt. Full `eds_can_frame_t` ↔ `uds_can_frame_t` conversion.

- `platform/freertos/freertos_nvm.c` — implements `nvm_store_*` API routing through
  customer-provided `eds_nvm_ops_t` callbacks. Schema version check and migration on
  init. Guarded by `EDS_PLATFORM_FREERTOS` so it never compiles into Zephyr builds.

- `examples/basic_ecu_freertos/` — FreeRTOS example using stub CAN loopback for CI.
  Same `diagnostics_config.yaml` as `examples/basic_ecu/` — proves same YAML generates
  working firmware on both platforms. Includes `boards/qemu_cortex_m4/FreeRTOSConfig.h`
  and linker script targeting QEMU `mps2-an386`.

- `cmake/toolchain/arm-none-eabi.cmake` — CMake toolchain file for bare-metal
  ARM cross-compilation. Sets `CMAKE_SYSTEM_NAME=Generic`,
  `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` (skips linker probe that fails without
  `crt0.o`), and `CMAKE_SYSROOT=/usr/lib/arm-none-eabi` (resolves newlib headers from
  Ubuntu `libnewlib-arm-none-eabi` package). Auto-detects `arm-none-eabi-gcc` or
  `arm-zephyr-eabi-gcc`.

- `.github/workflows/ci.yml` — `freertos-qemu` job (job 13 of 13). Clones
  `FreeRTOS-Kernel`, runs codegen, cross-compiles with `arm-none-eabi-gcc` for QEMU
  Cortex-M4, verifies ELF exists and text segment is non-zero, uploads artifact.

### Changed — platform directory restructured

- All Zephyr-specific platform files moved from `platform/` root into `platform/zephyr/`.
  The `platform/` root now contains only `platform_api.h` (shared interface) and
  `uds_flash_ops.c/.h` (platform-independent flash ops registration).

- `platform/zephyr/zephyr_platform_api.c` added — implements `eds_platform_*` for
  Zephyr using `k_uptime_get_32()`, `sys_reboot()`, and the existing NVM flush logic
  from `zephyr_port.c`.

- Root `CMakeLists.txt` and all example `CMakeLists.txt` files updated: `EDS_PLATFORM`
  CMake variable selects `zephyr` (default, unchanged behaviour) or `freertos`.

- `tests/mocks/zephyr_port_mock.c` — updated includes and added `eds_platform_*` stubs.

- `build_tests.sh` — `platform/zephyr/` added to include path; `nvm_store_mock.c`
  path updated from `platform/nvm_store_mock.c` to `platform/zephyr/nvm_store_mock.c`.

### Changed — CI

- `tests/CMakeLists.txt` — `DIAG_PLATFORM_ZEPHYR` variable added; `platform/zephyr/`
  added to include dirs; all source paths updated to `platform/zephyr/` prefix.

- `core/uds_services/service_0x11.c` — removed `#include "zephyr_port.h"` (service
  only sets `ctx->pending_reset_type`; never called platform functions directly).

---

## [1.1.0] — Commercial Readiness

**Status: Pre-release hardening. All CI jobs green. Codebase is feature-complete for v1.1.x
evaluation builds. Remaining open items are licensing, hardware validation, and community
publishing — none block technical evaluation.**

### Added — CI safety assertions (HIGH-1 / CRIT-2)
- `.github/workflows/ci.yml` — new step **"Assert ASIL_B_REQUIRE_WRITE_SECURITY is True
  (HIGH-1)"** in the `unit-tests` job. A flip of the flag to `False` in `tools/codegen.py`
  now fails CI immediately rather than silently downgrading write-security enforcement from
  a fatal codegen error to an advisory warning.
  Traceability: HIGH-1 / ISO 26262-6 / ASIL-B write-access policy.
- `.github/workflows/ci.yml` — new steps **"Assert safety invariants in ARDEP / BMS / MC
  generated output (CRIT-2 / HIGH-1)"** in the `ardep-example`, `bms-example`, and
  `mc-example` jobs. Each independently verifies: `uds_safety_self_test()` present,
  abort-on-failure guard (`return self_test_rc`) present, and `ASIL_B_REQUIRE_WRITE_SECURITY`
  intact — applied to that job's freshly-regenerated `uds_init.c`.
  Previously the self-test assertion only covered `basic_ecu` in the `unit-tests` job.
  Traceability: REQ-SAFE-SELFTEST-01 / ISO 26262-6 §9.4.3.

### Added — Repo & documentation
- `SECURITY.md` (repo root) — canonical security policy; GitHub surfaces the
  "Report a vulnerability" button when this file is present at root.
- `CONTRIBUTING.md` (repo root) — canonical contribution guide; explains dual-license
  CLA requirement, coding conventions, PR checklist, and what is / is not accepted.
- `docs/SECURITY.md` and `docs/CONTRIBUTING.md` replaced with thin redirect stubs
  pointing to the canonical root files.
- `README.md` rewritten as a product page: problem-first structure, 60-second walkthrough
  with realistic YAML, CI-verified safety properties called out explicitly, placeholder
  key warning surfaced, links to `docs/` instead of duplicating content.

### Removed
- `tests/legacy_unit/` deleted. The 15 modules it contained are a strict subset of the
  canonical 35-module set in `tests/unit_runnable/`. Both `build_tests.sh` and
  `tests/CMakeLists.txt` have always referenced `unit_runnable/` — the archived copy
  was purely confusing. (M2)
- Binary artifacts (M1) and shadow source copies in `project/src/` (M3) were already
  removed in a prior phase; confirmed absent.

### Changed
- `tests/CMakeLists.txt` — M2 annotation updated to reflect `legacy_unit/` removed
  (not merely archived).
- `docs/PHASE1_SECURITY_CHANGES.md` — M2 row updated to final resolution status.
- `.github/workflows/ci.yml` — `[HIGH-1-CI]` and `[CRIT-2-CI]` entries added to the
  FIXES header block.

### Notes
- All licensing decisions (D1–D10) resolved. GPL v2 runtime, commercial toolchain.

## [1.1.0] — Layer 4 + Layer 5 Complete

**Status: All v1.0.0 tests still passing. New CAPL generation and VS Code extension added.**

### Added — CANoe CAPL Test Generation (`--capl` flag in `testgen.py`)

- `tools/testgen.py` v1.1.0: new `--capl` and `--capl-only` CLI flags
- `tools/templates/ecu_diagnostics_test_suite.can.j2` — master CANoe test module template:
  - Full ISO-TP transport layer in CAPL (SF / FF / CF / FC, Flow Control, 0x78
    response-pending loop)
  - `on message kTxCanId` handler for frame reassembly (SF/FF/CF/FC)
  - Shared UDS helpers: `Uds_EnterSession`, `Uds_UnlockLevel`, `Uds_ReadDid`,
    `Uds_WriteDid`, `Uds_ClearDtcs`, `Uds_EcuReset`
  - Assert helpers: `AssertPositiveResponse`, `AssertNegativeResponse`,
    `AssertResponseLength`
  - Security key arrays (`kSecKeyLevelN[16]`) with AES-CMAC and XOR-stub derivation
  - Core service testcases: `TC_Services_DefaultSession`, `TC_Services_ExtendedSession`,
    `TC_Services_ProgrammingSession`, `TC_Services_TesterPresent`,
    `TC_Services_TesterPresentSuppressed`, `TC_Services_EcuReset_Hard`,
    `TC_Services_EcuReset_Soft`, `TC_Services_SessionTimeout`, SecurityAccess testcases
  - DID smoke testcases (`TC_DID_Read_Smoke_XXXX`) per configured DID
  - `testgroup TG_CoreServices`, `testgroup TG_DID_SmokeTests`,
    `maintest <ECU>_DiagnosticsSuite`
- `tools/templates/test_did_XXXX.can.j2` — per-DID exhaustive test module:
  - DID constants, `SetupRead_XXXX()` / `SetupWrite_XXXX()` helpers
  - Conditionally generated testcases per YAML access policy
  - `testgroup TG_DID_XXXX` runner
- `tools/templates/test_dtcs.can.j2` — DTC service test template:
  - DTC code constants, helpers, RDTCI sub-function testcases, `testgroup TG_DTCTests`
- `testgen.py`: `_build_security_levels()` adds `default_key_bytes` for CAPL key arrays
- `testgen.py`: `code_hi`, `code_mid`, `code_lo` pre-computed in DTC context
- `testgen.py`: `_capl_readme()` generates `README_CANOE.md` with import instructions
- Scale: `basic_ecu` (5 DIDs, 2 DTCs) → 8 `.can` files, 47 `testcase` functions

### Added — VS Code Extension (`ide/vscode-extension/`)

- `src/extension.ts` — activation on `onLanguage:yaml`, command registrations,
  status bar item, auto-save hook
- `src/validator.ts` — inline YAML diagnostics: DID/DTC format, duplicates,
  `data_length > 64`, enum values, write-security ASIL-B warning
- `src/hoverDocs.ts` — documentation for every YAML key with ISO 14229 context
- `src/hoverProvider.ts` — key-path resolver + `HoverProvider` implementation
- `src/codegenRunner.ts` — terminal-based codegen execution with QuickPick flag picker
- `schemas/diagnostics_config.schema.json` — full JSON Schema for
  `diagnostics_config.yaml`
- Commands: `EDS: Run Codegen`, `EDS: Run Codegen (with options)`,
  `EDS: Validate diagnostics_config.yaml`, `EDS: Open Documentation`

### Changed

- `tools/testgen.py` version bumped to 1.1.0; `--capl`/`--capl-only` flags added;
  fully backward-compatible (no `--capl` = identical v1.0.0 behaviour)
- `CLAUDE.md`: absolute rule #8 added (never use `>>` inside Jinja2 `{{ }}`);
  `ide/` directory added to repo tree; CAPL build commands added

---

## [1.0.0] — Phase 9 Complete

**Status: All tests passing. 35/35 unit tests. 68/68 harness tests.**

### Added
- 14 UDS service handlers: 0x10, 0x11, 0x14, 0x19, 0x22, 0x27, 0x28, 0x2E, 0x31,
  0x34, 0x36, 0x37, 0x3D, 0x3E
- ISO-TP transport: SF, FF, CF, FC with full N_As/N_Bs/N_Cs/N_Cr timing parameters
- ASIL-B 5-step safety wrapper chain enforced by code generator on every DID access
- AES-128-CMAC SecurityAccess (0x27) — production security algorithm
- YAML-driven code generator (`tools/codegen.py`) with 8 Jinja2 templates
- Auto-generated pytest test suites (`tools/testgen.py`)
- 4 reference ECU examples: basic_ecu, bms_ecu, motor_controller, ardep
- React/TypeScript live dashboard GUI + bridge.py WebSocket bridge
- 12-job GitHub Actions CI pipeline
- STM32 Nucleo-H743ZI2 board overlay and build configuration
- DTC NVM mirror with `dtc_mirror_init()` / `dtc_mirror_load()` in init sequence
- 35 Unity unit test modules (all passing)
- 68 harness integration tests (all passing)
- `uds_safety_self_test()` callable at boot for runtime safety self-check
- Violation counters and `last_violation_code` for field diagnostics
- Requirement traceability tags REQ-SAFE-001 through REQ-SAFE-007

### Fixed (Phase 9 repairs)
- P9-H1: `basic_ecu/CMakeLists.txt` — added 6 missing DFU sources
- P9-M1: Root `CMakeLists.txt` — `CONFIG_BOARD_NATIVE_SIM` conditional for
  `nvm_store` + `zephyr_flash_ops`
- P9-M2: `ci.yml` — `npm install` → `npm ci`; `cache-dependency-path` updated
- P9-L1: `build_harness.sh` — test count updated 55 → 68
- P9-L2: `build_tests.sh` — test count updated 29 → 35
- `generate_lockfile.sh` added to `gui/`

### Architecture
- No dynamic memory allocation anywhere in the stack
- No recursion; all state machines use explicit state variables
- Static buffer management with compile-time size bounds
- Explicit `uds_status_t` return on all public APIs
- Initialization guards on all context structures

---

## [0.9.0] — Phase 8

### Added
- ARDEP fourth ECU example with DFU support
- Extended DTC database with NVM mirror architecture
- Motor controller ECU example with speed/torque DIDs
- ISO-TP consecutive frame and flow control improvements
- Python integration test framework

### Fixed
- Session timeout handling in extended diagnostic session
- Security access delay timer reset on ECU reset

---

## [0.8.0] — Phase 7

### Added
- BMS ECU example with cell voltage and temperature DIDs
- YAML validation in code generator (duplicate DID detection, format checks)
- DTC severity classification
- React GUI configurator initial release

### Fixed
- ISO-TP first frame segmentation for payloads > 4095 bytes
- UDS session persistence across TesterPresent timeouts

---

## [0.5.0] — Phase 5

### Added
- Initial code generator (`tools/codegen.py`) with 3 templates
- DID database with read/write handler registration
- DTC database with status tracking
- Basic ECU reference example
- Unity unit test framework integration
- GitHub Actions CI (4-job initial pipeline)

---

## [0.1.0] — Phase 1

### Added
- Repository structure and architecture documents
- Core UDS server skeleton (0x10, 0x22, 0x3E)
- ISO-TP single frame support
- Zephyr CAN driver binding
- Initial CMakeLists.txt and west.yml
