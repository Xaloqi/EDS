#!/usr/bin/env python3
"""Fail if the DoIP transport calls the UDS server off the executor path (EDS#197 J6).

Every UDS server, session or security call in transport/doip/*.c must sit in
the body of a doip_srv_*() function, and those functions may only be passed to
doip_on_server(), never called. doip_on_server() runs them on the diagnostic
executor (eds_doip_set_executor()), or, deprecated, between the lock
callbacks. The integrator callbacks that read server state (s_discovery.get_vin,
s_doip_pending_cb) count as server calls too.

One exemption: uds_server_build_negative_response() only formats three bytes
into the caller's buffer and touches no server state.

Comments and string literals are ignored.

Usage:
  verify_doip_exec_path.py [EDS_ROOT]   check transport/doip/
  verify_doip_exec_path.py --self-test  the canary: plant a server call, and a
                                        direct doip_srv_*() call, in a copy of
                                        doip_server.c; each must fail the check,
                                        and the unchanged copy must pass
"""
import pathlib
import re
import sys
import tempfile

SERVER_CALL = re.compile(
    r"\b(uds_server_\w+|uds_session_\w+|uds_security_\w+|s_discovery\.get_vin|s_doip_pending_cb)\s*\(")
EXEMPT = {"uds_server_build_negative_response"}
SRV_FN = "doip_srv_"
SRV_IDENT = re.compile(r"\b(doip_srv_\w+)\s*\(")
FUNC_HEAD = re.compile(r"\b(\w+)\s*\([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*\)\s*$")


def strip_comments_and_strings(src):
    """Replace comments and string/char literals with spaces, keeping newlines."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + c if j - i >= 2 else c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_spans(code):
    """(name, start, end) for each top-level brace block whose head looks like
    a function definition."""
    spans = []
    depth = 0
    start = None
    name = None
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                head = code[max(0, code.rfind(";", 0, i), code.rfind("}", 0, i)) + 1:i]
                m = FUNC_HEAD.search(head.strip())
                name = m.group(1) if m else None
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and name is not None:
                spans.append((name, start, i))
                name = None
    return spans


def line_of(code, pos):
    return code.count("\n", 0, pos) + 1


def check_file(path, text=None):
    """Violations in one C file, as 'path:line: message' strings."""
    src = path.read_text() if text is None else text
    code = strip_comments_and_strings(src)
    spans = function_spans(code)
    violations = []

    def enclosing(pos):
        for name, a, b in spans:
            if a <= pos <= b:
                return name
        return None

    for m in SERVER_CALL.finditer(code):
        if m.group(1) in EXEMPT:
            continue
        fn = enclosing(m.start())
        if fn is None or not fn.startswith(SRV_FN):
            violations.append(f"{path}:{line_of(code, m.start())}: {m.group(1)}() called in "
                              f"{fn or 'file scope'}(), outside a {SRV_FN}*() function")

    defined = {name for name, _, _ in spans if name.startswith(SRV_FN)}
    for m in SRV_IDENT.finditer(code):
        if m.group(1) in defined and enclosing(m.start()) is None:
            continue   # its own definition (or a prototype)
        violations.append(f"{path}:{line_of(code, m.start())}: {m.group(1)}() called directly; "
                          f"pass it to doip_on_server()")
    return violations


def check_root(root):
    files = sorted((root / "transport" / "doip").glob("*.c"))
    if not files:
        return [f"{root}/transport/doip: no C files found"], 0
    violations = []
    for f in files:
        violations += check_file(f)
    return violations, len(files)


def self_test(root):
    """The canary: two planted violations must each fail; the original passes."""
    src_path = root / "transport" / "doip" / "doip_server.c"
    original = src_path.read_text()
    anchor = "static uds_status_t doip_exec_dispatch("
    if anchor not in original:
        print(f"self-test: anchor '{anchor}' not found in {src_path}")
        return 1
    plants = {
        "a server call on the DoIP thread":
            "static void doip_planted(uds_server_ctx_t *c)\n{\n"
            "    (void)uds_server_tick_1ms(c);\n}\n\n",
        "a doip_srv_*() function called directly":
            "static void doip_planted(uds_server_ctx_t *c)\n{\n"
            "    doip_srv_end_session(c, NULL);\n}\n\n",
    }
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        copy = pathlib.Path(tmp) / "doip_server.c"
        base = check_file(copy, original)
        print(f"self-test: unchanged copy: {len(base)} violation(s)")
        ok = ok and not base
        for what, text in plants.items():
            found = check_file(copy, original.replace(anchor, text + anchor, 1))
            print(f"self-test: planted {what}: {len(found)} violation(s)")
            for v in found:
                print(f"  {v}")
            ok = ok and len(found) == 1
    print("self-test: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    root = pathlib.Path(__file__).resolve().parent.parent
    if len(argv) > 1 and argv[1] == "--self-test":
        return self_test(root)
    if len(argv) > 1:
        root = pathlib.Path(argv[1]).resolve()
    violations, nfiles = check_root(root)
    for v in violations:
        print(v)
    if violations:
        print(f"FAIL: {len(violations)} UDS call(s) off the executor path in transport/doip/ (EDS#197)")
        return 1
    print(f"OK: {nfiles} file(s) in transport/doip/ call the UDS server only through doip_on_server()")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
