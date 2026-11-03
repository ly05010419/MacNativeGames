"""Disassemble an original NSO: nsodis.py <nso> <hex address> [before=40] [after=8]"""
import struct, sys
import capstone, lz4.block
def load(path):
    raw = open(path, 'rb').read(); flags = struct.unpack_from('<I', raw, 12)[0]; packed = struct.unpack_from('<III', raw, 0x60)
    segments = []
    for i in range(3):
        fo, addr, size, _ = struct.unpack_from('<IIII', raw, 0x10 + 16 * i); stored = packed[i] if flags & (1 << i) else size
        data = raw[fo:fo + stored]; segments.append((addr, lz4.block.decompress(data, uncompressed_size=size) if flags & (1 << i) else data))
    image = bytearray(max(a + len(d) for a, d in segments))
    for a, d in segments: image[a:a + len(d)] = d
    return image
if __name__ == '__main__':
    image = load(sys.argv[1]); a = int(sys.argv[2], 16)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 40; after = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for i in md.disasm(bytes(image[a - 4 * before:a + 4 * after]), a - 4 * before): print(f'{i.address:8x}: {i.bytes.hex()} {i.mnemonic} {i.op_str}')
