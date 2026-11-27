#!/usr/bin/env python3
"""Fail if a production build exports or declares a test-only accessor (EDS#599).

Twelve functions are called by nothing but tests/. They are compiled and
declared only under EDS_TEST_HOOKS, which the test and harness builds define
and no production build does. Rule: a production build is one WITHOUT
EDS_TEST_HOOKS (and without UNIT_TEST, which is a separate switch).

For the six sources that hold them:
  * compiled without EDS_TEST_HOOKS, no object defines any of the twelve, and a
    translation unit that includes their headers can declare a variable of each
    name (it could not if a header still declared the function);
  * compiled with -DEDS_TEST_HOOKS=1, all twelve are defined and declared, so
    the check above is not vacuous.

It checks the race-point hook the same way (EDS#197): core/eds_race_point.h
expands EDS_RACE_POINT(id) to nothing without UNIT_TEST. A probe that uses the
macro must reference eds_race_point_hook with UNIT_TEST and not without it,
and a build that defines EDS_RACE_POINT itself must stop at the header's #error.

Usage: verify_no_test_hooks.py [EDS_ROOT]
"""
import pathlib
import subprocess
import sys
import tempfile

SOURCES = [
    "core/uds_safety.c",
    "core/uds_security_algo.c",
    "core/uds_server.c",
    "core/uds_session.c",
    "core/uds_session_stats.c",
    "transport/isotp.c",
]
HEADERS = [
    "uds_safety.h", "uds_security_algo.h", "uds_server.h", "uds_session.h",
    "uds_session_stats.h", "isotp.h",
]
TEST_ONLY = [
    "isotp_reset",
    "uds_safety_check_service_in_session",
    "uds_safety_reset_counters",
    "uds_security_algo_derive_key",
    "uds_security_algo_get_sequence",
    "uds_server_get_counters",
    "uds_server_pending_active",
    "uds_session_is_default",
    "uds_session_stats_get",
    "uds_session_stats_init",
    "uds_session_stats_on_change",
    "uds_session_stats_record_security",
]
INCLUDE_DIRS = ["core", "core/uds_services", "transport", "config", "platform",
                "platform/zephyr", "examples/basic_ecu/generated", "transport/doip"]
# Never UNIT_TEST. CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1 is a firmware dev image (the
# examples' setting): without UNIT_TEST a host build would otherwise count as a
# production image and stop on the placeholder-key gate (SEC-KEY-GATE-01).
BASE = ["gcc", "-std=c11", "-c", "-Wno-unused-function", "-DNVM_STORE_HOST_MOCK=1",
        "-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1", "-DEDS_MSG_BUF_MAX_STACK_BYTES=8192"]


def defined_symbols(root, tmp, extra):
    inc = [f"-I{root / d}" for d in INCLUDE_DIRS]
    found = set()
    for i, src in enumerate(SOURCES):
        obj = tmp / f"{i}.o"
        subprocess.run(BASE + extra + inc + [str(root / src), "-o", str(obj)], check=True)
        out = subprocess.run(["nm", "--defined-only", "-g", str(obj)],
                             capture_output=True, text=True, check=True).stdout
        found.update(line.split()[-1] for line in out.splitlines() if line.strip())
    return found


def header_declares(root, tmp, extra, name):
    """True if the headers declare `name` as something other than an int."""
    tu = tmp / "decl.c"
    tu.write_text("".join(f'#include "{h}"\n' for h in HEADERS) + f"static int {name};\n")
    inc = [f"-I{root / d}" for d in INCLUDE_DIRS]
    r = subprocess.run(BASE + extra + inc + [str(tu), "-o", str(tmp / "decl.o")],
                       capture_output=True, text=True)
    return r.returncode != 0


RACE_HOOK = "eds_race_point_hook"
RACE_PROBE = ('#include "eds_race_point.h"\n'
              "void eds_race_probe(void);\n"
              "void eds_race_probe(void) { EDS_RACE_POINT(1U); }\n")


def race_point_problems(root, tmp):
    """EDS#197: EDS_RACE_POINT is empty and hook-free in a production build."""
    tu = tmp / "race.c"
    tu.write_text(RACE_PROBE)
    obj = tmp / "race.o"
    inc = [f"-I{root / d}" for d in INCLUDE_DIRS]

    def symbols(extra):
        r = subprocess.run(BASE + extra + inc + [str(tu), "-o", str(obj)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None
        return subprocess.run(["nm", str(obj)], capture_output=True, text=True,
                              check=True).stdout.split()

    problems = []
    prod = symbols([])
    if prod is None:
        problems.append("eds_race_point.h: the production probe does not compile")
    elif RACE_HOOK in prod:
        problems.append(f"EDS_RACE_POINT: a production build references {RACE_HOOK}")
    test = symbols(["-DUNIT_TEST=1"])
    if test is None or RACE_HOOK not in test:
        problems.append(f"EDS_RACE_POINT: no {RACE_HOOK} reference even with UNIT_TEST "
                        "(the check above would be vacuous)")
    if symbols(["-DEDS_RACE_POINT(x)=(void)(x)"]) is not None:
        problems.append("EDS_RACE_POINT: a build that defines it compiles "
                        "(eds_race_point.h must stop it with #error)")
    return problems


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                        pathlib.Path(__file__).resolve().parent.parent).resolve()
    problems = []
    with tempfile.TemporaryDirectory() as t:
        tmp = pathlib.Path(t)
        prod = defined_symbols(root, tmp, [])
        hooks = defined_symbols(root, tmp, ["-DEDS_TEST_HOOKS=1"])
        for name in TEST_ONLY:
            if name not in hooks:
                problems.append(f"{name}: not defined even with EDS_TEST_HOOKS (stale list?)")
            if name in prod:
                problems.append(f"{name}: a production build exports it")
            if not header_declares(root, tmp, ["-DEDS_TEST_HOOKS=1"], name):
                problems.append(f"{name}: not declared with EDS_TEST_HOOKS (stale list?)")
            if header_declares(root, tmp, [], name):
                problems.append(f"{name}: a production build declares it")
        problems += race_point_problems(root, tmp)
    if problems:
        print("FAIL: test-only accessors reach a production build:")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print(f"PASS: none of the {len(TEST_ONLY)} test-only accessors is defined or declared "
          "without EDS_TEST_HOOKS; all are present with it")
    print("PASS: EDS_RACE_POINT is empty without UNIT_TEST, calls the hook with it, "
          "and cannot be defined outside eds_race_point.h")


if __name__ == "__main__":
    main()
