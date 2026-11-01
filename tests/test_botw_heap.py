"""games/totk/runtime/botw_heap.c (E9 S9.5): Breath of the Wild's heap plan, one table per version (1.6.0 and 1.9.0),
built with xcrun clang into a harness and run on a synthetic main image per version that holds that version's words of
every site and neighbour (the C table) and a non-zero pattern elsewhere in the code, zero padding after it. Checked for
each version: nothing at all without the 2K output or the 8 GB mode; the 2K output's two words (as patch_output_res
always wrote them on 1.6.0); in the 8 GB mode only the listed words and the veneer bytes change, capstone decodes each,
and unicorn runs each heap veneer from its site to the instruction after it (every general register, SP and NZCV set
first: only the size register changes, to the planned size) and each of the ten other sites (the size the game gets);
any one checked word altered, a used veneer byte, too little room, a small pool, a small cap, an overflowing sum or the
other version's image refuse with the image unchanged byte for byte. Capstone and unicorn are optional elsewhere
(skipped without them). Real images: BOTW_HEAP_MAIN=<1.6.0 main.patched.bin> BOTW_HEAP_TEXT_END=<hex>, and
BOTW_HEAP_MAIN_190=<main>[:<main>...] BOTW_HEAP_TEXT_END_190=<hex> (both 1.9.0 mains, 6d48 and e84d).
How game_patches_botw.c and the Makefile use the plan: tests/test_botw_heap_integration.py."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
except ImportError:
    Cs = Uc = None

RUNTIME = Path(__file__).resolve().parents[1] / 'games/totk/runtime'
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "botw_heap.h"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const BotwHeapTable *t = !strcmp(argv[1], "160") ? &botw_heap_160 : !strcmp(argv[1], "190") ? &botw_heap_190 : NULL;
    if (!t) return 2;
    if (argc == 3) {
        for (unsigned i = 0; i < t->count; i++) printf("%x %x %x\n", t->sites[i].off, t->sites[i].original, t->sites[i].big);
        return 0;
    }
    if (argc < 10) return 2;   /* version image out 2k 8gb pool cap code text_end [off ...] */
    FILE *f = fopen(argv[2], "rb"); if (!f) return 3;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n), *before = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) return 4;
    fclose(f);
    for (int i = 10; i < argc; i++) b[strtoull(argv[i], NULL, 16)] ^= 0x01;   /* one bit flipped at each offset given */
    memcpy(before, b, n);
    BotwHeapPlan p; char err[256];
    int r = botw_heap_prepare(t, b, n, strtoull(argv[9], NULL, 16), atoi(argv[4]), atoi(argv[5]), strtoull(argv[6], NULL, 16),
                              strtoull(argv[7], NULL, 16), strtoull(argv[8], NULL, 16), &p, err, sizeof err);
    if (r) printf("refused %s|%s\n", err, memcmp(b, before, n) ? "changed" : "unchanged");
    else { printf("ok %llx %llx %llx %u\n", (unsigned long long)p.heap, (unsigned long long)p.root, (unsigned long long)p.veneer, p.n);
           botw_heap_apply(b, &p); }
    f = fopen(argv[3], "wb"); fwrite(b, 1, n, f); fclose(f);
    return 0;
}
'''
POOL_8GB_2K, CAP_8GB, CODE = 0x1c1400000, 0x1c0000000, 0x415c000   # services.c's numbers; the 1.6.0 code size measured
BASE = 0x10000000
# Per version: the synthetic image (code ends 4 bytes into a 16-byte slot: a sound-link veneer slot, then ours), the
# heap and root sites with the root's register, the ten sizes (and the register each goes to, w0 otherwise), the site
# whose movk follows it.
VERSIONS = {
    '160': dict(text_end=0x1cf0004, size=0x1d00000, heap_at=0xe536dc, root_at=0xe53fa0, root_reg=21, movk_site=0x1c8f0a0,
                sizes={0xdfa578: 0x3600000, 0x182f368: 0x45000000, 0x182f39c: 0xc00000, 0x182f3e0: 0x37000000,
                       0x1c8f060: 0x3780000, 0x1c8f0a0: 0x28000, 0x1c8f0d0: 0x90000, 0x1c8f108: 0x40000,
                       0x1620708: 0x1280000, 0x16216e0: 0x780000, 0x182fad0: 0x7f000000, 0x182fb24: 0x20000000},
                registers={0x182fad0: 8, 0x182fb24: 10}),
    '190': dict(text_end=0x15a0004, size=0x15b0000, heap_at=0x71c, root_at=0xc617c0, root_reg=8, movk_site=0x156dffc,
                sizes={0xe61348: 0x3600000, 0x131aef8: 0x45000000, 0x131b278: 0xc00000, 0x131b49c: 0x37000000,
                       0x156dfc8: 0x3780000, 0x156dffc: 0x28000, 0x156e034: 0x90000, 0x156e080: 0x40000,
                       0x112b198: 0x1280000, 0x112b360: 0x780000, 0x127b6e0: 0x7f000000, 0x127b774: 0x20000000},
                registers={0x156dfc8: 3, 0x127b6e0: 9, 0x127b774: 8}),
}


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and the Xcode command line tools')
class Harness(unittest.TestCase):
    """Builds the harness once for both versions."""
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.addClassCleanup(shutil.rmtree, cls.tmp)
        (cls.tmp / 'harness.c').write_text(HARNESS)
        cls.binary = cls.tmp / 'harness'
        build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{RUNTIME}', str(cls.tmp / 'harness.c'),
                                str(RUNTIME / 'botw_heap.c'), '-o', str(cls.binary)], capture_output=True, text=True, timeout=120)
        if build.returncode:
            raise AssertionError(build.stderr[-2000:])

    @classmethod
    def table(cls, version):
        out = subprocess.run([str(cls.binary), version, 'table'], capture_output=True, text=True, check=True, timeout=60).stdout
        return [tuple(int(v, 16) for v in line.split()) for line in out.splitlines()]

    @classmethod
    def synthetic(cls, version):
        v = VERSIONS[version]
        image = bytearray(b'\xa5' * v['text_end'] + bytes(v['size'] - v['text_end']))
        for off, original, _ in cls.table(version):
            image[off:off + 4] = original.to_bytes(4, 'little')
        return bytes(image)


class HeapPlanCases:
    """The same cases for each version's table; the subclasses name the version."""
    version = None

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        v = VERSIONS[cls.version]
        cls.text_end, cls.size, cls.heap_at, cls.root_at = v['text_end'], v['size'], v['heap_at'], v['root_at']
        cls.sizes, cls.registers, cls.root_reg, cls.movk_site = v['sizes'], v['registers'], v['root_reg'], v['movk_site']
        cls.veneer = ((cls.text_end + 15) & ~15) + 0x10                # behind the sound-link veneer's 16 bytes
        cls.sites = cls.table(cls.version)
        cls.image = cls.synthetic(cls.version)
        cls.path = cls.tmp / f'main{cls.version}.bin'
        cls.path.write_bytes(cls.image)

    def run_plan(self, output_2k, dram, pool=POOL_8GB_2K, cap=CAP_8GB, code=CODE, text_end=None, flips=(), path=None,
                 version=None):
        out = self.tmp / 'out.bin'
        args = [str(self.binary), version or self.version, str(path or self.path), str(out), str(int(output_2k)), str(int(dram)),
                f'{pool:x}', f'{cap:x}', f'{code:x}', f'{text_end or self.text_end:x}'] + [f'{o:x}' for o in flips]
        line = subprocess.run(args, capture_output=True, text=True, check=True, timeout=60).stdout.strip()
        return line, out.read_bytes()

    def changed(self, after):
        """Offsets of the 4-byte words that differ from the image (64 KiB blocks compared first)."""
        found = []
        for block in range(0, self.size, 0x10000):
            if self.image[block:block + 0x10000] != after[block:block + 0x10000]:
                found += [o for o in range(block, block + 0x10000, 4) if self.image[o:o + 4] != after[o:o + 4]]
        return found

    def refused(self, line):
        self.assertTrue(line.startswith('refused '), line)
        self.assertTrue(line.endswith('|unchanged'), line)

    def test_the_table(self):
        self.assertEqual(len(self.sites), 42)
        self.assertEqual(sorted(off for off, _, big in self.sites if big), sorted(self.sizes))
        self.assertEqual((self.sites[1][0], self.sites[4][0]), (self.heap_at, self.root_at))

    def test_nothing_without_the_2k_output_or_the_8gb_mode(self):
        line, after = self.run_plan(False, False)
        self.assertEqual((line, after == self.image), ('ok c3000000 c1000000 0 0', True))

    def test_the_2k_output_alone_writes_the_two_words(self):
        line, after = self.run_plan(True, False, pool=1, cap=1)          # no budget there: the game checks as before
        self.assertEqual(line, 'ok d1000000 cf000000 0 2')
        self.assertEqual(self.changed(after), sorted([self.heap_at, self.root_at]))
        self.assertEqual((after[self.heap_at:self.heap_at + 4].hex(), after[self.root_at:self.root_at + 4].hex()),
                         ((0x52ba2000).to_bytes(4, 'little').hex(), (0x52b9e000 | self.root_reg).to_bytes(4, 'little').hex()))

    def test_8gb_writes_only_the_listed_words_and_the_veneers(self):
        for output_2k, heap in ((True, 0x18e000000), (False, 0x180000000)):
            with self.subTest(output_2k=output_2k):
                line, after = self.run_plan(output_2k, True)
                self.assertEqual(line, f'ok {heap:x} {heap - (32 << 20):x} {self.veneer:x} 20')
                want = sorted([self.heap_at, self.root_at] + list(self.sizes) + [self.veneer + i for i in (0, 4, 8, 16, 20, 24)])
                self.assertEqual(self.changed(after), want)

    def test_any_altered_word_refuses_with_nothing_written(self):
        for off, _, _ in self.sites:
            with self.subTest(site=hex(off)):
                self.refused(self.run_plan(True, True, flips=(off,))[0])
        for off in (self.veneer, self.veneer + 0x1f):                     # a byte the veneers would take
            self.refused(self.run_plan(True, True, flips=(off,))[0])
        self.assertTrue(self.run_plan(True, True, flips=(self.veneer - 1,))[0].startswith('ok '))   # the sound-link's: not ours

    def test_the_other_versions_image_refuses_with_nothing_written(self):
        other = '190' if self.version == '160' else '160'
        path = self.tmp / f'other{other}.bin'
        path.write_bytes(self.synthetic(other))
        self.refused(self.run_plan(True, True, path=path, text_end=VERSIONS[other]['text_end'])[0])

    def test_the_budget_is_what_setheapsize_checks(self):
        heap = 0x18e000000
        self.assertTrue(self.run_plan(True, True, pool=heap + CODE, cap=heap)[0].startswith('ok '))   # exactly full
        self.refused(self.run_plan(True, True, pool=heap + CODE - 0x1000)[0])
        self.refused(self.run_plan(True, True, cap=heap - 0x200000)[0])
        self.refused(self.run_plan(True, True, code=(1 << 64) - 0x1000, pool=(1 << 64) - 1)[0])          # the sum overflows
        self.assertIn('TOTK_APP_POOL', self.run_plan(True, True, pool=0xcd500000)[0])
        self.assertIn('TOTK_APP_POOL', self.run_plan(False, True, pool=0xdb500000)[0])   # the pool render-res sets on 1.9.0

    def test_undoing_the_writes_restores_the_original(self):
        """Every written word put back from the table and the veneer bytes cleared: the image is the original again."""
        _, after = self.run_plan(True, True)
        undone = bytearray(after)
        for off, original, big in self.sites:
            if big or off in (self.heap_at, self.root_at):
                undone[off:off + 4] = original.to_bytes(4, 'little')
        undone[self.veneer:self.veneer + 0x20] = bytes(0x20)
        self.assertEqual((after != self.image, bytes(undone) == self.image), (True, True))

    def test_no_room_after_the_code(self):
        page_end = (self.text_end + 0xfff) & ~0xfff
        self.refused(self.run_plan(True, True, text_end=page_end - 0x20)[0])   # 0x10 left after the sound-link's 16 bytes
        self.refused(self.run_plan(True, True, text_end=self.size + 0x100000)[0])   # past the module

    @unittest.skipIf(Cs is None, 'capstone and unicorn are not installed')
    def test_capstone_reads_every_written_word(self):
        _, after = self.run_plan(True, True)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        text = lambda off: ' '.join(next(md.disasm_lite(after[off:off + 4], off))[2:4])
        v, r = self.veneer, self.root_reg
        self.assertEqual((text(self.heap_at), text(self.root_at)), (f'b #{v:#x}', f'b #{v + 16:#x}'))
        self.assertEqual([text(v + i) for i in (0, 4, 8)], ['mov x0, #0x8e000000', 'movk x0, #1, lsl #32', f'b #{self.heap_at + 4:#x}'])
        self.assertEqual([text(v + 16 + i) for i in (0, 4, 8)], [f'mov x{r}, #0x8c000000', f'movk x{r}, #1, lsl #32',
                                                                 f'b #{self.root_at + 4:#x}'])
        for off, size in self.sizes.items():
            want = 'mov w0, #0x8000' if off == self.movk_site else f'mov w{self.registers.get(off, 0)}, #{size:#x}'   # its movk adds 0x20000
            self.assertEqual(text(off), want)

    def machine(self, image, start, end, regs):
        """Run image from start to end with regs set; only the site's and the veneers' 16 KiB pages are mapped (as
        games/botw/scripts/verify_realtime_patch.py maps them: one large map stops unicorn with a bus error here)."""
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for page in sorted({start & ~0x3fff, self.veneer & ~0x3fff}):
            mu.mem_map(BASE + page, 0x4000)
            mu.mem_write(BASE + page, image[page:page + 0x4000])
        for reg, value in regs.items():
            mu.reg_write(reg, value)
        mu.emu_start(BASE + start, BASE + end)
        return {reg: mu.reg_read(reg) for reg in regs}

    @unittest.skipIf(Cs is None, 'capstone and unicorn are not installed')
    def test_unicorn_runs_the_veneers_and_the_sizes(self):
        _, after = self.run_plan(True, True)
        regs = {getattr(A, f'UC_ARM64_REG_X{i}'): 0x1111111100000000 + i * 0x10101 for i in range(29)}
        regs.update({A.UC_ARM64_REG_X29: 0x2929292929, A.UC_ARM64_REG_X30: 0x3030303030, A.UC_ARM64_REG_SP: BASE + self.size - 0x100,
                     A.UC_ARM64_REG_NZCV: 0xa0000000})
        root_reg = getattr(A, f'UC_ARM64_REG_X{self.root_reg}')
        for site, reg, size in ((self.heap_at, A.UC_ARM64_REG_X0, 0x18e000000), (self.root_at, root_reg, 0x18c000000)):
            with self.subTest(site=hex(site)):
                got = self.machine(after, site, site + 4, regs)
                self.assertEqual(got[reg], size)
                self.assertEqual({r: v for r, v in got.items() if r != reg}, {r: v for r, v in regs.items() if r != reg})
        for off, size in self.sizes.items():
            with self.subTest(site=hex(off)):
                end = off + (8 if off == self.movk_site else 4)              # through the movk that follows it
                reg = getattr(A, f'UC_ARM64_REG_X{self.registers.get(off, 0)}')
                self.assertEqual(self.machine(after, off, end, regs)[reg], size)

    def real_mains(self):
        name = 'BOTW_HEAP_MAIN' if self.version == '160' else 'BOTW_HEAP_MAIN_190'
        paths = [Path(p) for p in os.environ.get(name, '').split(':') if p]
        if not paths:
            self.skipTest(f'{name} unset: no real {self.version} main.patched.bin')
        return paths, int(os.environ[f'{"BOTW_HEAP_TEXT_END" if self.version == "160" else "BOTW_HEAP_TEXT_END_190"}'], 16)

    def test_a_real_main(self):
        paths, text_end = self.real_mains()
        stub = (text_end + 15) & ~15
        for path in paths:
            with self.subTest(main=path.name):
                for output_2k, heap in ((True, 0x18e000000), (False, 0x180000000)):
                    line, after = self.run_plan(output_2k, True, text_end=text_end, path=path)
                    self.assertEqual(line, f'ok {heap:x} {heap - (32 << 20):x} {stub + 0x10:x} 20')
                    self.assertEqual(after[stub:stub + 16], bytes(16))       # the sound-link's slot left to it
                self.refused(self.run_plan(True, True, text_end=text_end, path=path,
                                           version='190' if self.version == '160' else '160')[0])   # the other table


class HeapPlan160(HeapPlanCases, Harness):
    version = '160'


class HeapPlan190(HeapPlanCases, Harness):
    version = '190'


if __name__ == '__main__':
    unittest.main()
