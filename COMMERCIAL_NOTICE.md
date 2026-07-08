# Commercial Notice

Xaloqi EDS uses a dual-license model. This file makes the license
boundary explicit for anyone reading or contributing to this repository.

---

## The short version

**You can clone this repo and build, run, study, and modify the full
diagnostics stack and every bundled example for free, under GPL v2.** The
examples ship their generated C (`examples/*/generated/`), so `west build`
works with no license.

A commercial license is required for two things:

1. **Running the code generator on your own config** — `tools/codegen.py`
   is readable and runs, but the Jinja2 templates it needs
   (`tools/templates/`) are the commercial deliverable and are not in this
   repo. Running codegen without them fails with a message pointing here.
2. **Shipping proprietary (closed-source) firmware** — GPL v2 requires you
   to release the source of anything you link the runtime into; a
   commercial license lifts that so you can keep your firmware private.

If neither applies to you, you owe nothing. Details below.

---

## What is free (GPL v2)

The following components are open source under the GNU General Public
License version 2 (`LICENSE` at repo root):

| Directory | Contents |
|---|---|
| `core/` | UDS server, session manager, security, safety, service handlers |
| `transport/` | ISO-TP, CAN transport, Zephyr CAN driver integration |
| `config/` | DID database, DTC database, routine database |
| `platform/` | Zephyr port, NVM store, flash ops, mutex, timer, watchdog |

GPL v2 means: you may use, study, modify, and redistribute these files,
provided that any combined work you distribute — including ECU firmware
that links against this stack — is also distributed under GPL v2, with
source code made available to recipients.

**If you cannot meet the GPL v2 conditions** (for example, because your
ECU firmware is proprietary and you cannot release its source code),
you must purchase a commercial license. See below.

Examples in `examples/` are licensed under Apache 2.0 and may be used
freely in both open-source and proprietary projects without GPL
obligations.

---

## What requires a commercial license

The following components are **not** covered by GPL v2. They require a
paid commercial license from Xaloqi:

| File / Directory | Description |
|---|---|
| `tools/codegen.py` | Publicly readable. Generating code for proprietary firmware requires a commercial license. Functional use requires `tools/templates/` (Developer/Professional tier). |
| `tools/testgen.py` | Automated pytest + CAPL test generator |
| `tools/config_parser.py` | Publicly readable. Used by `codegen.py` — same license requirement applies. |
| `tools/templates/` | All Jinja2 code generation templates |
| `ide/vscode-extension/` | VS Code extension for EDS |
| `tools/arxml_parser.py`, `tools/mcp_server.py`, `tools/eds_ai.py` | ARXML importer, MCP server, AI assistant (Developer tier) |

The safety documentation package — Safety Manual (EDS-SM-001), HARA, Tool
Qualification Argument, MISRA Deviation Log, Requirements Traceability
Matrix, and OEM Key Provisioning Guide — is **not in this repository**. It
is delivered as part of the **Professional** tier ZIP.

Using any of the above files in a project — commercial or otherwise —
without a valid Xaloqi commercial license is a violation of Xaloqi's
intellectual property rights.

---

## How to get a commercial license

EDS is available in two tiers at **https://xaloqi.com**:

| Tier | Price | Includes |
|---|---|---|
| Developer | €690 / year | Runtime + codegen + testgen + templates + ARXML importer + VS Code extension + MCP/AI tools + Integration Guide |
| Professional | €1,990 / year | Developer tier + 68-test integration harness + safety documentation package (Safety Manual, HARA, TQA, MISRA Log, RTM, Key Provisioning Guide) + priority support |

Xaloqi TestLab (the validation campaign runner) and the discounted
**Developer + TestLab** and **Professional + TestLab** bundles are also
available at xaloqi.com.

A commercial license grants you the right to use Xaloqi EDS in one
proprietary project per seat, ship binary firmware to your customers,
and keep your firmware source code private.

Full license terms: `LICENSE_COMMERCIAL.txt` in this repository.

---

## Questions

Licensing enquiries: contact@xaloqi.com
