# Xaloqi EDS — Code Generation Templates

This directory contains the Jinja2 templates that drive `tools/codegen.py`.

## Availability

Templates are **not included in the community (free) tier** of Xaloqi EDS.

They are delivered with **Developer** and **Professional** licenses as part of the
commercial toolchain ZIP. Purchase at **[xaloqi.com](https://xaloqi.com)**.

---

## What the templates produce

Running `python3 tools/codegen.py --config <yaml> --out <dir> --safety-wrappers --asil-level B`
with the templates installed generates 10 production-ready C/H files:

| Generated file | Contents |
|---|---|
| `generated_config.h` | Compile-time constants — CAN IDs, DID count, timing parameters |
| `did_handlers.c/.h` | DID read/write handler stubs — implement these in your application |
| `did_safety_wrappers.c/.h` | ASIL-B 5-step validation chain — one wrapper per DID, do not edit |
| `routine_handlers.c/.h` | Routine start/stop/results stubs |
| `uds_init.c/.h` | Full UDS + DTC + DID + flash-ops initialisation sequence |
| `safety_config.h` | Compile-time ASIL-B assertions — safety gates |

Plus (with `--test-gen`): a complete pytest test suite + CANoe CAPL scripts in `generated/tests/`.

---

## After purchase

1. Extract the delivery ZIP into this repo root
2. Run `python3 tools/activate.py --key YOUR-LICENSE-KEY`
3. This directory will contain the 17 `.j2` template files
4. `python3 tools/codegen.py` will run normally

See `INSTALL.md` (delivered with the ZIP) for full setup instructions.

---

## Evaluation

The `examples/basic_ecu/generated/` and `examples/basic_ecu_freertos/generated/`
directories contain pre-committed codegen output. You can inspect these files now
to evaluate the quality and structure of what codegen produces — before purchasing.

The `examples/basic_ecu/` example also includes the full `diagnostics_config.yaml`
showing the YAML schema. You can write your own YAML and validate it against
the schema in the VS Code extension (Developer tier) before generating.
