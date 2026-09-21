"""
tests/test_composite_action_expressions.py — expressions in a composite
action manifest may only use contexts that exist there.

WHY THIS EXISTS (#290)
----------------------
`.github/actions/setup-zephyr-sdk/action.yml` failed its first CI run with:

    Unrecognized named-value: 'env'.
    Located at position 1 within expression: env.ZEPHYR_SDK_VERSION

The offending text was not in a step. It was in an input's **description**,
illustrating how callers should pass the value in. The runner parses
`${{ ... }}` anywhere in the manifest — prose included — so an example
written literally takes the whole action down, and every job that uses it
fails in about five seconds with a template error rather than anything
resembling the real problem.

`env` genuinely is not available inside a composite action: workflow-level
environment variables are not propagated to its steps' expression context.
That is why `sdk-version` is an input in the first place. The irony is that
the comment explaining the design is what broke it.

WHAT THIS GUARDS
----------------
Every expression in every composite action manifest references a context
that actually exists there. Cheap, static, needs no runner — and it turns a
CI round-trip into a local failure.

Run:  XALOQI_LICENSE_SKIP=1 pytest tests/test_composite_action_expressions.py -v
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent
_ACTIONS_DIR = _REPO_ROOT / ".github" / "actions"

#: Contexts a composite action manifest may reference.
#: Deliberately excludes `env` and `secrets`, neither of which the runner
#: provides there — that omission is the whole point of this check.
_ALLOWED_CONTEXTS = frozenset({
    "inputs", "steps", "runner", "github", "job",
    "strategy", "matrix", "needs", "vars",
})

_EXPR = re.compile(r"\$\{\{(.+?)\}\}", re.S)
_ROOT_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*")


def _manifests() -> list[Path]:
    if not _ACTIONS_DIR.is_dir():
        return []
    return sorted(_ACTIONS_DIR.glob("*/action.yml")) + sorted(
        _ACTIONS_DIR.glob("*/action.yaml")
    )


def test_there_is_something_to_check() -> None:
    """Fail loudly rather than pass vacuously if the layout moves."""
    assert _manifests(), (
        f"no composite action manifests found under {_ACTIONS_DIR} — if they "
        "have moved, this guard is protecting nothing"
    )


@pytest.mark.parametrize(
    "manifest", _manifests() or [None], ids=lambda p: p.parent.name if p else "none"
)
def test_expressions_use_only_contexts_that_exist(manifest: Path | None) -> None:
    if manifest is None:
        pytest.skip("no manifests")

    text = manifest.read_text(encoding="utf-8")
    problems = []
    for m in _EXPR.finditer(text):
        expr = m.group(1).strip()
        root = _ROOT_NAME.match(expr)
        if root is None:
            continue
        name = root.group(0)
        if name in _ALLOWED_CONTEXTS:
            continue
        lineno = text[: m.start()].count("\n") + 1
        hint = ""
        if name == "env":
            hint = (
                " — workflow-level env is not propagated into a composite "
                "action's expression context; pass the value as an input. "
                "This also applies to prose: do not illustrate an expression "
                "in a description, the runner parses it."
            )
        problems.append(
            f"{manifest.relative_to(_REPO_ROOT)}:{lineno}: "
            f"'{name}' is not a context available in a composite action{hint}"
        )

    assert not problems, "\n  ".join([""] + problems)
