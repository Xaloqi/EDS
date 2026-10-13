# Open Reference Implementation Grant

**Version 1.0 · published 2026-10-13 · granted by Xaloqi**

This is a standing, public, one-way grant. There is nothing to sign, nobody to
ask, and no registration. If your work meets the condition below, the grant
already applies to you.

---

## Why this exists

EDS is open-core. The runtime is GPL-2.0, so an open-source project can already
use it. **The code generator is not** — and the generator is the part that makes
EDS what it is: one `diagnostics_config.yaml` that produces the handlers, the
tests, the SOVD CDA descriptor and the CANoe suite together.

That left open-source reference implementations in an odd position. They could
use the runtime and then hand-write everything the generator would have produced,
which is the least interesting version of this software and not the thing we
argue for in public.

This grant closes that gap. It costs us nothing, because **an open reference
implementation was never going to be a sale.** It is the thing suppliers copy —
and each of those suppliers is a sale.

---

## What is granted

**Free, perpetual use of the commercially-licensed Xaloqi toolchain** to produce
output for a qualifying open-source project:

- `tools/codegen.py` — generation of diagnostic handlers, headers, safety
  wrappers, and the SOVD CDA configuration descriptor
- `tools/testgen.py` — the generated pytest suite and the CANoe CAPL suite
- `SovdTester` and the TestLab Pro runner actions, for testing that project

This covers **generating, building, testing, publishing and distributing** that
project, including by its downstream users.

No fee. No seat count. No expiry. No reporting.

## The condition

**The generated output must be published, as source, under an
[OSI-approved licence](https://opensource.org/licenses), in a project whose
source is publicly available to anyone.**

That is the whole test. It is deliberately objective: either a person who does
not know you can read the source under an OSI-approved licence, or they cannot.

## What is **not** granted

- **Anything whose source is not published.** A fork taken into a product needs
  an ordinary commercial licence, exactly like any other product.
- **The ISO 26262 safety work products.** The HARA, the Tool Qualification
  report and the safety case are not covered, in any circumstance.
- **Re-licensing or resale of the toolchain itself.** You may use it to generate
  your project's output; you may not redistribute the generator as your own, or
  as part of a competing product.
- **Any claim of certification, endorsement, or safety qualification** on our
  part. Using EDS does not make your project ASIL-anything, and this grant is
  not a statement about your project's fitness for any purpose.

---

## Irrevocability — the part that matters

**Any version of EDS or the Xaloqi toolchain released while this grant is
published is granted under it permanently, for that version.** Withdrawing or
narrowing this grant later cannot reach back and strip a version that was
already covered.

We may stop applying it to *future* versions. We cannot strand a project that
already adopted one.

This is deliberate, and it is most of the point. The reason open-source projects
refuse to build on a dual-licensed vendor component is rarely price — it is
*"what happens when they change the terms."* A grant you can revoke answers that
question badly, so this one cannot be.

---

## Relationship to the GPL

This grant sits **alongside** the GPL-2.0 licence on the runtime and changes
nothing about it. If you are building an open-source project you are already
satisfying the GPL; this adds the toolchain, which the GPL never covered.

If your project is **not** open-source, this grant does not apply and the GPL's
own terms are what bind you — see [COMMERCIAL_NOTICE.md](COMMERCIAL_NOTICE.md).

---

## Using it

Use it. There is nothing to apply for.

Two requests, neither of them conditions:

1. **Tell us, if you feel like it** — open an issue on
   [Xaloqi/EDS](https://github.com/Xaloqi/EDS/issues) or write to
   `contact@xaloqi.com`. Knowing who relies on this changes what we prioritise,
   and we would rather hear from you than discover it later.
2. **Attribute it** the way you would any dependency. A line in your README is
   plenty.

If you are unsure whether your project qualifies, ask. A question costs us
nothing and a wrong assumption costs you more.

---

## Scope of this document

This grant covers the commercial toolchain's use for qualifying projects. It is
not a warranty, not a support commitment, and not advice about your own
licensing obligations. The runtime's own terms are in [LICENSE](LICENSE); the
commercial boundary is in [COMMERCIAL_NOTICE.md](COMMERCIAL_NOTICE.md). Where
this document and a signed commercial agreement disagree, the signed agreement
governs for the party that signed it.
