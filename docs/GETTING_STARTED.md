# Getting Started with Xaloqi EDS

**Goal:** From zero to a running UDS diagnostics session on `native_sim` in 15 minutes.

**What you'll have at the end:**
- Zephyr workspace with EDS checked out
- A minimal ECU firmware built and running in the simulator
- UDS service 0x22 (ReadDataByIdentifier) responding to requests
- All 37 unit tests passing

**No prior Zephyr knowledge assumed.**

---

## Prerequisites

You need a Linux or macOS machine. Windows is supported via WSL2 (Ubuntu 22.04 recommended).

Install these before starting:

```bash
# Python 3.9 or newer
python3 --version   # must print 3.9+

# CMake 3.20 or newer
cmake --version     # must print 3.20+

# Git
git --version

# pip
pip3 --version
```

If anything is missing, install via your package manager:
```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y python3 python3-pip cmake git

# macOS (requires Homebrew)
brew install python cmake git
```

---

## Step 1 — Install West (5 min)

West is Zephyr's meta-tool. It manages the workspace and build commands.

```bash
pip3 install west
west --version   # must print 1.2 or newer
```

---

## Step 2 — Set Up the Zephyr Workspace (5 min)

```bash
# Create a workspace directory
mkdir eds-workspace && cd eds-workspace

# Initialise the workspace from the EDS repository
west init -m https://github.com/Xaloqi/EDS --mr v1.8.2 .

# Pull Zephyr and all dependencies (this downloads ~500 MB, takes 2-4 min)
west update
```

> **Note:** On a slow connection the
> `west update` step can take up to 10 minutes. It only runs once.

After `west update` your workspace looks like this:

```
eds-workspace/
├── EDS/                          ← EDS repo (this is where you work)
├── zephyr/                       ← Zephyr RTOS source
├── modules/                      ← Zephyr modules
└── .west/                        ← West config (don't touch)
```

---

## Step 3 — Install the Zephyr SDK (3 min)

The Zephyr SDK provides the compilers for all supported boards, including `native_sim`.

```bash
# Download the minimal SDK bundle (Linux x86-64)
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz
tar xf zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-0.16.8
./setup.sh -t x86_64-zephyr-elf   # native_sim uses the host GCC, but this registers the SDK
cd ..
```

> **macOS:** Download the `macos-x86_64` or `macos-aarch64` bundle from the same release page.
>
> **WSL2:** Use the Linux x86-64 bundle.

Set the SDK environment variable (add to your `~/.bashrc` or `~/.zshrc` to make it permanent):

```bash
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-0.16.8
```

---

## Step 4 — Install Python Dependencies (1 min)

```bash
cd EDS

# EDS Python tools (codegen, testgen, integration tests)
pip3 install -r tools/requirements.txt

# Verify the code generator works
python3 tools/codegen.py --help
```

You should see the codegen help text. If you see a Python import error, re-run
`pip3 install -r tools/requirements.txt`.

---

## Step 5 — Run the Code Generator (1 min)

The code generator reads `diagnostics_config.yaml` and produces the C source files
that implement your DID and DTC tables, safety wrappers, and init sequence.

```bash
# From the eds repo root
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --safety-wrappers \
    --asil-level B

# You should see output like:
# [codegen] Loaded 5 DIDs, 3 DTCs
# [codegen] Generated: examples/basic_ecu/generated/did_handlers.c
# [codegen] Generated: examples/basic_ecu/generated/did_handlers.h
# [codegen] Generated: examples/basic_ecu/generated/did_safety_wrappers.c
# [codegen] Generated: examples/basic_ecu/generated/did_safety_wrappers.h
# [codegen] Generated: examples/basic_ecu/generated/generated_config.h
# [codegen] Generated: examples/basic_ecu/generated/safety_config.h
# [codegen] Generated: examples/basic_ecu/generated/routine_handlers.c
# [codegen] Generated: examples/basic_ecu/generated/routine_handlers.h
# [codegen] Generated: examples/basic_ecu/generated/uds_init.c
# [codegen] Generated: examples/basic_ecu/generated/uds_init.h
# [codegen] Done.
```

> **Never hand-edit files in `generated/`.** They are overwritten every time codegen runs.

