#!/usr/bin/env python3
"""Fails unless a PE file loads on Windows 98 SE: every import is in the
allow-list (functions Windows 98 SE's own kernel32/user32/winmm/dsound and
its original msvcrt.dll export), nothing is imported by ordinal, there is
no TLS directory (Windows 9x does not run TLS callbacks) and the subsystem
and OS versions are 4.x.

    python3 -I check_imports.py [--objdump OBJDUMP] --allow LIST FILE.exe
"""

import argparse
import re
import subprocess
import sys


def read_allow(path):
    allow = set()
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) == 2:
                allow.add((line[0].lower(), line[1]))
    return allow


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="i686-w64-mingw32-objdump")
    ap.add_argument("--allow", required=True)
    ap.add_argument("exe")
    a = ap.parse_args()

    allow = read_allow(a.allow)
    dump = subprocess.run([a.objdump, "-p", a.exe], check=True, capture_output=True,
                          text=True).stdout
    errors = []

    fields = dict(re.findall(r"^(\w+)\s+([0-9a-fA-F]+)", dump, re.M))
    if fields.get("MajorSubsystemVersion") != "4":
        errors.append("subsystem version %s.%s (Windows 98 needs 4.x)" % (
            fields.get("MajorSubsystemVersion"), fields.get("MinorSubsystemVersion")))
    if int(fields.get("MajorOSystemVersion", "99")) > 4:
        errors.append("OS version %s.%s" % (fields.get("MajorOSystemVersion"),
                                            fields.get("MinorOSystemVersion")))
    tls = re.search(r"^Entry 9 ([0-9a-fA-F]+) ([0-9a-fA-F]+) Thread Storage", dump, re.M)
    if not tls or int(tls.group(1), 16) or int(tls.group(2), 16):
        errors.append("has a TLS directory (TLS callbacks do not run on Windows 9x)")

    imports = []
    dll = None
    for line in dump.splitlines():
        m = re.match(r"\s*DLL Name: (\S+)", line)
        if m:
            dll = m.group(1).lower()
            continue
        if not dll:
            continue
        if not line.strip():
            dll = None
            continue
        m = re.match(r"\s*[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]+)\s+(\S+)\s*$", line)
        if m:
            if m.group(1) != "<none>":
                errors.append("%s imported by ordinal %s" % (dll, m.group(1)))
            imports.append((dll, m.group(3)))
            continue
        m = re.match(r"\s*[0-9a-fA-F]+\s+(\d+)\s*$", line)
        if m:
            errors.append("%s imported by ordinal %s" % (dll, m.group(1)))

    if not imports:
        errors.append("no imports found (objdump output not understood)")
    for dll_name, fn in imports:
        if (dll_name, fn) not in allow:
            errors.append("import %s!%s is not in %s" % (dll_name, fn, a.allow))

    dlls = sorted({d for d, _ in imports})
    print("%s: %d imports from %s" % (a.exe, len(imports), ", ".join(dlls)))
    if errors:
        for e in errors:
            print("error: " + e, file=sys.stderr)
        return 1
    print("ok: Windows 98 SE compatible (imports, no TLS, subsystem 4.0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
