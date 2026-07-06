# Xaloqi EDS — Installation Guide

> **This file is for commercial license customers.** It is included in the Developer and
> Professional ZIP archives and describes how to install the toolchain files into your EDS
> repo. Community users cloning the public repo can ignore this file.

**Product:** Xaloqi EDS (Xaloqi Embedded Diagnostic Suite)  
**Version:** v1.10.0  
**Support:** contact@xaloqi.com

---

## What you have

This ZIP delivers the commercial toolchain for Xaloqi EDS.  
It installs by extracting into the public EDS repo you already cloned.

**Developer tier includes:**
- 17 Jinja2 code generation templates (the generation engine)
- SOVD Bridge: `--sovd` flag generates OpenSOVD 1.0 `sovd_cda.json` from any `diagnostics_config.yaml`
- `testgen.py` — pytest + CANoe CAPL test suite generator
- `arxml_parser.py` — AUTOSAR 4.x ECU Extract ARXML → `diagnostics_config.yaml` importer (stdlib only, no deps)
- `mcp_server.py` — AI assistant integration (Claude, Cursor, Copilot)
- `eds_ai.py` — AI CLI: generate configs from plain English, explain NRC failures, validate ASIL-B
- `_license.py` — offline license verification module
- React/TypeScript GUI dashboard + WebSocket bridge
- VS Code extension (YAML validation, hover docs, Run Codegen)
- 7 specialist ECU examples: BMS, Motor Controller, ARDEP, Sensor, SafeBoot, Robotics, Sensor FreeRTOS

**Professional tier adds:**
- Integration test harness (`harness/`) — 68 C tests against the full UDS stack
- Safety documentation package (`safety_docs/`):
  - Safety Manual EDS-SM-001 Rev 1.1 (PDF)
  - Hazard Analysis and Risk Assessment (EDS-HARA-001)
  - Tool Qualification Argument (EDS-TQA-001)
  - Requirements Traceability Matrix — 30 ASIL-B requirements, all COVERED
  - MISRA C:2012 Deviation Log — 38 deviations, 0 open violations
  - Security Integration Guide
  - Customer Notice (integration responsibilities, ASIL-B candidate status)

---

## Step 1 — Clone the public repo (skip if already done)

```bash
git clone https://github.com/Xaloqi/EDS
cd EDS
```

Verify you are on the correct version:

```bash
git checkout v1.10.0
```

---

## Step 2 — Extract this ZIP into the repo root

```bash
# From the EDS repo root:
unzip -o /path/to/xaloqi-eds-developer-v1.10.0.zip

# Or for Professional:
unzip -o /path/to/xaloqi-eds-professional-v1.10.0.zip
```

The `-o` flag overwrites existing files without prompting — required when updating an existing installation.

> **Upgrading a clone made before 2026-07-06?** Earlier repo revisions contained
> `tools/templates`, `tools/testgen.py`, `tools/_license.py`, and `harness` as
> symbolic links, which caused `unzip -o` to fail with
> `checkdir error: cannot create tools/templates: File exists`. If you see that
> error, remove the stale links once and re-extract:
>
> ```bash
> rm -f tools/templates tools/testgen.py tools/_license.py harness
> unzip -o /path/to/xaloqi-eds-developer-v1.10.0.zip
> ```

This extracts the toolchain files directly into the repo tree. No separate directory. After extraction you will have:

```
EDS/
├── tools/
│   ├── templates/      ← new
│   ├── testgen.py      ← new
│   ├── mcp_server.py   ← new
│   └── _license.py     ← new
├── gui/
│   ├── src/            ← new
│   └── server/bridge.py ← new
├── ide/
│   └── vscode-extension/ ← new
├── examples/
│   ├── bms_ecu/        ← new
│   ├── motor_controller_ecu/ ← new
│   ├── ardep_ecu/      ← new
│   ├── sensor_ecu/     ← new
│   ├── safeboot_ecu/   ← new
│   ├── robot_joint_controller_ecu/ ← new
│   └── sensor_ecu_freertos/ ← new
├── harness/            ← new (Professional only)
└── safety_docs/        ← new (Professional only)
```

---

## Step 3 — Install Python dependencies

```bash
pip install pyyaml jinja2 cryptography PyJWT
```

For the test suite and MCP server:

```bash
pip install pytest python-can
```

