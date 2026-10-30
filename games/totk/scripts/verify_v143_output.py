#!/usr/bin/env python3
"""Execute the production output veneer and the original display-scale path.

Native screenshots/pass dumps still verify actual GPU allocation separately.
No game entry point, graphics or user saves are used by this check.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_PC, UC_ARM64_REG_X0, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
    UC_ARM64_REG_X8, UC_ARM64_REG_X9, UC_ARM64_REG_X10, UC_ARM64_REG_X11, UC_ARM64_REG_X19,
)

ROOT = Path(__file__).resolve().parents[1]
RENDER_SITES = {0x585e48, 0x585e50, 0x585e54, 0x585e60}


def clean_env():
    """The fixture runs the production patches with this environment: a size or export switch left in the shell
    would change words the checks below count."""
    return {k: v for k, v in os.environ.items()
            if k not in ('TOTK_RENDER_RES', 'TOTK_EXPORT_IOS', 'TOTK_OUTPUT_RES')}


def render_res_checks(out, raw, data, layout):
    """Scene render size (TOTK_RENDER_RES, PR #19): nothing changes unless it is set; set, exactly the four 1.4.3
    sites change and the size sits on the data page +16; the patched sequence stores that size where the game stored
    its own and keeps the output width; the iOS export bakes it only when set; a malformed value stops the start."""
    def image(extra):
        path = out / 'main-render-res.bin'
        env = clean_env() | {'TOTK_REALTIME': '1', 'TOTK_FPS60': '1', 'TOTK_OUTPUT_RES': '0'} | extra
        run = subprocess.run(['build/v143-output-fixture', 'rewritten/v143/main.patched.bin', str(path),
                              str(layout['TEXT_END'])], capture_output=True, text=True, env=env)
        got = path.read_bytes() if run.returncode == 0 else None
        path.unlink(missing_ok=True)
        return run.returncode, got

    def sites_changed(got):
        return {p for p in RENDER_SITES if raw[p:p+4] != got[p:p+4]}

    def stores(img):
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(0, (len(img) + 0x3fff) & ~0x3fff)
        mu.mem_write(0, img)
        renderer, source = 0x10000000, 0x10004000
        mu.mem_map(renderer, 0x8000)
        mu.mem_write(source + 0x40, struct.pack('<I', 0x1234))
        mu.mem_write(renderer + 0xc38, struct.pack('<Q', 0x5678))
        mu.mem_write(renderer + 0x9f0, struct.pack('<Q', 0x9abc))
        mu.reg_write(UC_ARM64_REG_X19, renderer)
        mu.reg_write(UC_ARM64_REG_X8, source)
        mu.reg_write(UC_ARM64_REG_X9, 1920)
        mu.reg_write(UC_ARM64_REG_X10, 900)
        mu.reg_write(UC_ARM64_REG_X11, 1600)
        mu.emu_start(0x585e48, 0x585e6c, count=16)
        assert mu.reg_read(UC_ARM64_REG_PC) == 0x585e6c
        assert (mu.reg_read(UC_ARM64_REG_X2), mu.reg_read(UC_ARM64_REG_X9)) == (0x1234, 1920)
        assert (mu.reg_read(UC_ARM64_REG_X8), mu.reg_read(UC_ARM64_REG_X0)) == (0x5678, 0x9abc)
        size = struct.unpack('<ff', mu.mem_read(renderer + 0xcc4, 8))
        output = struct.unpack('<f', mu.mem_read(renderer + 0xcc0, 4))[0]
        return size, output

    code, unset = image({})
    assert code == 0 and sites_changed(unset) == set(), 'TOTK_RENDER_RES unset changed the render-size sites'
    code, set_ = image({'TOTK_RENDER_RES': '960x540'})
    assert code == 0 and sites_changed(set_) == RENDER_SITES, 'TOTK_RENDER_RES=960x540 did not patch the four sites'
    others = {p for p in range(0, len(raw), 4) if unset[p:p+4] != set_[p:p+4]}
    assert others == RENDER_SITES, [hex(p) for p in sorted(others ^ RENDER_SITES)]
    assert struct.unpack('<ff', set_[data + 16:data + 24]) == (960.0, 540.0)
    assert stores(raw) == ((1600.0, 900.0), 1920.0), 'the original sequence no longer stores the game size'
    assert stores(set_) == ((960.0, 540.0), 1920.0), 'the patched sequence does not store 960x540 with the output kept'
    export = {'TOTK_EXPORT_IOS': str(out / 'unused-export')}
    code, export_unset = image(export)
    assert code == 0 and sites_changed(export_unset) == set(), 'the iOS export baked a render size nobody asked for'
    code, export_set = image(export | {'TOTK_RENDER_RES': '960x540'})
    assert code == 0 and sites_changed(export_set) == RENDER_SITES, 'the iOS export did not bake the asked render size'
    code, _ = image({'TOTK_RENDER_RES': '960x540x'})
    assert code != 0, 'a malformed TOTK_RENDER_RES was accepted'
    return dict(changed_words=len(RENDER_SITES), patched_size=[960, 540], kept_output_width=1920,
                ios_export_unset_changed=0, ios_export_set_changed=len(RENDER_SITES), malformed_rejected=True)


def main():
    os.chdir(ROOT)
    out = ROOT / 'analysis/v143-port'
    raw = (ROOT / 'rewritten/v143/main.patched.bin').read_bytes()
    expected = json.loads((ROOT / 'assets/expected-v1.4.3.json').read_text())
    assert hashlib.sha256(raw).hexdigest() == expected['rewritten_sha256']['main.patched.bin']
    layout = json.loads((out / 'runtime-layout.json').read_text())[0]['layout']
    subprocess.run([
        'xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Iruntime', 'scripts/v143_timing_fixture.c', 'runtime/game_patches_v143.c',
        '-o', 'build/v143-output-fixture',
    ], check=True)
    target = out / 'main-output-fixture.bin'
    run = subprocess.run([
        'build/v143-output-fixture', 'rewritten/v143/main.patched.bin', str(target),
        str(layout['TEXT_END']),
    ], capture_output=True, text=True, check=True,
        env=clean_env() | {'TOTK_REALTIME': '1', 'TOTK_FPS60': '1', 'TOTK_OUTPUT_RES': '2560x1440',
                           'TOTK_CLOTH_REALTIME': '0', 'TOTK_HIGHFPS_FIX': '0'})   # their words: verify_v143_timing.py
    (out / 'output-fixture.log').write_text(run.stdout + run.stderr)
    patched = target.read_bytes()
    data = (len(raw) + 0x3fff) & ~0x3fff
    # Same placement as game_patches_v143.c: both veneers sit in the zero
    # padding after the text segment, the frame-step one (4 words) first and
    # this one (7 words) right after it. They must end before read-only data.
    frame_step = (layout['TEXT_END'] + 15) & ~15
    stub = frame_step + 16
    assert stub + 28 <= layout['RO_OFF'], 'output veneer would overlap read-only data'
    assert raw[frame_step:stub + 28] == bytes(stub + 28 - frame_step), \
        'veneer padding not empty in the rewritten image'
    changes = {p for p in range(0, len(raw), 4) if raw[p:p+4] != patched[p:p+4]}
    want = {
        0xbdb578, 0xbdb584, 0x3dcb28,
        0x2db204, 0x2db208, 0x2db22c, 0x2db360, 0x2db36c, 0x2db340, 0x2db34c,
        0x3e5658, 0x2daa54, 0x2daa58, 0x2daa6c, 0x2daa70, 0x2daa74, 0x2daa78,
    } | {frame_step + 4 * i for i in range(4)} | {stub + 4 * i for i in range(7)} \
      | {0x628c74, 0xb43460, 0x1ff04f4, 0x253fbb0} | {frame_step + 48 + 4 * i for i in range(32)} \
      | {0x18e468, 0x23be9b4, 0x23d756c, 0x23d2dd0}   # ultrawide + direct mouse camera (always guarded; disabled until mouse mode)
    assert changes == want, [hex(p) for p in sorted(changes ^ want)]

    def execute(w, h, scale, apply):
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(0, len(patched))
        mu.mem_write(0, patched if apply else raw)
        config = 0x10000000
        mu.mem_map(config, 0x4000)
        mu.mem_write(config, bytes([0xa5]) * 0x600)
        mu.mem_write(config + 0x10, struct.pack('<II', 1920, 1080))
        mu.mem_write(config + 0x0c, struct.pack('<I', 0))
        mu.mem_write(config + 0x520, struct.pack('<ff', *scale))
        mu.mem_write(data + 32, struct.pack('<II', w, h))
        mu.mem_write(data + 40, bytes(8))
        mu.reg_write(UC_ARM64_REG_X19, config)
        mu.reg_write(UC_ARM64_REG_X8, 0)
        mu.reg_write(UC_ARM64_REG_X9, 1 << 21)
        mu.reg_write(UC_ARM64_REG_X3, (1080 << 32) | 1920)
        mu.emu_start(0x3dca48, 0x3dcb30, count=100)
        assert mu.reg_read(UC_ARM64_REG_PC) == 0x3dcb30
        assert mu.reg_read(UC_ARM64_REG_X19) == config
        canvas = struct.unpack('<II', mu.mem_read(config + 0x10, 8))
        crop = struct.unpack('<II', mu.mem_read(config + 0x508, 8))
        assert struct.unpack('<ff', mu.mem_read(config + 0x528, 8)) == scale
        assert bytes(mu.mem_read(config + 0x518, 8)) == bytes([0xa5]) * 8
        if apply:
            assert canvas == crop == (w, h), (canvas, crop)
            assert mu.reg_read(UC_ARM64_REG_X4) == h
            assert struct.unpack('<Q', mu.mem_read(data + 40, 8))[0] == config
        return dict(canvas=canvas, crop=crop, scale=scale)

    cases = []
    for w, h in [(1920, 1080), (2560, 1440), (1920, 1080)]:
        for scale in [(1.0, 1.0), (0.5, 0.75)]:
            cases.append(execute(w, h, scale, True))
    negative = execute(2560, 1440, (1.0, 1.0), False)
    assert negative['canvas'] == negative['crop'] == (1920, 1080)

    # Auto (no TOTK_OUTPUT_RES) on the Mac takes 2560x1440 when the heap overlay exists. iOS shows 720p only: the iOS
    # export (TOTK_EXPORT_IOS) must leave every output word alone, in auto and with an explicit size (2026-10-18).
    overlay = out / 'auto-overlay'
    (overlay / '2560x1440').mkdir(parents=True, exist_ok=True)
    (overlay / '2560x1440/manifest.tsv').write_text('# presence is what auto looks for\n')
    output_sites = {0xbdb578, 0xbdb584, 0x3dcb28}

    def output_words(extra):
        image = out / 'main-output-auto.bin'
        env = clean_env()
        env |= {'TOTK_REALTIME': '1', 'TOTK_FPS60': '1', 'TOTK_ROMFS_PATCHES_OUT': str(overlay)} | extra
        subprocess.run(['build/v143-output-fixture', 'rewritten/v143/main.patched.bin', str(image),
                        str(layout['TEXT_END'])], capture_output=True, text=True, check=True, env=env)
        got = image.read_bytes(); image.unlink()
        return {p for p in output_sites if raw[p:p+4] != got[p:p+4]}

    export = {'TOTK_EXPORT_IOS': str(out / 'unused-export')}
    auto_mac = output_words({})
    auto_export = output_words(export)
    explicit_export = output_words(export | {'TOTK_OUTPUT_RES': '2560x1440'})
    assert auto_mac == output_sites, ('auto on the Mac did not take 2560x1440', auto_mac)
    assert auto_export == set(), ('the iOS export took the 2560x1440 swapchain in auto', auto_export)
    assert explicit_export == set(), ('the iOS export took an explicit output size', explicit_export)
    render = render_res_checks(out, raw, data, layout)
    report = dict(status='pass', version='v143', cases=cases,
                  negative_control=negative, veneer_offset=hex(stub),
                  auto={'mac_changed_output_words': len(auto_mac), 'ios_export_auto_changed': len(auto_export), 'ios_export_explicit_changed': len(explicit_export)},
                  render_res=render,
                  changed_original_words=len(changes),
                  scope='production C patch, original scale selection, canvas/crop publication; GPU allocations tested separately')
    (out / 'output-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
