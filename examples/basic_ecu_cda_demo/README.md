# basic_ecu_cda_demo

This is `examples/basic_ecu`'s diagnostics layer, forked into its own example
rather than edited in place, plus exactly two additions. It exists only to be
the EDS side of `scripts/demo_can_sovd_cda_interop.sh` — the one-command
reference target that proves real EDS interoperates with the real Eclipse
OpenSOVD Classic Diagnostic Adapter (CDA) over real CAN.

## Why a fork, not an edit to basic_ecu

`basic_ecu` is the canonical minimal example used throughout this repo's own
documentation as "the 5-DID example" — README.md, docs/GETTING_STARTED.md,
docs/PERFORMANCE.md's benchmark table, docs/INTEGRATION_GUIDE.md and others
all cite it by that exact count, in some cases alongside measured numbers
(docs/PERFORMANCE.md's cycle counts) tied to that specific configuration.
Adding demo-only entries there would quietly invalidate all of that. Forking
avoids it entirely: `basic_ecu` is untouched, and everything below is scoped
to this directory.

## The two additions, and why each exists

Both are disclosed here, in the script, and in this file's
`diagnostics_config.yaml` comments — the policy for this demo is to never
carry a fixture-matching choice quietly.

1. **DID 0xF100 ("Identification"), value `0xFF0000`.** A real, general
   capability — `tools/codegen.py`'s `default_value` DID field, a fixed
   compile-time value instead of the usual all-zero stub. The *specific*
   value is chosen because it's exactly what the CDA's own shipped
   `FLXC1000.mdd` test fixture pattern-matches to select that ECU's
   `Boot_Variant` — the variant that exposes SecurityAccess. Without it,
   the CDA has no SecurityAccess service to route to over SOVD at all.

2. **Routine 0x1001 ("SelfTest").** A real RoutineControl Start capability,
   at an ID chosen to equal the ID the CDA's fixture declares for its own
   "SelfTest" operation. SOVD resolves `/operations/<name>` against
   whatever ODX database the CDA has loaded — not against anything EDS
   reports on the wire — so exercising RoutineControl through the CDA's
   SOVD API requires this ID to line up with the fixture, not with
   `basic_ecu`'s own `ECU_SelfTest` at 0xFF00 (which the fixture doesn't
   know about and can't route to).

## Why not generate a custom ODX fixture instead

That would be the cleaner fix — EDS reporting its own real identity instead
of borrowing the fixture's "Flux capacitor" one — and was considered first.
It isn't available: the CDA's own fixtures are generated as `.pdx`
(`testcontainer/odx/generate.py`, Apache-2.0), but converting `.pdx` to the
`.mdd` format the CDA actually loads requires a separate Java "ODX
converter" tool that the CDA project itself cannot distribute
("Unfortunately for legal reasons, we can't provide a binary release at the
moment" — `testcontainer/README.md` in their repo). That gate is the same
shape as this project's own ISO 22901-1 purchase decision (see
xaloqi-knowledge's `lessons/run-043`): schema-adjacent tooling that isn't
freely redistributable. Matching the shipped fixture's IDs, fully disclosed,
is the available option — not a shortcut taken in place of a better one that
was skipped.

## What this buys, concretely

With both additions, `scripts/demo_can_sovd_cda_interop.sh` exercises, all
against unmodified EDS core/platform code:

- Session control (SecurityAccess and RoutineControl both require a
  non-default session — `core/uds_services/service_0x27.c` enforces this).
- SecurityAccess with real crypto — EDS's own AES-128-CMAC seed/key exchange
  (`core/uds_security_algo.c`), routed through the CDA's SOVD
  `/modes/security` API.
- RoutineControl (SID 0x31) through the CDA's SOVD `/operations` API.
- A multi-frame ISO-TP read (VIN, DID 0xF190, unchanged from `basic_ecu`).

See the script itself, and
`xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md`, for the full wire
traces this was verified against before being committed here.