For `eds_ai.py` (AI configuration assistant):

```bash
pip install anthropic
```

All dependencies are listed in `tools/requirements.txt`. To install everything at once:

```bash
pip install -r tools/requirements.txt
```

> **Note for Ubuntu 23.04+ / Debian 12+ users:** if `pip install` fails with
> `externally-managed-environment`, either use a virtual environment
> (`python3 -m venv .venv && source .venv/bin/activate`) or add
> `--break-system-packages` to the pip command.

---

## Step 4 — Activate your license key

Your license key was delivered in the purchase email. Install it:

```bash
python3 tools/activate.py --key YOUR-LICENSE-KEY-HERE
```

The key is stored at `~/.xaloqi/license.key`.  
Alternatively, set the environment variable: `export XALOQI_LICENSE_KEY=YOUR-KEY`.

Verify activation:

```bash
python3 tools/activate.py --check
```

Expected output:

```
License: ACTIVE
  Tier   : developer          (or: professional)
  Email  : engineer@yourcompany.com
  Expires: 2027-04-21
  Product: xaloqi-eds v1
```

---

## Step 5 — Verify: run codegen on the BMS example

```bash
python3 tools/codegen.py \
  --config examples/bms_ecu/diagnostics_config.yaml \
  --out    examples/bms_ecu/generated/ \
  --safety-wrappers --asil-level B --test-gen

# Optional: also generate the SOVD CDA for Eclipse SDV / OEM SOVD clients
python3 tools/codegen.py \
  --config examples/bms_ecu/diagnostics_config.yaml \
  --out    examples/bms_ecu/generated/ \
  --safety-wrappers --asil-level B --sovd
```

Expected output:

```
========================================================================
  Xaloqi EDS — Code Generator v1.10.0
========================================================================
  Config   : examples/bms_ecu/diagnostics_config.yaml
  ...
[1/5] Config loaded  — <N> DID(s), <N> DTC(s).
[2/5] Base validation passed.
[2B]  ASIL-B safety validation passed.
[3/5] Rendering standard templates...
  [OK]     generated/uds_init.c
  [OK]     generated/did_handlers.c
  ... (5 standard files + 3 safety wrapper files)
[5/5] Manifest skipped (--no-manifest).

========================================================================
  Generation complete.
========================================================================
```

---

## Step 6 — Run the generated test suite

```bash
cd examples/bms_ecu/generated/tests
pytest . -v --can-interface=simulator
```

All tests should pass. This validates the generated code, the license key, and the testgen pipeline in one command.

---

## Optional: VS Code extension

Install the pre-built extension:

```bash
code --install-extension ide/vscode-extension/eds-diagnostics-1.1.0.vsix
```

Or open `ide/vscode-extension/` in VS Code and press F5 to run in development mode.

Features:
- YAML schema validation as you type
- Hover documentation for every YAML field
- Command palette: `EDS: Run Codegen` — one-click generation
- Inline error messages for ASIL-B violations in your config

---

## Optional: GUI dashboard

Start the dashboard in demo mode (no hardware required):

```bash
cd gui
npm install       # first time only
bash start-demo.sh
```

Open `http://localhost:3000`. The demo bridge simulates a live ECU with sensor data.

For real hardware, connect your ECU via CAN and run:

```bash
bash start-can.sh
```

---

## Optional: TestLab AI — job result analysis and reporting

`testlab.py` analyses the JSON files produced by `jobrunner.py --json` and generates terminal summaries, trend tables, HTML reports, and regression diffs. No ECU connection required — it works entirely from saved results files.

### Analyse a single run

```bash
python3 tools/testlab.py analyze \
    --results results/sensor_ecu_20260501_090000.json
```

Add `--explain-failures` to call `eds_ai.py` for each failed step that has an NRC (requires `ANTHROPIC_API_KEY`):

```bash
python3 tools/testlab.py analyze \
    --results results/sensor_ecu_20260501_090000.json \
    --config  examples/sensor_ecu/diagnostics_config.yaml \
    --explain-failures
```

### Generate an HTML report

Produces a single self-contained HTML file — no external dependencies, works offline:

```bash
python3 tools/testlab.py report \
    --results results/sensor_ecu_*.json \
    --config  examples/sensor_ecu/diagnostics_config.yaml \
    --out     reports/sensor_ecu_week_$(date +%V).html
```

