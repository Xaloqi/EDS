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
- Changes that break the `native_sim` CI build or cause any of the 35 unit tests to fail
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

# All 36 unit tests must pass
bash build_tests.sh

# All 68 harness tests must pass
bash build_harness.sh

# native_sim build must succeed
west build -b native_sim examples/basic_ecu \
  -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
```

CI runs all 12 jobs automatically on pull requests. A PR will not be merged if
any CI job is red.

### 5. Pull request checklist

- [ ] Issue number referenced in the PR description
- [ ] CLA signed (if touching `core/`, `transport/`, `config/`, or `platform/`)
- [ ] SPDX header on every new file
- [ ] Unit test added or updated for the changed module
- [ ] `generated/` files regenerated and committed if YAML or templates changed
- [ ] All 35 unit tests pass locally (`bash build_tests.sh`)
- [ ] PR title follows format: `[fix|feat|docs|test|chore]: short description`

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

