#!/usr/bin/env python3
"""Re-translate all 58 known FP64-failing vertex shaders and compile their MSL."""
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/fp64-fallback-check"
OUT.mkdir(parents=True, exist_ok=True)
failures = [line.split("\t")[0] for line in (ROOT / "docs/shader-preheat-failures.tsv").read_text().splitlines()[1:]]
shader_map = {a: c for a, b, c in (line.split("\t") for line in (ROOT / "build/shader-map.tsv").read_text().splitlines()[1:])}
command = [
    "python3", "-I", str(ROOT / "scripts/shader_service.py"),
    "--dotnet", str(ROOT / "tooling/dotnet/dotnet"),
    "--xlate", str(ROOT / "build/totk_xlate/totk_xlate.dll"),
    "--spirv-cross", str(ROOT / "build/spirv-cross/spirv-cross"),
    "--out", str(OUT), "--chain", "totk-native-v6-unverified",
]
results = []
with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, cwd=ROOT) as service:
    for key in failures:
        asset = shader_map[key]
        src = ROOT / "extracted/v0/shaders-v2"
        control = (src / (asset + ".control")).read_bytes()
        code = (src / (asset + ".code")).read_bytes()
        c1_offset, length = struct.unpack_from("<II", control, 0x700)
        variant = key + "-fp64check"
        code_path = OUT / (variant + ".code")
        code_path.write_bytes(code[0x30:length])
        request = {"variant": variant, "stage": "vs", "code": str(code_path),
                   "c1": code[c1_offset:length].hex(), "tex": {}, "img": {},
                   "attr": {}, "fout": {}, "depth_minus_w": True}
        service.stdin.write(json.dumps(request) + "\n")
        service.stdin.flush()
        reply = json.loads(service.stdout.readline())
        row = {"shader": key, **reply}
        if reply.get("ok"):
            row["softfloat64"] = json.loads((OUT / (variant + "_b.json")).read_text()).get("softfloat64")
            row["msl"] = str(OUT / (variant + "_b.metal"))
        results.append(row)
    service.stdin.close()
    service.wait(timeout=10)

translated = [r for r in results if r.get("ok")]
compiled = None
if translated:
    metal = subprocess.run([str(ROOT / "build/metal-shader-probe")] + [r["msl"] for r in translated],
                           cwd=ROOT, text=True, capture_output=True)
    compiled = {"returncode": metal.returncode, "stdout": metal.stdout[-1000:], "stderr": metal.stderr[-3000:]}
status = "pass" if len(translated) == len(failures) and compiled and compiled["returncode"] == 0 else "fail"
report = {"status": status, "known_failure_count": len(failures), "translated": len(translated),
          "fallback_used": sum(bool(r.get("softfloat64")) for r in translated),
          "metal_compilation": compiled, "failures": [r for r in results if not r.get("ok")]}
(ROOT / "analysis/fp64-fallback-validation.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: v for k, v in report.items() if k != "metal_compilation"}, ensure_ascii=False))
sys.exit(status != "pass")
