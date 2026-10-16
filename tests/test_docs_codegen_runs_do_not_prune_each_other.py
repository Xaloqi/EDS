"""
tests/test_docs_codegen_runs_do_not_prune_each_other.py

codegen reconciles --out against its previous run's manifest and deletes what
the new run did not produce (EDS#292, Step 4E) — deliberately, so a directory
always describes one configuration. INSTALL.md Step 5 then told customers to
run codegen with --test-gen and, as an "optional" second command into the same
--out, with --sovd but without --test-gen. Following it literally deleted the
36-file test suite Step 6 runs ("cd: …/generated/tests: No such file or
directory") (2026-10-16 validation campaign, F4).

Rule checked here, for every shipped doc: a codegen command must not drop a
conditional output that an earlier command in the same doc wrote to the same
--out (adding one is fine — nothing is deleted).

Run:  pytest tests/test_docs_codegen_runs_do_not_prune_each_other.py -v
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_FENCE = re.compile(r"```(?:bash|sh|shell|console)?\n(.*?)```", re.S)
#: Outputs codegen writes only when asked — and prunes when not asked.
_CONDITIONAL = ("--test-gen", "--sovd", "--gui-types")


def _codegen_runs(text: str) -> list[tuple[str, frozenset]]:
    runs = []
    for block in _FENCE.findall(text):
        block = re.sub(r"\\\s*\n\s*", " ", block)
        for line in block.splitlines():
            toks = line.split()
            if not any(t.endswith("codegen.py") for t in toks) or "--out" not in toks:
                continue
            out = toks[toks.index("--out") + 1].rstrip("/")
            runs.append((out, frozenset(t for t in toks if t in _CONDITIONAL)))
    return runs


def test_no_doc_prunes_its_own_earlier_codegen_output() -> None:
    problems = []
    for doc in [_REPO_ROOT / "INSTALL.md", _REPO_ROOT / "README.md",
                *sorted((_REPO_ROOT / "docs").glob("*.md"))]:
        if not doc.is_file():
            continue
        seen: dict[str, frozenset] = {}
        for out, flags in _codegen_runs(doc.read_text(encoding="utf-8")):
            dropped = seen.get(out, frozenset()) - flags
            if dropped:
                problems.append(
                    f"{doc.relative_to(_REPO_ROOT)}: --out {out} written with "
                    f"{sorted(seen[out])} then without {sorted(dropped)} — the "
                    f"second run deletes what only the first produced"
                )
            seen[out] = seen.get(out, frozenset()) | flags
    assert not problems, "\n".join(problems)


def test_the_check_sees_install_md_step_5() -> None:
    """Guard against the check silently matching nothing (run-041)."""
    runs = _codegen_runs((_REPO_ROOT / "INSTALL.md").read_text(encoding="utf-8"))
    assert any(out == "examples/bms_ecu/generated" for out, _ in runs), runs