When more than one results file is given, the report includes an inline SVG trend chart showing pass rate per step across all runs.

### Compare two runs (regression diff)

Identifies which steps regressed (was passing, now failing), recovered, are new, or were removed:

```bash
python3 tools/testlab.py compare \
    --before results/run_before.json \
    --after  results/run_after.json \
    --out    reports/compare.html
```

Returns exit code 1 if any regressions are found — suitable for use as a CI gate.

### Show pass-rate trend across multiple runs

```bash
python3 tools/testlab.py trend \
    --results results/sensor_ecu_*.json
```

### CI integration example

Accumulate results in CI and generate a weekly report as a build artifact:

```yaml
# .github/workflows/weekly-diagnostics.yml
- name: Run diagnostic jobs
  run: |
    python3 tools/jobrunner.py \
      --config examples/sensor_ecu/diagnostics_config.yaml \
      --all --mode harness \
      --binary ./build/uds_harness \
      --json results/sensor_ecu_$(date +%Y%m%d_%H%M%S).json

- name: Generate TestLab AI report
  run: |
    python3 tools/testlab.py report \
      --results results/sensor_ecu_*.json \
      --config  examples/sensor_ecu/diagnostics_config.yaml \
      --out     reports/weekly_$(date +%V).html
  env:
    XALOQI_LICENSE_SKIP: "1"

- name: Upload report
  uses: actions/upload-artifact@v4
  with:
    name: testlab-report-week-${{ env.WEEK }}
    path: reports/

- name: Fail on regressions (compare with last baseline)
  run: |
    python3 tools/testlab.py compare \
      --before results/baseline.json \
      --after  results/sensor_ecu_latest.json
```

---

## Optional: MCP server (AI assistant integration)

Connect Claude, Cursor, or Copilot to your EDS installation:

```bash
python3 tools/mcp_server.py
```

Exposed tools:
- `generate_did_config` — generate a DID YAML block from a description
- `run_codegen` — run codegen and return generated file paths
- `validate_asil_b` — check a YAML config for ASIL-B compliance
- `explain_uds_error` — decode any UDS NRC code with context

See `tools/templates/CLAUDE.md` for the full AI workflow context document.

---

## Optional: AI configuration assistant (eds_ai.py)

`eds_ai.py` wraps the Anthropic Claude API to provide three domain-specific commands. It requires a valid Xaloqi license key and your own Anthropic API key (billed directly to your Anthropic account — Xaloqi does not proxy or meter API calls).

### One-time setup

Store your Anthropic API key:

```bash
python3 tools/eds_ai.py setup --api-key sk-ant-YOUR-KEY-HERE
```

The key is stored at `~/.xaloqi/anthropic_api_key` with permissions `600`. Alternatively: `export ANTHROPIC_API_KEY=sk-ant-...`

Verify everything is configured:

```bash
python3 tools/eds_ai.py status
```

Expected output:

```
Xaloqi EDS AI Assistant v1.0.0

  License:      ✓ Valid (Developer tier, expires 2027-04-21)
  API key:      ✓ Set (ANTHROPIC_API_KEY)
  Connectivity: ✓ Anthropic API reachable
  Codegen:      ✓ tools/codegen.py found

Ready. Try: python3 tools/eds_ai.py generate "your ECU description"
```

### generate — create a config from plain English

```bash
python3 tools/eds_ai.py generate \
  "Battery management ECU with 8 cell voltages (0–5V), state of charge,
   pack temperature, and a cell balancing calibration routine.
   ASIL-B. Extended session requires security level 1 for writes."
```

Output is written to a derived path (e.g. `bms_ecu/diagnostics_config.yaml`). Override with `--out`:

```bash
python3 tools/eds_ai.py generate "Motor controller: speed, torque, fault status" \
  --out my_project/diagnostics_config.yaml
```

Generate and immediately run codegen in one step:

```bash
python3 tools/eds_ai.py generate "Simple ECU with VIN and serial number" \
  --run-codegen --codegen-out my_ecu/generated/
```

**Always review AI-generated YAML before committing it to production.** The tool validates the output through `codegen --dry-run` automatically and retries once on failure, but engineer sign-off is required before production use.

### explain — diagnose a UDS NRC failure