---

## Step 6 — Build the basic_ecu Example (2 min)

```bash
# From the eds repo root
west build -b native_sim examples/basic_ecu \
    -- -DDTC_OVERLAY_FILE=boards/native_sim/native_sim.overlay

# Expected output ends with:
# [100%] Linking C executable zephyr/zephyr.elf
# Memory region         Used Size  Region Size  %age Used
#    SRAM:              ...
# Build complete!
```

If the build fails, see the [Troubleshooting](#troubleshooting) section at the bottom.

> **Using FreeRTOS instead of Zephyr?**
> Steps 1–5 (Python tools and codegen) are identical — the same YAML produces the same C code on both platforms. Skip Steps 6–8 and go directly to [`docs/INTEGRATION_GUIDE.md` — FreeRTOS Integration](INTEGRATION_GUIDE.md#freertos-integration). You need `arm-none-eabi-gcc`, `cmake`, and a clone of [FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel). No Zephyr SDK or West required.

---

## Step 7 — Run the Simulator (1 min)

```bash
west build -t run
```

You should see the ECU boot log:

```
*** Booting Zephyr OS build v3.7.0 ***
[00:00:00.000] EDS v1.4.0 starting
[00:00:00.001] UDS server initialised
[00:00:00.001] DID database: 5 DIDs registered
[00:00:00.001] DTC database: 3 DTCs registered
[00:00:00.001] ISO-TP transport ready (CAN loopback)
[00:00:00.002] Diagnostics task running
```

The ECU is now running in Zephyr's `native_sim` — a Linux process that simulates the
embedded environment including the CAN loopback driver.

Press **Ctrl+C** to stop.

---

## Step 8 — Send a UDS Request (2 min)

With the simulator running, open a second terminal and run the Python integration test
to send a real UDS `ReadDataByIdentifier` (0x22) request:

```bash
# In a second terminal, from the eds repo root
pytest tests/integration/test_uds_read_did.py -v
```

Expected output:

```
tests/integration/test_uds_read_did.py::test_read_vin PASSED
tests/integration/test_uds_read_did.py::test_read_odometer PASSED
tests/integration/test_uds_read_did.py::test_read_invalid_did_returns_nrc_31 PASSED
tests/integration/test_uds_read_did.py::test_read_did_wrong_session_returns_nrc_7f PASSED
```

You just sent ISO-TP framed UDS requests to a running Zephyr ECU and validated the responses.

---

## Step 9 — Run All Tests (2 min)

```bash
# Unit tests — 37 modules, all must pass
bash build_tests.sh

# Integration tests (simulator mode, no hardware required)
cd examples/basic_ecu/generated/tests
pytest . --can-interface=simulator -v
```

All three should complete with zero failures. If any fail, check the
[Troubleshooting](#troubleshooting) section.

---

## What Just Happened

Here is the full flow you just ran:

```
diagnostics_config.yaml
        │
        ▼
  tools/codegen.py          ← Step 5: reads YAML, writes generated/
        │
        ▼
   generated/*.c/h          ← DID tables, safety wrappers, init sequence
        │
        ▼
  west build -b native_sim  ← Step 6: Zephyr CMake compiles everything
        │
        ▼
   zephyr/zephyr.elf        ← Step 7: runs as a Linux process
        │
        ▼
  pytest integration/       ← Step 8: Python sends ISO-TP UDS frames
```

The safety chain that ran on every DID request:

```
UDS 0x22 request (Read DID 0xF190)
    │
    ▼  Step 1: Does DID 0xF190 exist?          → yes, continue
    ▼  Step 2: Is current session allowed?      → yes (default session), continue
    ▼  Step 3: Is security level sufficient?    → yes (level 0, no auth needed), continue
    ▼  Step 4: Is read access permitted?        → yes, continue
    ▼  Step 5: Is response buffer large enough? → yes, continue
    │
    ▼
 vin_read() called → returns 17-byte VIN string
    │
    ▼
 UDS 0x62 positive response sent
```

---

## Next Steps

### Add your own DID

1. Open `examples/basic_ecu/diagnostics_config.yaml`
2. Add a new entry under `dids:`:

```yaml
dids:
  - id: "0xF1A0"
    name: "Odometer"
    length: 4
    sessions: [default, extended]
    read_handler: "odometer_read"
    security_level: 0
```

3. Regenerate: `python3 tools/codegen.py --config examples/basic_ecu/diagnostics_config.yaml --out examples/basic_ecu/generated/ --safety-wrappers --asil-level B`
4. Implement `odometer_read()` in `examples/basic_ecu/src/did_handlers_impl.c`:

```c
uds_status_t odometer_read(uint8_t *data, uint16_t *length, uint16_t max_length)
{
    uint32_t km = 12345U;
    data[0] = (uint8_t)(km >> 24);
    data[1] = (uint8_t)(km >> 16);
    data[2] = (uint8_t)(km >> 8);
    data[3] = (uint8_t)(km);
    *length = 4U;
    return UDS_STATUS_OK;
}
```

5. Rebuild and run: `west build -b native_sim examples/basic_ecu && west build -t run`

### Use the GUI configurator

The GUI has two modes in one app: a **YAML configurator** (⊕ CONFIG nav icon) and a
**live ECU dashboard** (all other nav icons).

**Start the GUI and bridge:**

```bash
# Terminal 1 — React dev server
cd gui
npm ci
npm run dev       # opens http://localhost:5173

# Terminal 2 — WebSocket bridge (no ECU needed in demo mode)
python3 gui/server/bridge.py --mode demo
```

**Configurator workflow (no ECU needed):**

1. Click the **⊕ CONFIG** icon in the left nav bar
2. Edit ECU name, add DIDs and DTCs using the form — the YAML preview updates live
3. Click **↓ DOWNLOAD** to save `diagnostics_config.yaml`, or **COPY** to clipboard
4. Click **⚙ GENERATE CODE** — this sends the YAML to `bridge.py`, which runs
   `tools/codegen.py` and streams the output line-by-line in a terminal panel
5. On success, the generated file list appears (e.g. `did_handlers.c`, `did_safety_wrappers.c`)
6. Rebuild firmware: `west build -b native_sim examples/basic_ecu`

> The bridge must be running (Terminal 2) for the Generate button to be active.
> YAML export and download work without the bridge.

### Generate tests from your YAML

`testgen.py` reads the same `diagnostics_config.yaml` and produces a complete test suite automatically.

```bash
# pytest suite (simulator mode — no hardware required):
python3 tools/testgen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

cd examples/basic_ecu/generated/tests && pytest . -v
```

For every DID in your config this generates: positive read, DID echo check, response length check, wrong-session gate (NRC 0x7F), security gate (NRC 0x33 if `security_level > 0`), and a 5-read stability test. For every DTC: set/clear/report lifecycle tests.

### Generate CANoe CAPL test scripts

If your team uses CANoe for ECU validation, one flag switches the output format:

```bash
# Generate pytest + CANoe CAPL:
python3 tools/testgen.py --capl \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

# Generate CAPL only:
python3 tools/testgen.py --capl --capl-only \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/
```

This produces `.can` files in `examples/basic_ecu/generated/tests/capl/`. Import them into a CANoe workspace:

1. **File → New → Test Setup → Add CAPL Test Module**
2. Select `ecu_diagnostics_test_suite.can`
3. Add each `test_did_XXXX.can` and `test_dtcs.can`
4. Run → F9

See `examples/basic_ecu/generated/tests/capl/README_CANOE.md` for the full import guide and security key configuration.

### Install the VS Code extension

The extension in `ide/vscode-extension/` gives you inline YAML validation and one-click codegen directly from the editor.

```bash
cd ide/vscode-extension
npm install
npx vsce package
code --install-extension eds-diagnostics-*.vsix
```

Open any `diagnostics_config.yaml` and:

- Hover over any key for ISO 14229 documentation and allowed values
- Invalid field values appear as red squiggles immediately (ASIL-B violations, wrong enums, duplicate IDs)
- `Ctrl+Shift+P` → **EDS: Run Codegen** to regenerate without leaving the editor

### Build a new ECU example

```bash
cp -r examples/basic_ecu examples/my_ecu
# Edit examples/my_ecu/diagnostics_config.yaml
# Edit examples/my_ecu/src/ for your application
west build -b native_sim examples/my_ecu -- -DDTC_OVERLAY_FILE=boards/native_sim/native_sim.overlay
```

### Flash real hardware (STM32 Nucleo-H743ZI2)

```bash
west build -b nucleo_h743zi2 examples/basic_ecu
west flash
```

---

## Troubleshooting

### `west init` fails — "repository not found"

The repo is currently private. You need access. Contact the EDS team or check your
GitHub credentials:
```bash
git ls-remote https://github.com/Xaloqi/EDS
```

### `west update` hangs or fails

Check your internet connection. If it fails partway through, just run `west update` again —
it resumes from where it stopped.

### Build error: `__device_dts_ord_DT_N_ALIAS_can0_ORD undeclared`

You forgot the overlay argument. Always build native_sim with:
```bash
west build -b native_sim examples/basic_ecu \
    -- -DDTC_OVERLAY_FILE=boards/native_sim/native_sim.overlay
```

### Build error: `can_frame_flags_t undeclared`

Your Zephyr version is newer than the pinned revision in `west.yml`. Run:
```bash
west update   # re-pins to the correct revision
```
If the error persists, check `west.yml` and confirm the Zephyr revision is pinned to `v3.7.0`.

### `codegen.py` fails: `ModuleNotFoundError: No module named 'jinja2'`

```bash
pip3 install -r tools/requirements.txt
```

### Unit tests fail: `37 tests expected, X found`

The test count in `scripts/build_tests.sh` must match the actual test files. Run:
```bash
bash build_tests.sh 2>&1 | tail -5
```
If tests are missing, confirm you haven't accidentally deleted files from `tests/unit_runnable/`.

### `pytest` not found

```bash
pip3 install pytest
```

### `testgen.py --capl` fails: `TemplateError: unexpected '>'`

A Jinja2 CAPL template contains `>>` (bit-shift) inside a `{{ }}` expression. This is a known Jinja2 limitation. Verify the CAPL templates in `tools/templates/` are up to date:

```bash
ls tools/templates/*.can.j2
# Should show: ecu_diagnostics_test_suite.can.j2  test_did_XXXX.can.j2  test_dtcs.can.j2
```

If any file is missing, pull the latest from the repository.

### VS Code extension: hover docs or squiggles not appearing

Confirm the file is detected as YAML (check the language indicator in the VS Code status bar). Run `Ctrl+Shift+P` → **EDS: Validate diagnostics_config.yaml** to force a validation pass. Verify `eds.enableDiagnostics` and `eds.enableHoverDocs` are `true` in VS Code settings.

### native_sim build succeeds but `west build -t run` immediately exits

This is normal if there are no pending CAN messages. The ECU waits for ISO-TP traffic.
Run the integration tests in a second terminal to send traffic while it's running.

---

## Reference

| Command | What it does |
|---|---|
| `python3 tools/codegen.py --config CONFIG --out OUTPUT_DIR --safety-wrappers --asil-level B` | Generate C code from YAML |
| `west build -b native_sim examples/basic_ecu -- -DDTC_OVERLAY_FILE=boards/native_sim/native_sim.overlay` | Build simulator firmware |
| `west build -t run` | Run simulator |
| `west build -b nucleo_h743zi2 examples/basic_ecu` | Cross-compile for Nucleo |
| `west flash` | Flash to connected hardware |
| `bash build_tests.sh` | Run 37 unit tests |
| `pytest tests/integration/ -v` | Run Python integration tests |
| `cd gui && npm ci && npm start` | Start GUI configurator |
| `python3 tools/testgen.py --config CONFIG --out OUT` | Generate pytest test suite |
| `python3 tools/testgen.py --capl --config CONFIG --out OUT` | Generate pytest + CANoe CAPL |
| `python3 tools/testgen.py --capl --capl-only --config CONFIG --out OUT` | Generate CANoe CAPL only |
| `cd examples/basic_ecu/generated/tests && pytest . -v` | Run generated pytest suite |
| `cd ide/vscode-extension && npx vsce package` | Build VS Code extension |

For the full YAML schema and all five DID patterns, see [`AI_CONTEXT.md`](AI_CONTEXT.md).
