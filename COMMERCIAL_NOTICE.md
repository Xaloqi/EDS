# Commercial Notice

Xaloqi EDS uses a dual-license model. This file makes the license
boundary explicit for anyone reading or contributing to this repository.

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
| `tools/codegen.py` | YAML-to-C code generator |
| `tools/testgen.py` | Automated pytest + CAPL test generator |
| `tools/config_parser.py` | Configuration parser used by codegen |
| `tools/templates/` | All Jinja2 code generation templates |
| `ide/vscode-extension/` | VS Code extension for EDS |
| `docs/EDS_Safety_Manual_*` | Safety Manual (Professional tier) |
| `docs/MISRA_DEVIATION_LOG.md` | MISRA C Deviation Log (Professional tier) |
| `docs/EDS_Requirements_Traceability_Matrix.csv` | RTM (Professional tier) |

Using any of the above files in a project — commercial or otherwise —
without a valid Xaloqi commercial license is a violation of Xaloqi's
intellectual property rights.

---

## How to get a commercial license

Three tiers are available at **https://xaloqi.com**:

| Tier | Price | Includes |
|---|---|---|
| Developer | €690 / year | Runtime + codegen + testgen + templates + VS Code extension + Integration Guide |
| Professional | €1,990 / year | Developer tier + Safety Manual + MISRA Log + RTM + priority support |

A commercial license grants you the right to use Xaloqi EDS in one
proprietary project per seat, ship binary firmware to your customers,
and keep your firmware source code private.

Full license terms: `LICENSE_COMMERCIAL.txt` in this repository.

---

## Questions

Licensing enquiries: contact@xaloqi.com