```bash
# Minimal: NRC + service
python3 tools/eds_ai.py explain --nrc 0x33 --service 0x27

# With DID and session context
python3 tools/eds_ai.py explain \
  --nrc 0x31 --service 0x2E --did 0xF190 --session extended

# With your actual config file for DID-specific analysis
python3 tools/eds_ai.py explain \
  --nrc 0x33 --service 0x27 \
  --config examples/bms_ecu/diagnostics_config.yaml
```

The NRC table (all standard ISO 14229 codes) is embedded in the tool and works offline. The AI explanation requires an API connection.

### validate — ASIL-B review of a config file

Two-pass review: structural (codegen dry-run) then AI semantic analysis.

```bash
python3 tools/eds_ai.py validate \
  --config examples/bms_ecu/diagnostics_config.yaml
```

Example output:

```
✓ Structural validation passed (codegen dry-run: 0 errors)

⚠  ASIL-B advisory (1 item):

  1. DID 0xFD01 (cell_balance_target) — writable in extended session without
     security gating — add security_level_write: 1

Review these before running codegen for production use.
```

---

## Developer only: arxml_parser.py — AUTOSAR ARXML import

`tools/arxml_parser.py` converts an AUTOSAR 4.x ECU Extract ARXML file into a
`diagnostics_config.yaml` ready for `codegen.py`. No external dependencies.

**Typical use case:** your OEM supplied an ARXML file. Instead of re-entering all DIDs, DTCs,
and sessions by hand, import the ARXML and get a populated YAML in seconds — then review and run
codegen.

```bash
python3 tools/arxml_parser.py --input ecu_extract.arxml --output diagnostics_config.yaml
```

Warnings about skipped or defaulted elements are printed to stderr. The YAML is always valid —
review it before running codegen, particularly:

- `access` on DIDs defaults to `["read"]` — add `"write"` where needed
- `algorithm` on security levels defaults to `aes128_cmac` — confirm this matches your ECU
- Check that `data_length` values match your ECU's actual DID payloads

**Supported:** AUTOSAR 4.x ECU Extract ARXML. Not supported: AUTOSAR 3.x, full system ARXML.

---

## Professional only: safety documentation

The `safety_docs/` directory contains the complete ISO 26262-aligned documentation package:

| Document | File | Purpose |
|---|---|---|
| Safety Manual Rev 1.1 | `EDS_Safety_Manual_EDS-SM-001_Rev1.1.pdf` | Component safety manual — integration responsibilities, safety boundaries |
| HARA | `EDS-HARA-001.md` | 6 hazard events, ASIL decomposition argument |
| TQA | `EDS-TQA-001.md` | Tool qualification argument (TCL1 per ISO 26262-8 §11) |
| RTM | `EDS_Requirements_Traceability_Matrix.csv` | 30 ASIL-B requirements, all COVERED |
| MISRA log | `MISRA_DEVIATION_LOG.md` | 38 documented deviations, 0 open violations |
| Security guide | `SECURITY_INTEGRATION.md` | OEM key provisioning for AES-128-CMAC |
| Customer notice | `CUSTOMER_NOTICE.md` | ASIL-B candidate status, integrator responsibilities |

**Important:** These documents are ASIL-B *candidate* work products. They have not been independently assessed by a TÜV or other accredited body. An independent assessment is required before claiming certified ASIL-B status in a vehicle-level safety argument. See `CUSTOMER_NOTICE.md` for full detail.

---

## Professional only: integration test harness

The `harness/` directory contains a C-level integration test harness that exercises the full UDS stack end-to-end:

```bash
# Build and run all 68 integration tests:
bash build_harness.sh --run

# Build only (MISRA-clean flags):
bash build_harness.sh
```

The harness compiles `harness_main.c` + the full EDS stack under the full MISRA-relevant GCC warning set (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`) and verifies zero warnings.

---

## Renewal

Your license renews annually. You will receive a renewal email before expiry.  
After renewal, re-activate with your new key:

```bash
python3 tools/activate.py --key YOUR-NEW-KEY
```

A 14-day grace period applies after expiry before codegen stops working.

---

## Support

**Email:** contact@xaloqi.com  
**Response time:** 2 business days (Developer) / 5 business days SLA (Professional)  
**License issues:** contact@xaloqi.com  

For MISRA or safety documentation questions (Professional), include your ECU project context and the specific requirement in question.
