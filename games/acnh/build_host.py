#!/usr/bin/env python3
"""Build an ACNH diagnostic host from externally prepared, pinned player data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from games.acnh.prepare import check_exefs, prepared_profile, require
from games.acnh.tools.common import clone_file, outside_repository, verified_file
from games.acnh.tools.runtime_preflight import prepare as preflight
from games.acnh.tools.prepare_fps import prepare_fps
from games.acnh.tools.prepare_dlc import config_header as dlc_config_header


def write_changed(path, data):
    payload = data.encode() if isinstance(data, str) else data
    if not path.is_file() or path.read_bytes() != payload:
        path.write_bytes(payload)


def constructor_flag(image, module):
    """Decode the checked ADRP x19/LDR w8 guard in this pinned SDK's DT_INIT."""
    tags, layout = module["dynamic_tags"], module["layout"]
    init = int(tags["12"])
    a, word = struct.unpack_from("<II", image, init + 16)
    require(a & 0x9f00001f == 0x90000013 and word & 0xffc003ff == 0xb9400268,
            "Unknown DT_INIT guard instructions")
    immediate = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    immediate = (immediate ^ (1 << 20)) - (1 << 20)
    address = ((init + 16) & ~4095) + (immediate << 12) + ((word >> 10) & 0xfff) * 4
    require(layout["DATA_OFF"] <= address < layout["KEY_PAGE_OFF"], "DT_INIT guard outside data")
    return address


def clock_plt(image, module):
    symbols = module["symbols"]
    slot = next(row["where"] for row in module["relocations"] if row["kind"] == 1026 and
                symbols[row["symbol"]]["name"] == "_ZN2nn2os13GetSystemTickEv")
    matches = []
    for address in range(max(0, module["layout"]["TEXT_END"] - 0x10000), module["layout"]["TEXT_END"] - 12, 4):
        a, load, _, branch = struct.unpack_from("<IIII", image, address)
        if a & 0x9f00001f != 0x90000010 or load & 0xffc003ff != 0xf9400211 or branch != 0xd61f0220:
            continue
        immediate = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        immediate = (immediate ^ (1 << 20)) - (1 << 20)
        if (address & ~4095) + (immediate << 12) + ((load >> 10) & 0xfff) * 8 == slot:
            matches.append(address)
    require(len(matches) == 1, "Expected one original GetSystemTick PLT")
    return matches[0], slot


def emit_tables(target, output, data):
    rows = ["/* Generated from verified local player data. Never commit. */"]
    initializers = []
    for index, name in enumerate(data["summary"]["module_order"]):
        module = data["modules"][name]
        source = target / "rewritten" / name / f"{name}.patched.bin"
        verified_file(source, module["patched_sha256"])
        image = source.read_bytes()
        flag = constructor_flag(image, module)
        shutil.copy2(source, output.parent / "rewritten" / source.name)
        rows.append(f"static const Reloc reloc_{index}[] = {{")
        rows.extend(f'{{{r["where"]}ULL,{r["kind"]},{r["symbol"]},{r["addend"]}LL}},' for r in module["relocations"])
        rows.append("};")
        rows.append(f"static const Symbol symbol_{index}[] = {{")
        rows.extend("{" + f'{json.dumps(s["name"])},{s["value"]}ULL,{s["size"]}ULL,{int(s["defined"])},{s["binding"]},{s["type"]}' + "}," for s in module["symbols"])
        rows.append("};")
        rows.append(f"static const Gate gate_{index}[] = {{")
        rows.extend(f'{{{g["return"]}ULL,{g["original"]}ULL,{int(g["kind"] != "svc")},{g["arg"]}}},' for g in module["gates"])
        if not module["gates"]:
            rows.append("{0,0,0,0},")
        rows.append("};")
        layout, tags = module["layout"], module["dynamic_tags"]
        fields = {"name": json.dumps(name), "digest": json.dumps(module["patched_sha256"]),
                  "file_size": f'{len(image)}ULL', "symbols": f"symbol_{index}",
                  "nsym": len(module["symbols"]), "relocs": f"reloc_{index}",
                  "nreloc": len(module["relocations"]), "gates": f"gate_{index}",
                  "ngate": len(module["gates"]), "ctor_flag": f"{flag}ULL"}
        fields.update({key: f"{layout[value]}ULL" for key, value in
                       (("text_end", "TEXT_END"), ("ro_off", "RO_OFF"), ("data_off", "DATA_OFF"),
                        ("patch_off", "PATCH_OFF"), ("key_off", "KEY_PAGE_OFF"))})
        fields.update({key: f"{tags.get(value, 0)}ULL" for key, value in
                       (("init", "12"), ("init_array", "25"), ("init_size", "27"))})
        initializers.append("{" + ",".join(f".{key}={value}" for key, value in fields.items()) + "},")
        if name == "main":
            plt, slot = clock_plt(image, module)
    rows += ["static Module modules[] = {", *initializers, "};",
             f"#define PROBE_PLT {plt}ULL", f"#define PROBE_SLOT {slot}ULL"]
    write_changed(output / "runtime_tables.h", "\n".join(rows) + "\n")


