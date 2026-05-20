# Xaloqi EDS — Commercial License Onboarding

This guide is for customers who purchased a **Developer** or **Professional** license on Polar.
It covers what you received, how to activate your license, and how to verify everything works.

For general setup (Zephyr workspace, codegen basics, building examples) see
[GETTING_STARTED.md](GETTING_STARTED.md) first.

---

## What You Received

After purchase you will have received a download link containing a ZIP file.

### Developer license ZIP contents

```
xaloqi-eds-developer-v1.7.0.zip
├── tools/
│   ├── _license.py          ← License validation module (required for codegen)
│   └── templates/           ← Jinja2 templates for all generated C/H files
│       ├── did_handlers.c.j2
│       ├── did_handlers.h.j2
│       ├── did_safety_wrappers.c.j2
│       ├── did_safety_wrappers.h.j2
│       ├── generated_config.h.j2
│       ├── safety_config.h.j2
│       ├── routine_handlers.c.j2
│       ├── routine_handlers.h.j2
│       ├── uds_init.c.j2
│       └── uds_init.h.j2
└── YOUR_LICENSE_KEY.txt     ← Your personal JWT license key
```

### Professional license ZIP contents

Everything in Developer, plus:

```
xaloqi-eds-professional-v1.7.0.zip
├── tools/
│   ├── _license.py
│   └── templates/           ← Same Jinja2 templates
├── gui/                     ← React/TypeScript live ECU dashboard + YAML configurator
├── ide/
│   └── vscode-extension/    ← VS Code extension (YAML validation, hover docs, Run Codegen)
└── YOUR_LICENSE_KEY.txt
```

### TestLab (xaloqi-tester)

TestLab is a separate Python package — `pip install xaloqi-tester` — that provides
`CanBus` and `DoipBus` pytest fixtures for running the generated test suite against
**real compiled firmware** over a physical CAN interface or DoIP socket.
See [TESTLAB_GUIDE.md](TESTLAB_GUIDE.md) for installation and usage.

---

## Step 1 — Install the License Files

Copy `_license.py` and the `templates/` directory from the ZIP into your local EDS
`tools/` directory:

```bash
# From the extracted ZIP
cp _license.py    /path/to/eds-workspace/EDS/tools/
cp -r templates/  /path/to/eds-workspace/EDS/tools/

# Verify
ls EDS/tools/_license.py          # must exist
ls EDS/tools/templates/*.j2       # should list 10+ .j2 template files
```

> **Never commit `_license.py` or `YOUR_LICENSE_KEY.txt` to a public repository.**
> Add them to `.gitignore` if they are inside the repo directory.

---

## Step 2 — Activate Your License Key

Your key is in `YOUR_LICENSE_KEY.txt`. It is a JWT string starting with `eyJ`.

```bash
cd EDS
python3 tools/activate.py --key <paste-your-key-here>
```

On success you will see:

```
============================================================
  Xaloqi EDS — License Activated
============================================================
  Email    : your@email.com
  Tier     : Developer
  Expires  : 2027-05-20  (365 days remaining)
  Saved to : /home/you/.xaloqi/license.key
============================================================

  Run codegen to confirm activation:
    python3 tools/codegen.py --config diagnostics_config.yaml --out generated/
```

The key is stored at `~/.xaloqi/license.key`. It persists across shell sessions —
you only need to activate once per machine.

### Checking your license status

```bash
python3 tools/activate.py --check
```

### Removing a license (e.g. before transferring to a new machine)

```bash
python3 tools/activate.py --deactivate
```

---

## Step 3 — Verify Codegen Works

Run codegen on the `basic_ecu` example with `--safety-wrappers`:

```bash
python3 tools/codegen.py \
    --config  examples/basic_ecu/diagnostics_config.yaml \
    --out     examples/basic_ecu/generated/ \
    --safety-wrappers \
    --asil-level B
```

Expected output:

```
[codegen] License OK — Developer (365 days remaining)
[codegen] Loaded 5 DIDs, 3 DTCs, 3 routines
[codegen] Generated: examples/basic_ecu/generated/did_handlers.c
[codegen] Generated: examples/basic_ecu/generated/did_handlers.h
[codegen] Generated: examples/basic_ecu/generated/did_safety_wrappers.c
[codegen] Generated: examples/basic_ecu/generated/did_safety_wrappers.h
[codegen] Generated: examples/basic_ecu/generated/generated_config.h
[codegen] Generated: examples/basic_ecu/generated/safety_config.h
[codegen] Generated: examples/basic_ecu/generated/routine_handlers.c
[codegen] Generated: examples/basic_ecu/generated/routine_handlers.h
[codegen] Generated: examples/basic_ecu/generated/uds_init.c
[codegen] Generated: examples/basic_ecu/generated/uds_init.h
[codegen] Done.
```

