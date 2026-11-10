#!/usr/bin/env python3
"""Differentially execute original BLR x18 and the prepared BLR x30 window.

Tests callee-entry state, actual LR observations, stack arguments, callee return,
recursive calls, invalid targets, borrowed registers, flags, vectors, and four
deliberately broken patches. Unicorn is used only for verification.
"""
import json
import random
import struct
import sys
import unittest
from pathlib import Path
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm64_const import *

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
from call_window import BLR_X18, BLR_X30, branch, prepare_call

BASE, STACK, TSD = 0x100000, 0x300000, 0x500000
SP = STACK + 0x10000
OUTER, CALLEE, PATCH, KEYPAGE = 0x100, 0x800, 0x2000, 0x3000
STORE = 0xA901BFF1  # stp x17,x15,[sp,#24]
GP = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
# Unicorn's X0..X28 register IDs are contiguous; X29/X30 are separate aliases.
VECTORS = [UC_ARM64_REG_Q0 + i for i in range(32)]


def put(image, address, *words):
    image[address:address + 4 * len(words)] = struct.pack('<' + 'I' * len(words), *words)


def image(patched, recursive, mutation=None, prefix_word=STORE):
    blob = bytearray(0x4000)
    put(blob, OUTER, prefix_word, BLR_X18, 0xD503201F)
    if recursive:
        put(blob, CALLEE, 0xA9BC7BFD, 0x910003FD, 0x71000694, 0x54000060,
            STORE, BLR_X18, 0x91000400, 0xA8C47BFD, 0xD65F03C0)
        windows = [(OUTER, PATCH), (CALLEE + 16, PATCH + 0x100)]
    else:
        # Observe the real architectural link and stack arguments, then clobber
        # caller registers/flags/vector 0 and return through that same link.
        put(blob, CALLEE, 0xF900027E, 0xF9000660, 0xF9400FE0, 0xF94013E1,
            0x8B010000, 0xCA000021, 0xA9010660, 0xD2824691, 0x9E670000,
            0xEB01001F, 0xD65F03C0)
        windows = [(OUTER, PATCH)]
    if patched:
        for prefix, patch in windows:
            code = prepare_call(prefix_word, prefix, prefix + 4, patch, KEYPAGE, 0)
            blob[patch:patch + len(code)] = code
            put(blob, prefix, branch(prefix, patch), BLR_X30)
        if mutation == 'wrong_call_register': put(blob, OUTER + 4, 0xD63F0200)
        if mutation == 'skip_call': put(blob, PATCH + 32, branch(PATCH + 32, OUTER + 8))
        if mutation == 'no_scratch_restore': put(blob, PATCH + 28, 0xD503201F)
        if mutation == 'no_original_store': put(blob, PATCH, 0xD503201F)
    return blob


def execute(seed, patched, recursive=False, mutation=None, target=None, prefix_word=STORE):
    rng = random.Random(seed)
    registers = [rng.getrandbits(64) for _ in range(31)]
    registers[18] = BASE + CALLEE if target is None else target
    registers[19] = STACK + 0x200
    registers[20] = rng.randrange(1, 9)
    if prefix_word != STORE: registers[29] = SP + 0x80
    vectors = [rng.getrandbits(128) for _ in range(32)]
    slot = rng.randrange(32, 480)
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(BASE, 0x4000)
    u.mem_write(BASE, bytes(image(patched, recursive, mutation, prefix_word)))
    u.mem_map(STACK, 0x20000)
    u.mem_write(STACK, rng.randbytes(0x20000))
    u.mem_map(TSD, 0x10000)
    u.mem_write(BASE + KEYPAGE, struct.pack('<Q', slot * 8))
    u.mem_write(TSD + slot * 8, struct.pack('<Q', registers[18]))
    for reg, value in zip(GP, registers): u.reg_write(reg, value)
    for reg, value in zip(VECTORS, vectors): u.reg_write(reg, value)
    u.reg_write(UC_ARM64_REG_TPIDRRO_EL0, TSD)
    u.reg_write(UC_ARM64_REG_SP, SP)
    u.reg_write(UC_ARM64_REG_NZCV, rng.randrange(16) << 28)
    if patched: u.reg_write(GP[18], 0xBAD0BAD0BAD0BAD0)
    entries, fault = [], []

    def state():
        g = [u.reg_read(r) for r in GP]
        if patched: g[18] = struct.unpack('<Q', u.mem_read(TSD + slot * 8, 8))[0]
        return (g, u.reg_read(UC_ARM64_REG_SP), u.reg_read(UC_ARM64_REG_NZCV),
                [u.reg_read(r) for r in VECTORS])

    def hook(uc, address, size, _):
        if address == BASE + CALLEE: entries.append(state())
        if address == BASE + OUTER + 8: uc.emu_stop()

    def invalid(uc, access, address, size, value, _):
        fault.append((access, address, size)); return False

    u.hook_add(UC_HOOK_CODE, hook)
    u.hook_add(UC_HOOK_MEM_INVALID, invalid)
    error = None
    try: u.emu_start(BASE + OUTER, BASE + 0x4000, count=3000)
    except UcError as exc: error = exc.errno
    # Temporary red-zone spill bytes are outside the original store footprint.
    # Every other arena byte must agree, including all positive stack arguments.
    arena = bytearray(u.mem_read(STACK, 0x20000))
    if recursive:
        for depth in range(registers[20]):
            offset = SP - depth * 64 - STACK - 16
            arena[offset:offset + 8] = bytes(8)
    else:
        offset = SP - STACK - 16
        arena[offset:offset + 8] = bytes(8)
    return {'state': state(), 'entries': entries, 'arena': bytes(arena),
            'pc': u.reg_read(UC_ARM64_REG_PC), 'fault': fault, 'error': error}


class CallWindowTest(unittest.TestCase):
    def test_v143_frame_pointer_load_and_store(self):
        for prefix_word in (0xf85e83a0, 0xb81f83af):
            for seed in range(64):
                with self.subTest(prefix=hex(prefix_word), seed=seed):
                    self.assertEqual(execute(seed, False, prefix_word=prefix_word),
                                     execute(seed, True, prefix_word=prefix_word))
    def test_actual_stack_store_and_callee_return(self):
        for seed in range(256):
            with self.subTest(seed=seed):
                original = execute(seed, False)
                self.assertIsNone(original['error'])
                self.assertEqual(original['pc'], BASE + OUTER + 8)
                self.assertEqual(original, execute(seed, True))

    def test_recursive_callee_entry_link_and_stack(self):
        for seed in range(128):
            with self.subTest(seed=seed):
                original = execute(seed, False, True)
                self.assertIsNone(original['error'])
                self.assertGreaterEqual(len(original['entries']), 1)
                self.assertEqual(original, execute(seed, True, True))

    def test_invalid_targets(self):
        for target in [0, 0x900000, BASE + CALLEE + 1]:
            for seed in range(8):
                with self.subTest(target=target, seed=seed):
                    self.assertEqual(execute(seed, False, target=target), execute(seed, True, target=target))

    def test_negative_controls(self):
        for mutation in ['wrong_call_register', 'skip_call', 'no_scratch_restore', 'no_original_store']:
            with self.subTest(mutation=mutation):
                self.assertNotEqual(execute(15, False), execute(15, True, mutation=mutation))

    def test_reject_unsafe_prefix(self):
        for word in [0xD65F03C0, 0x910043FF, 0x58000000, 0xA9BF7BFD]:
            with self.subTest(word=word):
                with self.assertRaises(ValueError):
                    prepare_call(word, OUTER, OUTER + 4, PATCH, KEYPAGE, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
