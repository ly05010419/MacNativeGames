"""Prepare an optional, pinned 3.0.1 speed correction from the player's local inputs.

The external mod remains CC BY-NC 4.0 local data, never vendored source. We do
not run its tools or modify the original NSOs, rewritten modules or RomFS.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct

import lz4.block

from games.acnh.prepare import prepared_profile, require
from games.acnh.tools.common import verified_file

SOURCE_URL = "https://github.com/tmouh/acnh-60fps"
SOURCE_COMMIT = "e5e03e64739334c9fbd36f19658b98e805f8ba8d"
MOD = "ryujinx/mods/contents/01006F8002326000/ACNH 60 FPS + SpeedFix/"
PINS = {
    MOD + "exefs/speedfix_main_301.pchtxt": "37947a9fd64ee93882173bbe104dccef024d18f23b74315f4801d7330c6f3cf3",
    "pack/StaticParam.pack.ips": "05a169b3ffeb00818a55c329dbee7999c9bc4f990447111882bd5d5e5dcd953d",
    MOD + "romfs/Bcsv/NpcInterest.bcsv": "eed347d2511367bfdb7803e12d1314fb36a18c873e95306ea05ae473ecf6c7e9",
    MOD + "romfs/Bcsv/ItemNpcFtrActionParam.bcsv": "fa285b04f48774dadf6bcf1d8c608f8751c1825d1635870ec6e7f274e8e6688d",
    MOD + "romfs/Bcsv/ItemNpcWherearenFtrActionParam.bcsv": "c61bf2046ffcb46d3dc6c005c44abbf7584ed9cd43e40ab7c73e720c127188a3",
    "LICENSE": "41003d4a74749c0220e33dd415042164b5a1093ed401f36277234f772d22d3d0",
}
PACK_ORIGINAL = "4a3d6530d6430b6b967467f16784f91700a3cd8baad5f5b28fe3c6cfe4983298"
PACK_RESULT = "7ea1622e9367b063ffe98d2acb03f1acd375b752d2a1024e8825607972e7c572"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_changed(path, data):
    data = data.encode() if isinstance(data, str) else data
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def apply_ips(original, patch):
    """Bounded standard IPS replacements/RLE; extending or truncating is refused."""
    require(patch[:5] == b"PATCH", "Expected IPS header")
    out, pos = bytearray(original), 5
    while True:
        require(pos + 3 <= len(patch), "Truncated IPS record")
        address, pos = patch[pos:pos + 3], pos + 3
        if address == b"EOF":
            require(pos == len(patch), "IPS truncation or trailing data is unsupported")
            return bytes(out)
        offset = int.from_bytes(address, "big")
        require(pos + 2 <= len(patch), "Truncated IPS size")
        size, pos = int.from_bytes(patch[pos:pos + 2], "big"), pos + 2
        if size:
            require(pos + size <= len(patch), "Truncated IPS payload")
            value, pos = patch[pos:pos + size], pos + size
        else:
            require(pos + 3 <= len(patch), "Truncated IPS RLE")
            size = int.from_bytes(patch[pos:pos + 2], "big")
            value, pos = patch[pos + 2:pos + 3] * size, pos + 3
            require(size > 0, "Empty IPS RLE")
        require(offset + size <= len(out), "IPS record outside original file")
        out[offset:offset + size] = value


def parse_pchtxt(text, build_id):
    """Read virtual NSO offsets; offset_shift only describes the IPS file format."""
    identity, enabled, stopped, words = False, False, False, {}
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        require(not stopped, "Data after pchtxt stop")
        if line.startswith("@nsobid-"):
            require(not identity and line[8:].lower() == build_id[:len(line[8:])].lower()
                    and len(line[8:]) >= 32, "FPS patch Build ID mismatch")
            identity = True
        elif line == "@enabled":
            require(identity, "FPS patch lacks identity")
            enabled = True
        elif line == "@disabled":
            enabled = False
        elif line == "@stop":
            stopped = True
        elif line in ("@flag print_values", "@flag offset_shift 0x100"):
            continue
        else:
            match = re.fullmatch(r"([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})", line)
            require(match is not None and identity, "Unsupported FPS patch record")
            offset = int(match[1], 16)
            require(offset % 4 == 0 and offset not in words, "Unaligned or duplicate FPS patch")
            if enabled:
                words[offset] = bytes.fromhex(match[2])
    require(identity and stopped and words, "Incomplete FPS patch")
    return sorted(words.items())


# Words the instruction rewriter gates (third_party/d2r/patch_nso.py); a replacement
# must not introduce one after rewriting, or it would run ungated.
SYSTEM_MASK, SVC_MASK = 0xFFD00000, 0xFFE0001F
BRANCHES = {"b", "bl", "cbz", "cbnz", "tbz", "tbnz"}


def check_replacement(offset, word, original, rewritten, text_end):
    """Reject a text-segment replacement the rewriter would have gated or that branches into gated code."""
    if offset >= text_end:
        return
    require(word & SVC_MASK != 0xD4000001 and word & SYSTEM_MASK != 0xD5100000,
            f"FPS patch at {offset:#x} introduces a gated svc/system-register instruction")
    import capstone
    ins = next(capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM).disasm(word.to_bytes(4, "little"), offset), None)
    if ins is None:
        return
    require(re.search(r"\b[xw]18\b", ins.op_str) is None, f"FPS patch at {offset:#x} uses the reserved x18 register")
    if ins.mnemonic in BRANCHES or ins.mnemonic.startswith("b."):
        target = int(ins.op_str.rsplit("#", 1)[1], 0)
        require(0 <= target < text_end and target % 4 == 0
                and rewritten[target:target + 4] == original[target:target + 4],
                f"FPS patch at {offset:#x} branches outside the text segment or into rewritten code")


def text_end(path):
    raw = path.read_bytes()
    require(raw[:4] == b"NSO0", "Expected original NSO")
    _, address, size, _ = struct.unpack_from("<IIII", raw, 0x10)
    return address + size


def original_image(path):
    raw = path.read_bytes()
    require(raw[:4] == b"NSO0", "Expected original NSO")
    flags = struct.unpack_from("<I", raw, 12)[0]
    chunks, end = [], 0
    for i in range(3):
        fileoff, address, size, _ = struct.unpack_from("<IIII", raw, 0x10 + 16 * i)
        stored = struct.unpack_from("<I", raw, 0x60 + 4 * i)[0] if flags & (1 << i) else size
        payload = raw[fileoff:fileoff + stored]
        value = lz4.block.decompress(payload, uncompressed_size=size) if flags & (1 << i) else payload
        require(len(value) == size and hashlib.sha256(value).digest() == raw[0xa0 + 32 * i:0xc0 + 32 * i],
                "Original NSO segment hash mismatch")
        chunks.append((address, value))
        end = max(end, address + size)
    image = bytearray(end)
    for address, value in chunks:
        image[address:address + len(value)] = value
    return image


def verify_assets(directory):
    """Verify the complete generated bundle before packaging or reusing it."""
    manifest = json.loads((directory / "manifest.json").read_text())
    require(manifest.get("schema") == 1 and manifest.get("version") == "3.0.1"
            and manifest.get("source_commit") == SOURCE_COMMIT, "Unknown FPS asset profile")
    required = {"patches.tsv", "romfs/manifest.tsv", "romfs/Pack/StaticParam.pack", "LICENSE", "NOTICE"}
    required.update("romfs/Bcsv/" + Path(k).name for k in PINS if k.endswith(".bcsv"))
    require(set(manifest["files"]) == required, "Incomplete FPS asset bundle")
    for name, row in manifest["files"].items():
        path = PurePosixPath(name)
        require(not path.is_absolute() and ".." not in path.parts and name.isascii(), "Unsafe FPS asset path")
        file = directory / name
        require(file.is_file() and file.resolve().is_relative_to(directory.resolve()), "Missing or escaped FPS asset")
        require(file.stat().st_size == row["size"], "FPS asset size mismatch")
        verified_file(file, row["sha256"])
    return manifest


def prepare_fps(prepared, work, source):
    profile = prepared_profile(prepared)
    require(profile["cnmt_version_raw"] == 2097152, "SpeedFix is supported only for ACNH 3.0.1")
    source = source.expanduser().resolve()
    for name, sha in PINS.items():
        verified_file(source / name, sha)
    original_path = prepared / "update/exefs/main"
    verified_file(original_path, profile["exefs_sha256"]["main"])
    rewritten_path = work / "rewritten/main.patched.bin"
    verified_file(rewritten_path, profile["rewritten_sha256"]["main"])
    image, rewritten = original_image(original_path), rewritten_path.read_bytes()
    main = next(m for m in profile["modules"] if m["name"] == "main")
    words = parse_pchtxt((source / (MOD + "exefs/speedfix_main_301.pchtxt")).read_text(), main["build_id"])
    require(len(words) == 6417, "Unexpected SpeedFix patch count")
    lines, code_end = ["MNG_ACNH_FPS1"], text_end(original_path)
    for offset, value in words:
        require(offset + 4 <= len(image), "FPS patch outside original module")
        before = bytes(image[offset:offset + 4])
        require(rewritten[offset:offset + 4] == before, "FPS patch overlaps an instruction-rewriter gate")
        check_replacement(offset, int.from_bytes(value, "little"), image, rewritten, code_end)
        lines.append(f"{offset:x} {int.from_bytes(before, 'little'):08x} {int.from_bytes(value, 'little'):08x}")

    dest = work / "build/fps60"
    write_changed(dest / "patches.tsv", "\n".join(lines) + "\n")
    romfs = prepared / "update/romfs"
    index = {row["path"]: row for row in json.loads((romfs / "index.json").read_text())["files"]}
    overlays = []
    with (romfs / "romfs.bin").open("rb") as stream:
        for name in ("Pack/StaticParam.pack", "Bcsv/NpcInterest.bcsv", "Bcsv/ItemNpcFtrActionParam.bcsv",
                     "Bcsv/ItemNpcWherearenFtrActionParam.bcsv"):
            row = index[name]
            stream.seek(row["offset"])
            before = stream.read(row["size"])
            require(len(before) == row["size"], "Short original RomFS file")
            if name == "Pack/StaticParam.pack":
                require(digest(before) == PACK_ORIGINAL, "Unknown original StaticParam.pack")
                value = apply_ips(before, (source / "pack/StaticParam.pack.ips").read_bytes())
                require(digest(value) == PACK_RESULT, "SpeedFix parameter result hash mismatch")
            else:
                value = (source / (MOD + "romfs/" + name)).read_bytes()
            require(len(value) == len(before), "SpeedFix changed an overlay's structure/size")
            write_changed(dest / "romfs" / name, value)
            overlays.append(f"{name}\t{len(value)}\t{digest(value)}\t{digest(before)}")
    write_changed(dest / "romfs/manifest.tsv", "\n".join(sorted(overlays)) + "\n")
    write_changed(dest / "LICENSE", (source / "LICENSE").read_bytes())
    write_changed(dest / "NOTICE", f"ACNH 60 FPS + SpeedFix by tmouh\n{SOURCE_URL}\n"
                  f"Source commit: {SOURCE_COMMIT}\nLicense: CC BY-NC 4.0 (see LICENSE).\n"
                  "Local adaptation: checked main-module word replacements and RomFS overlays for the native Mac host.\n"
                  "The SDK display patch is replaced by the host's 60 Hz presentation policy.\n"
                  "Generated parameter files derive from the owner's verified game data. Do not commit them.\n")
    manifest = {"schema": 1, "version": "3.0.1", "title_id": profile["title_id"], "build_id": main["build_id"],
                "source_url": SOURCE_URL, "source_commit": SOURCE_COMMIT, "source_directory": str(source),
                "adapter": {"path": "games/acnh/tools/prepare_fps.py", "sha256": digest(Path(__file__).read_bytes())},
                "target_fps": 60, "sdk_enabler": "native host present interval at 60 Hz",
                "license": "CC BY-NC 4.0", "support_level": "experimental", "patch_count": len(words),
                "input_pins": PINS, "original_main_sha256": profile["exefs_sha256"]["main"],
                "rewritten_main_sha256": profile["rewritten_sha256"]["main"],
                "original_romfs_sha256": profile["romfs_sha256"], "files": {}}
    for file in sorted(dest.rglob("*")):
        if file.is_file() and file.name != "manifest.json":
            manifest["files"][file.relative_to(dest).as_posix()] = {"size": file.stat().st_size, "sha256": digest(file.read_bytes())}
    write_changed(dest / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    verify_assets(dest)
    write_changed(work / "build/fps_config.h", '#define ACNH_FPS_ENABLED 1\n'
                  f'#define ACNH_FPS_MANIFEST_SHA "{digest((dest / "manifest.json").read_bytes())}"\n'
                  f'#define ACNH_FPS_PATCH_SHA "{manifest["files"]["patches.tsv"]["sha256"]}"\n'
                  f'#define ACNH_FPS_ROMFS_SHA "{manifest["files"]["romfs/manifest.tsv"]["sha256"]}"\n'
                  f'#define ACNH_FPS_PATCH_COUNT {len(words)}\n')
    return manifest
