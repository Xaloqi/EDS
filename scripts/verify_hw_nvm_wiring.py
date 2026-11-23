#!/usr/bin/env python3
"""Fail if a hardware build of an example has no NVM partition behind its store.

An example that calls nvm_store_init(NULL) on hardware links the real NVS
backend, which returns UDS_STATUS_ERR_NULL_PTR for a NULL config: the DTC mirror
and the SecurityAccess counters then live in RAM only (EDS#643). That compiles,
links and boots, so only the build's objects show it.

Checks on a west build directory:
  * exactly the expected backend is linked: append -> nvm_store_append.c.obj,
    nvs -> nvm_store.c.obj, never both (--backend)
  * some app object references flash_get_page_info_by_offs (the syscall's
    z_impl_ name when it is inlined), which only the code
    that resolves the diag_nvs partition (eds_nvm_diag_nvs.c, basic_ecu's main.c)
    calls; a NULL-config build never does

Usage: verify_hw_nvm_wiring.py <build-dir> --backend append|nvs
"""
import argparse
import pathlib
import re
import subprocess
import sys


def cache_var(build, name):
    for line in (build / "CMakeCache.txt").read_text(errors="replace").splitlines():
        m = re.match(rf"{name}:[A-Z]+=(.*)$", line)
        if m:
            return m.group(1)
    sys.exit(f"FAIL: {name} not in {build}/CMakeCache.txt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir")
    ap.add_argument("--backend", required=True, choices=("append", "nvs"))
    args = ap.parse_args()

    build = pathlib.Path(args.build_dir)
    objs = sorted((build / "CMakeFiles" / "app.dir").rglob("*.obj"))
    if not objs:
        sys.exit(f"FAIL: no app objects under {build}/CMakeFiles/app.dir")
    names = {o.name for o in objs}
    want, other = (("nvm_store_append.c.obj", "nvm_store.c.obj") if args.backend == "append"
                   else ("nvm_store.c.obj", "nvm_store_append.c.obj"))

    problems = []
    if want not in names:
        problems.append(f"{want} is not linked (expected the {args.backend} backend)")
    if other in names:
        problems.append(f"{other} is linked next to the {args.backend} backend")

    nm = cache_var(build, "CMAKE_NM")
    resolves = False
    for o in objs:
        out = subprocess.run([nm, str(o)], capture_output=True, text=True, check=True).stdout
        if re.search(r"^\s+U (?:z_impl_)?flash_get_page_info_by_offs$", out, re.M):
            resolves = True
            break
    if not resolves:
        problems.append("no object resolves the diag_nvs partition "
                        "(flash_get_page_info_by_offs is never called): "
                        "nvm_store_init(NULL) on hardware")

    if problems:
        for p in problems:
            print(f"FAIL: {build.name}: {p}")
        sys.exit(1)
    print(f"PASS: {build.name}: {want}, diag_nvs resolved")


if __name__ == "__main__":
    main()
