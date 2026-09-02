# Contributing to Xaloqi EDS

Thanks for your interest in contributing. Before you open a pull request, read
this document — the dual-license model creates obligations that differ from a
typical open-source project.

---

## Dual-license model and the CLA requirement

EDS is published under two licenses:

| Component | License |
|---|---|
| Runtime stack: `core/`, `transport/`, `config/`, `platform/` | GPL v2 |
| ECU examples: `examples/` | Apache 2.0 |
| Tooling: `tools/codegen.py`, `tools/testgen.py`, Safety Manual | Commercial license |

This dual-license approach — the same model used by [Quantum Leaps QP](https://www.state-machine.com/qm/)
and [MySQL](https://www.mysql.com/about/legal/licensing/oem/) — means that **all
contributions to the GPL v2 runtime stack require a signed Contributor License
Agreement (CLA).**

### Why the CLA is required

Contributions to `core/`, `transport/`, `config/`, and `platform/` may be included
in builds distributed under the commercial license. Without a CLA, the commercial
license cannot legally cover your contribution. The CLA does **not** transfer
copyright — you retain full ownership. It grants the right to relicense your
contribution as part of the commercial offering.

### How to sign the CLA

Before your first pull request touching the runtime stack is merged:

1. Email **contact@xaloqi.com** with subject: `CLA — GitHub username: <your-handle>`
2. Include your full name, GitHub username, and employer name (if contributing on
   behalf of an employer — your employer's sign-off may also be required)
3. You will receive the CLA to review and sign electronically
4. Once signed, your handle is added to `CLA_SIGNATORIES.md` and future PRs
   from that account can be merged without delay

**Contributions to `examples/` (Apache 2.0) do not require a CLA.**
Bug reports, documentation improvements, and issue comments never require a CLA.

---

## What we welcome

### Always welcome — no CLA needed

- Bug reports via GitHub Issues
- Documentation corrections and improvements
- New or improved ECU examples under `examples/`
- Additional unit tests that expose existing bugs
- CI/build system improvements
- YAML configuration improvements to existing examples

### Welcome with CLA — runtime stack contributions

- Bug fixes in `core/`, `transport/`, `config/`, `platform/`
- New UDS service handlers (must include a unit test module in `tests/unit_runnable/`)
- Platform port additions (new board or RTOS target)
- ISO-TP timing or performance improvements
- Safety wrapper improvements with documented traceability

### Not accepted

- Changes that remove or weaken any step of the 5-step ASIL-B safety wrapper chain
- Dynamic memory allocation (`malloc`, `free`) anywhere in the stack
- Recursion in any safety-critical module
- Changes that break the `native_sim` CI build or cause any unit test to fail (currently 45 — see build_tests.sh, which enforces its own count via an anti-drift check)
- Hand-written changes to files under `generated/` — these are codegen output;
  fix the template in `tools/templates/` instead
- External runtime dependencies not already present in the stack

---

## Development workflow

### 1. Open an issue first

For anything beyond a trivial bug fix, open a GitHub Issue before writing code.
This avoids duplicate effort and lets us confirm the change fits the architecture
before you invest time in it.

### 2. Fork and branch

```bash
git clone https://github.com/Xaloqi/EDS
git checkout -b fix/isotp-consecutive-frame-timeout
```

Branch naming: `fix/`, `feat/`, `docs/`, `test/`, `chore/` prefix, kebab-case description.
Target branch for pull requests: `main`.

### 3. Coding conventions

- **C standard:** C11 (`-std=c11`)
- **No dynamic memory.** No `malloc`, no `free`, no VLAs. The `no-malloc-check` CI job
  enforces this with a hard grep gate.
- **No recursion** in any module in `core/` or `transport/`.
- **Return type:** All public API functions return `uds_status_t`. No `void` on any
  path that can fail.
- **Unit test:** Every new module in `core/` or `transport/` needs a corresponding
  `tests/unit_runnable/test_<module>.c`. New service handlers need both a unit test
  and an entry in `tests/CMakeLists.txt`.
- **SPDX header** on every new file:
  - Runtime stack: `// SPDX-License-Identifier: GPL-2.0-only`
  - Examples: `// SPDX-License-Identifier: Apache-2.0`
- **Generated files:** If you changed `diagnostics_config.yaml` or any template under
  `tools/templates/`, regenerate and commit the output:
  ```bash
  python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out generated/ \
    --safety-wrappers --asil-level B --test-gen
  ```

### 4. Verify before submitting

```bash
# Regenerate if you touched YAML or templates
python3 tools/codegen.py \
  --config examples/basic_ecu/diagnostics_config.yaml \
  --out generated/ \
  --safety-wrappers --asil-level B --test-gen

# All unit tests must pass (currently 45)
bash build_tests.sh

# All 68 harness tests must pass
bash build_harness.sh

# native_sim build must succeed
west build -b native_sim examples/basic_ecu \
  -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
```

CI runs the full workflow automatically on pull requests — see
`.github/workflows/ci.yml` for the current job list (it grows as coverage is
added; don't hardcode a count here, it has drifted before — #91). A PR will
not be merged if any CI job is red.

### 5. Pull request checklist

- [ ] Issue number referenced in the PR description
- [ ] CLA signed (if touching `core/`, `transport/`, `config/`, or `platform/`)
- [ ] DCO sign-off on every commit (`git commit -s`) — see [Developer Certificate of Origin](https://developercertificate.org/)
- [ ] SPDX header on every new file
- [ ] Unit test added or updated for the changed module
- [ ] `generated/` files regenerated and committed if YAML or templates changed
- [ ] All unit tests pass locally (`bash build_tests.sh`)
- [ ] PR title follows format: `[fix|feat|docs|test|chore]: short description`

---

## Adding a new UDS service handler

Use this checklist when implementing a new SID. Every item is required — gaps
here have caused real CI failures and customer-visible linker errors.

### Stack (EDS repo)

- [ ] `core/uds_types.h` — add `UDS_SID_*` constant
- [ ] `core/uds_services/services.h` — declare handler, bump `UDS_SERVICE_TABLE_COUNT`
- [ ] `core/uds_services/service_registration.c` — add table entry
- [ ] `core/uds_access_table.c` — add ACL entry
- [ ] `core/uds_access_table.h` — bump `UDS_ACCESS_TABLE_DEFAULT_COUNT`
- [ ] `core/uds_services/service_0xNN.c` — implement handler
- [ ] `cmake/eds_service_sources.cmake` — add one line (**this propagates to all 20 examples automatically**)
- [ ] `tests/unit_runnable/test_service_0xNN.c` — unit tests (aim for 14+ cases)
- [ ] `tests/CMakeLists.txt` — `add_diag_test(test_service_0xNN)`
- [ ] `build_tests.sh` — add to `STACK_SRCS` and `TESTS` arrays, bump count in comment
- [ ] `build_harness.sh` — add `$ROOT/core/uds_services/service_0xNN.c` to `STACK_SRCS`;
  also add any new support module the handler depends on (e.g. `uds_periodic.c` for 0x2A,
  `uds_io_control.c` for 0x2F if one exists). Omitting this causes undefined-reference linker
  errors that only surface when running the 68 harness tests, not the unit tests.
- [ ] `.github/workflows/ci.yml` — bump the expected count in the `Verify test
  count` step (the `unit-tests` job's display name is deliberately count-free,
  so it never needs touching — see #119)
- [ ] `misra_analysis.py` — add `core/uds_services/service_0xNN.c` to **DEV-MULT-01**
  `files` list (Rule 15.5 — early-return guard pattern used by all handlers)

### Docs

- [ ] `CHANGELOG.md` — entry under `[Unreleased]`
- [ ] `README.md` — service count and SID list (two places)
- [ ] `CONTRIBUTING.md` — unit test count (three places)
- [ ] `docs/llms.txt` — service count (two places), remove from "not implemented" list
- [ ] `docs/INTEGRATION_GUIDE.md` — add row to service table, remove from "out of scope"
- [ ] `docs/TESTING_STRATEGY.md` — service count
- [ ] `docs/AI_CONTEXT.md` — service count

> `docs/README.md` was a standalone duplicate of the root README and drifted
> out of sync for three releases (frozen at v1.8.2 through v1.10.0). It is now
> a one-line pointer stub — do not restore duplicate content there.

### EDS-toolchain repo

- [ ] `CLAUDE.md` — service count (one place)

> The per-example `CMakeLists.txt` files no longer need individual updates — adding
> one line to `cmake/eds_service_sources.cmake` covers all 20 build targets.
> The EDS-toolchain examples inherit the list when they include
> `${DIAG_ROOT}/cmake/eds_service_sources.cmake` at build time.

---

### Lessons learned from issue #33 (2026-07) — harness linker failure

**`build_harness.sh` must be updated alongside `service_registration.c`**

Five services (0x23, 0x2A, 0x2F, 0x35, 0x3D) were fully implemented and registered,
but `build_harness.sh` was never updated. The unit tests (`build_tests.sh`) passed
because they do not compile `service_registration.c` in a way that forces all
registered handlers to be linked. The harness does — it links the entire stack — so
it failed with `undefined reference` for all five handlers at once.

Rule: whenever you add an entry to the `STACK_SRCS` section of `build_tests.sh`,
ask "does `build_harness.sh` also need this file?" The answer is almost always yes
for new service handlers and for new support modules they depend on.

The fix was 6 lines in `build_harness.sh` (EDS PR #66):
the 5 service `.c` files plus `uds_periodic.c`, which `service_0x2A.c` requires for
`uds_periodic_subscribe` / `uds_periodic_unsubscribe`.

### Lessons learned from adding 0x2F (2026-06)

These caught CI failures on a real PR. Record them here so the next engineer
doesn't repeat the same debugging session.

**Counter conflict resolution during rebase**

When two branches both increment the same numeric constant from the same base
value (common when two SIDs land close together), naively keeping either side
is wrong. If main went A → B and your branch went A → C, the merged value is
A + (B−A) + (C−A). In practice: count the actual table entries in the merged
file and set the constant to match. Affected constants:

- `UDS_SERVICE_TABLE_COUNT` in `core/uds_services/services.h`
- `UDS_ACCESS_TABLE_DEFAULT_COUNT` in `core/uds_access_table.h`

GCC catches a wrong `UDS_SERVICE_TABLE_COUNT` as
`warning: excess elements in array initializer` on `service_registration.c`.
`misra_analysis.py` classifies this as **Rule N/A** (the warning has no `-W`
flag suffix). If you see "Rule N/A" in the MISRA report pointing at
`service_registration.c`, check the array size constant first.

**misra_analysis.py DEV-MULT-01 requires manual enumeration**

The Rule 15.5 deviation (`DEV-MULT-01`) applies only to files explicitly listed
in its `files` array in `misra_analysis.py`. Every new service handler `.c` file
must be added to that list or CI reports 15+ open violations. The entry goes
between the neighbouring service files (alphabetical / numerical order).

**MISRA Rule 2.5 — define only macros you reference**

Constants defined in the `.c` file for documentation (e.g., named control
parameter values) that are never referenced in the code itself trigger Rule 2.5
violations. Either use them explicitly in `switch`/`if` branches, or remove
them and rely on the file-level comment to document the protocol values.

### Lessons learned from the #212–#218 Software Integrator batch (2026-09)

Seven issues from one external evaluation pass, triaged and landed as two
consolidated PRs (#219 mechanical, #220 hardening) instead of seven. Record
these so the next batch of external feedback goes faster.

**Search for the concept, not just the literal string, before declaring a gap**

Issue #215 reported "grep confirms `EDS_PRODUCTION` does not exist anywhere
in the repo" as evidence there was no general production-build primitive to
gate the FreeRTOS flash/NVM stubs on. That grep was correct and the gap
in the FreeRTOS RAM stubs was real — but the general primitive it went
looking for already existed, just under a different name:
`EDS_BUILD_IS_PRODUCTION` (`SEC-BUILD-MODE-01`, `core/uds_security_algo.h`),
built for issue #84 specifically so no gate would ever need to re-derive
"is this production" from scratch again. The fix reused it. Rule: before
concluding a primitive doesn't exist, grep the *concept* (`grep -ri
"is.*production"` / read the architecture doc's build-mode section) as well
as the exact string an issue names — an issue's own literal grep is data
about what string was searched, not proof the capability is absent.

**Cross-repo issue scope needs to be explicit, or half the fix silently
doesn't land**

Issue #212 asked for two things: a doc fix in this repo, and two new regex
patterns in `check_version_headers()` — which lives in
`xaloqi-knowledge/scripts/check_release_docs.py`, a separate repository.
Only the doc fix could land from an EDS-scoped session; the checker patterns
are still open. When an issue's fix direction touches a file outside the
current repo, say which repo up front (as this one did, just not
prominently) so a partial fix doesn't get closed out as if it were
complete — the tracking issue should stay open, or a sibling issue should
be filed in the other repo, until both halves land.

**`doip_server.c` cannot call an RTOS clock directly — every new
time-bound limit in this file needs a bounded-count proxy, not a timer**

This constraint predates #218 (see #193's read-attempt bound) but #218 hit
it again for a connection-lifetime cap and reconnect rate limiting, and
will hit it again for the next limit added to this file. All platform
interaction here goes through `eds_doip_platform_ops_t` — no
`eds_platform_uptime_ms()`, no wall-clock deadline. The established
pattern (`DOIP_MAX_FRAME_READ_ATTEMPTS`, and now
`DOIP_MAX_CONNECTION_FRAMES` / `DOIP_RECONNECT_BACKOFF_REJECTS`) is a
bounded count standing in for a deadline. Before reaching for a real timer
in this file, check whether a bounded count already gets you the same
practical bound.

**`platform/freertos/*.c` has no host-test harness — plan to
syntax-check manually, not run `build_tests.sh`**

`build_tests.sh`'s `STACK_SRCS` / `INCLUDES` never reference anything under
`platform/freertos/`; those files are only compiled by the real FreeRTOS
example CMake builds (which need the actual FreeRTOS-Kernel + STM32Cube HAL
fetched, unavailable in a sandboxed session). #215's changes there were
validated with a standalone `gcc -fsyntax-only` pass against hand-written
FreeRTOS header stubs plus one real example's generated headers, not
`build_tests.sh`. If a future change to these files needs actual runtime
coverage (not just "does it compile"), that harness doesn't exist yet and
would need to be built — don't assume `build_tests.sh` passing says
anything about `platform/freertos/*.c`.

**Consolidating a batch of related low-risk issues into fewer PRs works —
split on "does this need a design decision," not on issue count**

Two PRs across seven issues, grouped as "mechanical/doc" vs "hardening,"
reviewed faster than seven would have and each PR's diff stayed easy to
read end to end. The actual split criterion that mattered: whether a fix
was a direct, low-ambiguity application of an existing pattern (exec bits,
stale version strings, a doc paragraph, an exit-code branch) vs. one that
required picking concrete values or a design approach with more than one
reasonable answer (#218's rate-limit thresholds, whether to extend the
DoIP platform-ops interface with a time source vs. use a bounded-count
proxy). Confirm the latter kind with whoever requested the batch before
implementing, even if the issue itself proposes concrete constant names —
proposed names in an issue are a direction, not a sign-off on values.

---

## AI-assisted development

Xaloqi EDS uses AI-assisted development tooling (Claude Code / Anthropic Claude) to
accelerate code generation, test generation, and code review. This section documents
how that tooling is used, what controls are applied, and where responsibility sits — in
response to procurement and safety audit questions about AI-generated content in
safety-relevant code.

### What AI tooling is used for

- First-draft generation of boilerplate C code, header files, and test stubs
- Review acceleration: surfacing potential MISRA violations, null-dereference paths,
  and missing error returns before static analysis runs
- Documentation drafting and changelog generation
- Codegen template authoring (the `tools/templates/` Jinja2 templates)

### What it is not used for

- Autonomous merge decisions — no AI output is merged without human sign-off
- Bypassing CI — all commits, regardless of origin, must pass all CI jobs (see .github/workflows/ci.yml for the current list)
- Architecture decisions — module boundaries, safety architecture, and API contracts
  are defined by the project maintainer

### Human review and validation requirements

Every commit that touches the EDS codebase, regardless of how it was generated, must satisfy:

1. **CI gate:** All CI jobs green — unit tests, CMake/CTest parity, integration
   tests, static analysis, Zephyr builds (multiple board targets), FreeRTOS
   builds, DoIP integration, SOVD codegen, robustness campaign, harness tests.
   See `.github/workflows/ci.yml` for the current, authoritative list.
2. **Human sign-off:** The committing engineer reads, understands, and takes
   responsibility for every line before it is staged. AI-generated output is a
   starting point, not a finished deliverable.
3. **MISRA scan:** The `static-analysis` CI job runs `misra_analysis.py` on every PR.
   Zero new violations are permitted.

### Safety-relevant changes — heightened review

Changes to any of the following files or modules require explicit human sign-off in the
PR description (`Reviewed-by: <name>`) in addition to CI passage:

- `core/uds_safety.c` — ASIL-B 5-step safety check engine
- `core/uds_session.c` — UDS session FSM (ISO 14229-1 §7.4)
- `core/uds_security.c` — SecurityAccess seed/key validation (ISO 14229-1 §10.4)
- `core/uds_services/service_0x27.c` — SecurityAccess service handler
- `core/uds_services/service_0x19.c` — ReadDTCInformation handler
- `transport/isotp.c` — ISO-TP framing and flow control (ISO 15765-2)
- `transport/doip/doip_server.c` — DoIP server (ISO 13400-2)
- Any file under `config/` that defines DTC status persistence

AI-generated output is **never merged directly** to `main` for these files without
a deterministic validation step: the static analysis job, the full unit test suite,
and explicit human code review.

### Maintainer responsibility

Xaloqi retains full engineering and legal responsibility for all committed code
in this repository, regardless of the method by which it was generated.
The use of AI tooling does not transfer, reduce, or qualify that responsibility.
AI-generated code that is merged into EDS is EDS code — it is subject to the same
MISRA conformance requirements, safety review process, and ISO 26262 obligations as
any other contribution.

---

## Reporting bugs

Open a GitHub Issue. Include:

- EDS version (git tag or full commit hash)
- Target board and Zephyr version
- Minimal reproduction: `diagnostics_config.yaml` + `west build` command + full error output
- What you expected vs. what happened

For security vulnerabilities, see [SECURITY.md](SECURITY.md) instead — do not use
public issues for security reports.

---

## Questions

- **Contribution questions:** open a GitHub Discussion or email **contact@xaloqi.com**
- **Commercial licensing:** see the [product page](https://xaloqi.com) or email **contact@xaloqi.com**

