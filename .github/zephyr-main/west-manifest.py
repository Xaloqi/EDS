#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""west-manifest.py <EDS west.yml> <zephyr_rev> <out> <module> [<module> ...]

Writes the weekly job's manifest: EDS's own west.yml with ONLY Zephyr's revision replaced and the
import narrowed to the named Zephyr modules. Refuses anything else, so a change to west.yml's
shape cannot silently turn this into a different workspace."""
import sys
import yaml

src, rev, out, *modules = sys.argv[1:]
if not modules:
    sys.exit("west-manifest.py: no modules given")
m = yaml.safe_load(open(src))
projects = [p for p in m["manifest"]["projects"] if p["name"] == "zephyr"]
if len(projects) != 1:
    sys.exit("west-manifest.py: expected exactly one 'zephyr' project in " + src)
z = projects[0]
if z.get("import") != {"file": "west.yml"}:
    sys.exit("west-manifest.py: zephyr's import is not 'file: west.yml' any more: " + repr(z.get("import")))
z["revision"] = rev
z["import"] = {"file": "west.yml", "name-allowlist": modules}
yaml.safe_dump(m, open(out, "w"), sort_keys=False)
print(f"{out}: zephyr revision {rev}, modules {', '.join(modules)}")
