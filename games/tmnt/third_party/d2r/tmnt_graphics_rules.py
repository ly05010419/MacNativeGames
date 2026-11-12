"""Checked original-input sites for TMNT's opening graphics phase gates.

This module contains only offsets, instruction guards and validation. The
rewriter emits its normal gate stubs and output manifests from these sites.
No game program bytes are stored here beyond the seven checked AArch64 words.
"""
import hashlib
import struct

MAIN_BUILD_ID = '18c437d0f8d74cc1f8ea5d927b03ec37'
MAIN_SHA256 = 'cc2ce61a94ee933a31423abae8017b36bcf5b95861acf54b22d551e4a4f9d73a'
# (offset, exact original instruction). Six count operations are emitted at
# four sites; the three exception lifecycle operations use the last sites.
SITE_WORDS = (
    (0xfd34c, 0xb9016100),  # set STR W0, [X8,#0x160]
    (0xfd28c, 0xb9016109),  # clear STR W9, [X8,#0x160]
    (0xfdf70, 0xb9416109),  # Present LDR W9, [X8,#0x160]
    (0xfdf74, 0x34000429),  # Present CBZ W9, success
    (0x8a8920, 0x14000001),  # caught return: B +4
    (0x8a9034, 0xf90007e0),  # rethrow: STR X0, [SP,#8]
    (0x8a9060, 0x910903ff),  # common return: ADD SP,SP,#0x240
)


def preflight(nso, image, module, enabled):
    """Fail closed before any rewrite; only the verified base main is wired."""
    if not enabled or module != 'main':
        return False
    if len(nso) < 0x50 or nso[:4] != b'NSO0':
        raise ValueError('TMNT graphics rules need an original NSO main')
    if nso[0x40:0x50].hex() != MAIN_BUILD_ID:
        raise ValueError('unsupported TMNT main build ID')
    if hashlib.sha256(nso).hexdigest() != MAIN_SHA256:
        raise ValueError('unsupported TMNT main input hash')
    for offset, word in SITE_WORDS:
        if offset + 4 > len(image) or struct.unpack_from('<I', image, offset)[0] != word:
            raise ValueError(f'TMNT original instruction mismatch at {offset:#x}')
    return True


def require_clear_sites(image, text_end, occupied, windows):
    """Validate the normal rewriter did not claim or alter a TMNT site."""
    for offset, word in SITE_WORDS:
        if offset & 3 or offset + 4 > text_end:
            raise ValueError(f'TMNT graphics gate outside text at {offset:#x}')
        if offset in occupied or any(lo <= offset < hi for lo, hi in windows):
            raise ValueError(f'TMNT graphics gate collides at {offset:#x}')
        if struct.unpack_from('<I', image, offset)[0] != word:
            raise ValueError(f'TMNT graphics gate word changed at {offset:#x}')


def emit_site_plan(site, word, gate, emit, here, branch):
    """Emit the original operation and both destinations through normal stubs.

    `gate` emits a host gate with the public rewriter's register-saving bridge;
    `emit`, `here` and `branch` are its normal checked AArch64 emitters.
    """
    if site == 0xfd34c:  # set count store
        gate(site, 0x90); emit(word); gate(site, 0x95)
    elif site == 0xfd28c:  # clear count store
        gate(site, 0x96); emit(word); gate(site, 0x91)
    elif site == 0xfdf70:  # Present count load
        gate(site, 0x92); emit(word)
    elif site == 0xfdf74:  # original zero -> success, nonzero -> error
        gate(site, 0x93)
        emit(0x34000049)  # cbz w9, next + 8
        emit(branch(here(), 0xfdf78))
        emit(branch(here(), 0xfdff8))
        return
    else:  # caught mark / rethrow / caught finish after cleanup
        gate(site, {0x8a8920: 0x98, 0x8a9034: 0x97,
                    0x8a9060: 0x99}[site])
        if site != 0x8a8920: emit(word)
    emit(branch(here(), site + 4))