If you see `[codegen] WARNING: No license found — running in community mode`, the key
was not found. Check the [Troubleshooting](#troubleshooting) section below.

---

## Using Your License in CI / Docker

Set the `XALOQI_LICENSE_KEY` environment variable instead of the file:

```bash
export XALOQI_LICENSE_KEY=<your-key>
python3 tools/codegen.py --config config.yaml --out generated/ --safety-wrappers --asil-level B
```

In GitHub Actions:

```yaml
- name: Run codegen (commercial)
  env:
    XALOQI_LICENSE_KEY: ${{ secrets.XALOQI_LICENSE_KEY }}
  run: |
    python3 tools/codegen.py \
      --config examples/basic_ecu/diagnostics_config.yaml \
      --out    examples/basic_ecu/generated/ \
      --safety-wrappers --asil-level B
```

Store the key as a repository secret (`Settings → Secrets → Actions → New repository secret`,
name: `XALOQI_LICENSE_KEY`, value: the full JWT string).

---

## Tier Comparison

| Feature | Community (GPL) | Developer | Professional |
|---|:---:|:---:|:---:|
| UDS/ISO-TP C runtime stack | ✓ | ✓ | ✓ |
| `codegen.py` (community mode — no templates) | ✓ | ✓ | ✓ |
| `testgen.py` (pytest + CANoe CAPL generation) | ✓ | ✓ | ✓ |
| Jinja2 templates (`--safety-wrappers`, ASIL-B output) | — | ✓ | ✓ |
| License key + offline activation | — | ✓ | ✓ |
| All 11 ECU examples with pre-committed generated output | ✓ | ✓ | ✓ |
| GUI YAML configurator + live ECU dashboard | — | — | ✓ |
| VS Code extension (inline validation, Run Codegen) | — | — | ✓ |
| Priority email support | — | — | ✓ |
| xaloqi-tester (TestLab, real hardware testing) | — | add-on | add-on |

> Community tier is the public GPL repo. It ships with pre-committed generated output for
> all 11 ECU examples so you can build and run firmware without a license — but you cannot
> regenerate the ASIL-B safety wrappers from your own YAML without a Developer or
> Professional key.

---

## What the Templates Unlock

Without a license (`--safety-wrappers` omitted), codegen produces basic DID/DTC tables
but skips the ASIL-B safety layer. With a Developer or Professional license:

| Generated file | Community | Licensed |
|---|:---:|:---:|
| `did_handlers.c/h` — DID lookup table | ✓ | ✓ |
| `generated_config.h` — timing + dimension macros | ✓ | ✓ |
| `did_safety_wrappers.c/h` — ASIL-B access validation | — | ✓ |
| `safety_config.h` — `GEN_ASIL_LEVEL`, session check flags | — | ✓ |
| `routine_handlers.c/h` — 0x31 RoutineControl dispatch | — | ✓ |
| `uds_init.c/h` — full init sequence with self-test (ISO 26262-6 §9.4.3) | — | ✓ |

The ASIL-B wrappers enforce session gates, security levels, and buffer-length checks on
every DID read/write — without them your application code calls the C handlers directly
with no diagnostic-layer protection.

---

## Professional Tier — GUI Setup

```bash
cd gui
npm ci
npm run dev          # starts http://localhost:5173

# In a second terminal: WebSocket bridge (connects GUI to running ECU)
python3 gui/server/bridge.py --mode demo    # demo mode — no ECU needed
```

Open `http://localhost:5173`. Click **⊕ CONFIG** in the left nav to open the YAML
configurator. The **⚙ GENERATE CODE** button calls `tools/codegen.py` via the bridge
and streams the output live.

For connecting to a real ECU binary, see the bridge `--help` output.

---

## Professional Tier — VS Code Extension

```bash
cd ide/vscode-extension
npm install
npx vsce package          # produces eds-diagnostics-x.y.z.vsix
code --install-extension eds-diagnostics-*.vsix
```

Open any `diagnostics_config.yaml` file in VS Code. You should see:
- Inline red squiggles for ASIL-B violations, duplicate IDs, unknown enum values
- Hover documentation for every YAML key (ISO 14229-1 reference included)
- `Ctrl+Shift+P` → **EDS: Run Codegen** to regenerate without leaving the editor

---

## Troubleshooting

### `[codegen] WARNING: No license found — running in community mode`

codegen did not find a key. Check in order:
1. `~/.xaloqi/license.key` exists: `cat ~/.xaloqi/license.key`
2. `XALOQI_LICENSE_KEY` environment variable is set (if using env var mode)
3. `tools/_license.py` is present: `ls tools/_license.py`
4. Re-run activation: `python3 tools/activate.py --key <your-key>`

### `ERROR: tools/_license.py not found`

`_license.py` is not in `tools/`. Copy it from the ZIP:
```bash
cp /path/to/zip-contents/_license.py tools/
```

### `ERROR: License key JWT signature invalid`

The key may have been truncated during copy-paste. Copy the full string from
`YOUR_LICENSE_KEY.txt` — it begins with `eyJ` and is typically 300–500 characters long.
Do not add line breaks.

### `ModuleNotFoundError: No module named 'jwt'` or `'cryptography'`

```bash
pip install pyyaml jinja2 cryptography PyJWT
# or
pip install -r tools/requirements.txt
```

### License expired

Run `python3 tools/activate.py --check` to see days remaining and the grace period.
Renew at [https://xaloqi.com](https://xaloqi.com) to receive a new key, then activate
with the new key.

### Templates not found after copying

codegen looks for templates at `tools/templates/*.j2`. Verify:
```bash
ls tools/templates/*.j2 | head -5
```
If the directory is missing or empty, re-extract the ZIP and copy the `templates/`
folder to `tools/`.

---

## License Terms Summary

- **Per-seat:** one license key per developer workstation. CI usage (GitHub Actions,
  GitLab CI, Jenkins) counts as one seat per CI organization, not per runner.
- **No source distribution:** you may not redistribute `_license.py`, the Jinja2
  templates, or the GUI/VS Code extension source to third parties.
- **Generated output:** C files produced by codegen (everything in `generated/`) are
  yours — no restrictions on use, redistribution, or embedding in commercial firmware.
- **Annual renewal:** keys expire after 12 months. A 30-day grace period allows
  continued use while you renew.

For the full license text, see the `LICENSE_COMMERCIAL.txt` file included in your ZIP.

---

## Support

- **Email:** support@xaloqi.com
- **GitHub Issues (community/bugs):** [github.com/Xaloqi/EDS/issues](https://github.com/Xaloqi/EDS/issues)
- **Professional tier:** priority response within 1 business day
