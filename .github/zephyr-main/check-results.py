#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check-results.py <builds.tsv> <results.tsv> <expected rows> : the weekly job's verdict (run-013).

Pass = every row of builds.tsv, plus the deprecation sweep, has a PASS line in results.tsv, nothing
else is in there, and the number of rows equals <expected rows> (the workflow's EXPECT_ROWS, so
shortening builds.tsv without saying so fails). A missing, failed or skipped row is a failure;
"no failure was reported" is not a pass."""
import sys

builds, results, expect = sys.argv[1], sys.argv[2], int(sys.argv[3])
want = [l.split("\t")[0] for l in open(builds) if l.strip() and not l.startswith("#")] + ["deprecation-sweep"]
got = {}
try:
    for l in open(results):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 2:
            got.setdefault(p[0], []).append(p[1])
except FileNotFoundError:
    pass
problems = []
if len(want) != expect:
    problems.append(f"builds.tsv lists {len(want)} rows (with the sweep) but EXPECT_ROWS is {expect}: update both together")
for n in want:
    if n not in got:
        problems.append(f"{n}: no result (did not run, or the job stopped before it)")
    elif got[n] != ["PASS"]:
        problems.append(f"{n}: {' '.join(got[n])}")
for n in got:
    if n not in want:
        problems.append(f"{n}: result for a row builds.tsv does not list")
passed = sum(1 for n in want if got.get(n) == ["PASS"])
print(f"{passed}/{len(want)} expected rows PASS")
for p in problems:
    print("FAIL:", p)
sys.exit(1 if problems else 0)
