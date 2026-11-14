#!/usr/bin/env python3
"""Run Breath of the Wild 1.9.0's hkQuaternion::getAngle (main+0xb06d4, 2 acos(|w|)) and the veneer patch_kf_angle of
games/totk/runtime/game_patches_botw.c writes in its place (2 atan2(|xyz|, |w|)) with Unicorn, on turns from 0.1 mrad
to 3 rad, with the sign of w both ways.

The veneer's words are read from the C file, the bl to atan2f is encoded as the C does for the 1.9.0 text end, and the
check[] words are compared with the image. The two library calls (acosf, atan2f through the PLT) are answered in single
precision. Patched, every turn comes back within float rounding. The shipped function is the negative control: it reads
turns under about half a milliradian as none (a tilting Divine Beast's floor over one high-frame-rate physics step,
#209 of SavageWindRecomp), and it must agree with the veneer on large turns.
usage: verify_kf_angle_patch.py [main.patched.bin]   (default totk/rewritten/botw190/main.patched.bin)"""
import json
import math
import re
import struct
import sys
from pathlib import Path

import numpy as np
from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_MODE_ARM, Uc
from unicorn import arm64_const as A

ROOT = Path(__file__).resolve().parents[2] / "totk"
TEXT_END = 0x15a1c64                        # end of 1.9.0 main's text segment
STUB = ((TEXT_END + 15) & ~15) + 0x300      # where patch_realtime puts the veneer
GET_ANGLE, SITE, ACOSF, ATAN2F = 0xb06d4, 0x12d57fc, 0x159fed0, 0x159ff10
SCRATCH, RET = 0x4000_0000, 0x4000_1000


def c_tables():
    """check {off: word} and the veneer words of patch_kf_angle in the 1.9.0 branch of game_patches_botw.c."""
    c = (ROOT / "runtime/game_patches_botw.c").read_text()
    body = c[c.index("static void patch_kf_angle("):]
    body = body[:body.index("\n}\n")]
    check_text = re.sub(r"/\*.*?\*/", "", body[body.index("check[] = {"):body.index("};", body.index("check[] = {"))], flags=re.S)
    check = {int(o, 16): int(w, 16) for o, w in re.findall(r"\{0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u\}", check_text)}
    assert len(check) == check_text.count("{0x"), "a check[] row of patch_kf_angle is in a form this script cannot read"
    veneer_text = body[body.index("veneer[] = {"):body.index("};", body.index("veneer[] = {"))]
    words = []
    for line in veneer_text.split("\n")[1:]:
        m = re.match(r"\s*0x([0-9a-f]{8})u(\s*\|)?", line)
        if m:
            w = int(m.group(1), 16)
            if m.group(2):                  # 0x94000000u | <offset to atan2f>: the bl
                w |= ((ATAN2F - (STUB + 4 * len(words))) >> 2) & 0x3ffffff
            words.append(w)
    return check, words


def main(image_path):
    image = image_path.read_bytes()
    check, veneer = c_tables()
    for off, word in check.items():
        have, = struct.unpack_from("<I", image, off)
        if have != word:
            raise SystemExit(f"original instruction mismatch at main+{off:#x}: {have:08x} != {word:08x}")
    assert len(veneer) == 14, len(veneer)
    assert image[STUB:STUB + 4 * len(veneer)] == bytes(4 * len(veneer)) or len(image) <= STUB, "veneer padding not empty"
    site_bl = 0x94000000 | (((STUB - SITE) >> 2) & 0x3ffffff)   # what the C writes at the call site
    assert ((site_bl & 0x3ffffff) << 2) + SITE == STUB

    def angle(entry, q):
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(0, (max(len(image), STUB + 0x100) + 0xfff) & ~0xfff)
        mu.mem_write(0, image)
        mu.mem_write(STUB, struct.pack(f"<{len(veneer)}I", *veneer))
        mu.mem_map(SCRATCH, 0x100000)
        mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        mu.mem_write(SCRATCH, struct.pack("<4f", *q))
        for reg, value in ((A.UC_ARM64_REG_X0, SCRATCH), (A.UC_ARM64_REG_SP, SCRATCH + 0x80000), (A.UC_ARM64_REG_LR, RET)):
            mu.reg_write(reg, value)

        def library(uc, address, size, _):
            f = lambda r: np.float32(struct.unpack("<f", struct.pack("<I", uc.reg_read(r) & 0xffffffff))[0])
            r = np.arccos(f(A.UC_ARM64_REG_S0)) if address == ACOSF else np.arctan2(f(A.UC_ARM64_REG_S0), f(A.UC_ARM64_REG_S1))
            uc.reg_write(A.UC_ARM64_REG_S0, struct.unpack("<I", struct.pack("<f", float(r)))[0])
            uc.reg_write(A.UC_ARM64_REG_PC, uc.reg_read(A.UC_ARM64_REG_LR))
        for stub in (ACOSF, ATAN2F):
            mu.hook_add(UC_HOOK_CODE, library, begin=stub, end=stub)
        mu.emu_start(entry, RET, count=200)
        assert mu.reg_read(A.UC_ARM64_REG_PC) == RET
        return struct.unpack("<f", struct.pack("<I", mu.reg_read(A.UC_ARM64_REG_S0) & 0xffffffff))[0]

    rows, lost = [], set()
    axis = np.array([0.36, -0.48, 0.8])
    for turn in (1e-4, 2.9e-4, 4.3e-4, 8.7e-4, 1.7e-3, 1e-2, 0.5, 2.0, 3.0):
        for sign in (1, -1):
            q = [float(np.float32(a * math.sin(turn / 2))) for a in axis] + [float(np.float32(sign * math.cos(turn / 2)))]
            shipped, patched = angle(GET_ANGLE, q), angle(STUB, q)
            assert abs(patched - turn) <= max(2e-6, 2e-6 * turn), (turn, sign, patched)
            if turn >= 1e-2:
                assert abs(shipped - patched) <= 1e-5 * max(1.0, turn), (turn, shipped, patched)
            if shipped == 0.0:
                lost.add(turn)
            rows.append({"turn_rad": turn, "w_sign": sign, "shipped": shipped, "patched": patched})
    assert lost == {1e-4, 2.9e-4, 4.3e-4}, sorted(lost)   # negative control: the shipped angle drops these turns
    print(json.dumps({"status": "pass", "image": str(image_path), "veneer": hex(STUB),
                      "shipped_reads_as_zero_rad": sorted(lost), "turns": rows}, indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "rewritten/botw190/main.patched.bin")
