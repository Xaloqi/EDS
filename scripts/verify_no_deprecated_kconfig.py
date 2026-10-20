#!/usr/bin/env python3
"""Fail if any EDS .conf assigns a Kconfig symbol the pinned Zephyr marks deprecated.

Zephyr marks an option deprecated before it removes it: `select DEPRECATED`
(bool options), "[DEPRECATED]" in the prompt, or a definition in a
Kconfig.deprecated file (int options such as POSIX_MAX_FDS: deprecated in 3.7,
gone by 4.4, so basic_ecu_doip stopped configuring - #498).
Usage: verify_no_deprecated_kconfig.py <ZEPHYR_BASE> [EDS_ROOT]
"""
import pathlib, re, subprocess, sys

def deprecated_symbols(zephyr):
    syms = set()
    for kf in zephyr.rglob("Kconfig*"):
        if not kf.is_file():
            continue
        in_dep_file = kf.name == "Kconfig.deprecated"
        cur = None
        for line in kf.read_text(errors="replace").splitlines():
            m = re.match(r"\s*(?:menu)?config\s+(\w+)\s*$", line)
            if m:
                cur = m.group(1)
                if in_dep_file:
                    syms.add(cur)
            elif re.match(r"\s*(choice|endchoice|menu|endmenu|if|endif|source|comment)\b", line):
                cur = None
            elif cur and (re.match(r"\s*select\s+DEPRECATED\b", line)
                          or re.match(r'\s*(bool|int|hex|string)\s+".*\[DEPRECATED\]', line)):
                syms.add(cur)
    return syms

def main():
    zephyr = pathlib.Path(sys.argv[1])
    root = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else ".")
    dep = deprecated_symbols(zephyr)
    if not dep:
        print("FAIL: found no deprecated symbols in", zephyr, "- wrong ZEPHYR_BASE?")
        return 1
    hits, n = [], 0
    tracked = subprocess.run(["git", "-C", str(root), "ls-files", "*.conf"],
                             capture_output=True, text=True, check=True).stdout.split()
    for rel in tracked:
        conf = root / rel
        n += 1
        for i, line in enumerate(conf.read_text(errors="replace").splitlines(), 1):
            m = re.match(r"\s*CONFIG_(\w+)=", line)
            if m and m.group(1) in dep:
                hits.append(f"{conf}:{i}: CONFIG_{m.group(1)} is deprecated in this Zephyr")
    if n == 0:
        print("FAIL: no tracked .conf files found under", root)
        return 1
    print(f"checked {n} .conf files against {len(dep)} deprecated Kconfig symbols")
    for h in hits:
        print("FAIL:", h)
    return 1 if hits else 0

if __name__ == "__main__":
    sys.exit(main())