def prepare_host(prepared, work, fonts, fps_mod=None, dlc=None, forget=()):
    """fps_mod/dlc default to the inputs the work folder remembers; names in forget ("fps", "dlc") drop them."""
    prepared = outside_repository(prepared, ROOT)
    work = outside_repository(work, ROOT)
    fonts = outside_repository(fonts, ROOT)
    target = prepared / "update"
    expected = prepared_profile(prepared)
    if work.exists():
        info_path = work / "host-info.json"
        require(info_path.is_file(), "Existing host work has no identity record")
        info = json.loads(info_path.read_text())
        require(info.get("prepared") == str(prepared) and
                info.get("display_version") == expected["display_version"],
                "Existing host work belongs to another prepared game/version")
        if fps_mod is None and "fps" not in forget and info.get("fps_mod"):
            fps_mod = Path(info["fps_mod"])
        if dlc is None and "dlc" not in forget and info.get("dlc"):
            dlc = Path(info["dlc"])
    check_exefs(json.loads((target / "extraction-manifest.json").read_text()), expected)
    index = json.loads((target / "romfs/index.json").read_text())
    verified_file(target / "romfs/romfs.bin", expected["romfs_sha256"])
    require(index["romfs_sha256"] == expected["romfs_sha256"], "RomFS index identity mismatch")
    # The runtime starts build/shader-service/totk_xlate (totk_shader_rt.m): the pinned prebuilt translator.
    translator = HERE / "prebuilt/macos-arm64/totk_xlate"
    verified_file(translator, (HERE / "prebuilt/macos-arm64/totk_xlate.sha256").read_text().split()[0])
    work.mkdir(parents=True, exist_ok=True)
    for folder in ("build", "rewritten", "logs"):
        (work / folder).mkdir(exist_ok=True)
    write_changed(work / "build/version-config.h",
        f'#define ACNH_VERSION_RAW {expected["cnmt_version_raw"]}\n')
    clone_file(translator, work / "build/shader-service/totk_xlate")
    with tempfile.TemporaryDirectory(dir=work) as temporary:
        preflight(target / "exefs", target / "extraction-manifest.json", target / "rewritten", Path(temporary) / "runtime")
        data = json.loads((Path(temporary) / "runtime/runtime-modules.json").read_text())
        emit_tables(target, work / "build", data)
    strings, entries = bytearray(), bytearray()
    for row in index["files"]:
        name = row["path"].encode() + b"\0"
        entries.extend(struct.pack("<QQII", row["offset"], row["size"], len(strings), len(name)))
        strings.extend(name)
    binary_index = struct.pack("<8sQQ", b"TOTKRFS1", len(index["files"]), 24 + len(entries)) + entries + strings
    write_changed(target / "romfs/runtime-index.bin", binary_index)
    write_changed(work / "build/romfs_config.h",
        f'#define ROMFS_IMAGE_SIZE {expected["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{expected["romfs_sha256"]}"\n'
        f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(binary_index).hexdigest()}"\n')
    font_manifest = json.loads((fonts / "manifest.json").read_text())
    require(font_manifest["status"] == "pass" and [f["type"] for f in font_manifest["fonts"]] == list(range(6)), "Expected six verified fonts")
    rows = []
    for font in font_manifest["fonts"]:
        path = fonts / f'{font["type"]}-{font["name"]}.bfttf'
        verified_file(path, font["sha256"])
        rows.append("{" + f'{json.dumps(str(path))},{font["bfttf_bytes"]}ULL,{font["font_bytes"]}ULL,"{font["sha256"]}"' + "}")
    write_changed(work / "build/shared_fonts_config.h", "#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS " + ",".join(rows) + "\n")
    info = {"title_id": expected["title_id"], "display_version": expected["display_version"],
            "prepared": str(prepared), "work": str(work), "shared_fonts": str(fonts),
            "scope": "Diagnostic host build; execution is a separate validation step"}
    if fps_mod is not None:
        prepare_fps(prepared, work, fps_mod)
        info["fps_mod"] = str(fps_mod.expanduser().resolve())
        info["fps_manifest_sha256"] = hashlib.sha256((work / "build/fps60/manifest.json").read_bytes()).hexdigest()
    else:
        write_changed(work / "build/fps_config.h", "#define ACNH_FPS_ENABLED 0\n")
    if dlc is not None:
        require(expected["cnmt_version_raw"] == 2097152, "DLC support is currently scoped to 3.0.1")
        dlc = outside_repository(dlc, ROOT)
        header, digest = dlc_config_header(dlc)
        write_changed(work / "build/dlc_config.h", header)
        info["dlc"] = str(dlc)
        info["dlc_manifest_sha256"] = digest
    else:
        write_changed(work / "build/dlc_config.h", '#define ACNH_DLC_COUNT 0\n#define ACNH_DLC_MARKER "MNG_ACNH_DLC:none"\n')
    (work / "host-info.json").write_text(json.dumps(info, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("prepared", "work", "shared-fonts"):
        parser.add_argument(f"--{flag}", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--fps-mod", type=Path, help="Optional pinned tmouh/acnh-60fps source folder; enables experimental 3.0.1 60 FPS")
    parser.add_argument("--dlc", type=Path, help="Optional directory from tools/prepare_dlc.py; remembered for rebuilds")
    parser.add_argument("--no-fps-mod", action="store_true", help="Forget the remembered --fps-mod input (original 30 FPS only)")
    parser.add_argument("--no-dlc", action="store_true", help="Forget the remembered --dlc input (zero DLCs)")
    args = parser.parse_args()
    if args.no_fps_mod and args.fps_mod or args.no_dlc and args.dlc:
        parser.error("an input and its --no-* option are mutually exclusive")
    forget = {name for name, drop in (("fps", args.no_fps_mod), ("dlc", args.no_dlc)) if drop}
    try:
        require(args.jobs > 0, "jobs must be positive")
        work = args.work.expanduser().resolve()
        prepare_host(args.prepared, work, args.shared_fonts, args.fps_mod, args.dlc, forget)
        with (work / "logs/build.log").open("w") as log:
            result = subprocess.run(["make", f"-j{args.jobs}", f"WORK={work}"], cwd=HERE, stdout=log, stderr=subprocess.STDOUT)
        require(result.returncode == 0, f"Host compilation failed; see {work / 'logs/build.log'}")
        print(work / "build/acnh-native")
    except (OSError, ValueError, KeyError) as error:
        print(f"ACNH host build failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
