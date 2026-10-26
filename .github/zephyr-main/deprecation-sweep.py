#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""deprecation-sweep.py <workspace> <out dir> : the weekly job's deprecation sweep (EDS#524).

Since EDS#515 the Zephyr, module and HAL include directories are SYSTEM includes for the example
targets, so GCC silences `#warning` / `__WARN()` raised inside them: a deprecated header (for
example zephyr/fs/nvs.h) no longer fails a warnings-as-errors build. This recompiles every EDS
translation unit of the builds listed in <out>/sweep-dirs.txt with -Wsystem-headers -Wno-undef
-fsyntax-only and fails on any "deprecated" message.

Positive result (run-013): the sweep also fails if it examined nothing. It writes one row to
<out>/results.tsv:   deprecation-sweep  PASS|FAIL  <n> TUs in <m> builds, <k> messages"""
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ws, out = sys.argv[1:3]
eds = os.path.realpath(os.path.join(ws, "EDS")) + os.sep
names = [n.strip() for n in open(os.path.join(out, "sweep-dirs.txt")) if n.strip()]
jobs = []  # (build name, directory, command)
for n in names:
    cc = json.load(open(os.path.join(ws, "build", n, "compile_commands.json")))
    for e in cc:
        f = os.path.realpath(os.path.join(e["directory"], e["file"]))
        if not f.startswith(eds) or not f.endswith(".c"):
            continue
        cmd = re.sub(r" -o \S+", " -o /dev/null", e["command"])
        cmd = re.sub(r" -MD -MT \S+ -MF \S+", "", cmd)
        cmd = cmd.replace(" -fdiagnostics-color=always", "")
        cmd += " -Wsystem-headers -Wno-undef -Wno-error -fno-diagnostics-color -fsyntax-only"
        jobs.append((n, e["directory"], cmd))


def run(j):
    r = subprocess.run(j[2], shell=True, cwd=j[1], capture_output=True, text=True)
    hits = [l for l in r.stderr.splitlines()
            if re.search(r"deprecated", l, re.I) and re.search(r"(warning|error)", l)]
    return j[0], r.returncode, hits, r.stderr


with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
    res = list(ex.map(run, jobs))

msgs = {}
bad_rc = 0
for n, rc, hits, err in res:
    if rc != 0:
        bad_rc += 1
        print(f"[{n}] syntax-only compile exited {rc}:\n{err[-1500:]}")
    for h in hits:
        h = re.sub(r"^.*?/(EDS|zephyr|modules|bootloader)/", r"\1/", h)[:240]
        msgs.setdefault(h, set()).add(n)
for h, ns in sorted(msgs.items()):
    print(f"DEPRECATED ({', '.join(sorted(ns))}): {h}")
detail = f"{len(jobs)} TUs in {len(names)} builds, {len(msgs)} distinct messages"
ok = bool(jobs) and not msgs and not bad_rc
if not jobs:
    detail += " (nothing examined)"
if bad_rc:
    detail += f", {bad_rc} compiles failed"
print("deprecation sweep:", "PASS" if ok else "FAIL", "-", detail)
with open(os.path.join(out, "results.tsv"), "a") as f:
    f.write(f"deprecation-sweep\t{'PASS' if ok else 'FAIL'}\t{detail}\n")
if not ok:
    ff = os.path.join(out, "first-failure.txt")
    if not os.path.exists(ff) or os.path.getsize(ff) == 0:
        first = next(iter(sorted(msgs)), "see the sweep log")
        open(ff, "w").write(f"deprecation-sweep: {detail}\n{first}\n")
        open(os.path.join(out, "tail.txt"), "w").write("\n".join(sorted(msgs)[:30]) + "\n")
sys.exit(0)
