"""The in-repo translator (dm/gfx/tools/dmsh_xlate_bin) runs on both Mac architectures (GitHub issue #803): its launcher is universal, and every dll is AnyCPU, since the
launcher's x86_64 half cannot load a dll built for arm64 (a build with -r osx-arm64 makes one unless the csproj pins PlatformTarget). Reads the headers only: no .NET needed."""
from pathlib import Path
import struct
import unittest

REPO = Path(__file__).resolve().parents[3]
BIN = REPO / 'dm/gfx/tools/dmsh_xlate_bin'

I386, PE32 = 0x14C, 0x10B
ILONLY, REQUIRED32, PREFERRED32 = 0x1, 0x2, 0x20000


def pe_arch(data):
    """(machine, optional header magic, CLI flags) of a .NET PE file"""
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0', 'not a PE file'
    machine, nsect = struct.unpack_from('<HH', data, pe + 4)
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    cli_rva = struct.unpack_from('<I', data, opt + (208 if magic == PE32 else 224))[0]   # data directory 14, the CLI header
    assert cli_rva, 'no CLI header'
    for s in range(opt + opt_size, opt + opt_size + 40 * nsect, 40):
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, s + 8)
        if va <= cli_rva < va + max(vsize, rawsize):
            return machine, magic, struct.unpack_from('<I', data, raw + cli_rva - va + 16)[0]
    raise AssertionError('CLI header outside every section')


def macho_archs(data):
    magic, n = struct.unpack_from('>II', data, 0)
    assert magic == 0xCAFEBABE, 'not a universal binary'
    return sorted({0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(struct.unpack_from('>I', data, 8 + 20 * i)[0], '?') for i in range(n))


class AnyCpu(unittest.TestCase):
    def test_dlls_are_anycpu(self):
        dlls = sorted(BIN.glob('*.dll'))
        self.assertIn(BIN / 'dmsh_xlate.dll', dlls)
        for dll in dlls:
            with self.subTest(dll=dll.name):
                machine, magic, flags = pe_arch(dll.read_bytes())
                self.assertEqual((machine, magic), (I386, PE32), f'{dll.name} is tied to machine {machine:#x}: rebuild it AnyCPU')
                self.assertTrue(flags & ILONLY, f'{dll.name} is not IL only')
                self.assertFalse(flags & (REQUIRED32 | PREFERRED32), f'{dll.name} is 32-bit only or prefers 32-bit')

    def test_launcher_universal(self):
        self.assertEqual(macho_archs((BIN / 'dmsh_xlate').read_bytes()), ['arm64', 'x86_64'])

    def test_csproj_pins_anycpu(self):   # so a build for a runtime ID does not tie the next rebuild to it
        self.assertIn('<PlatformTarget>AnyCPU</PlatformTarget>', (REPO / 'dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj').read_text())


if __name__ == '__main__':
    unittest.main()
