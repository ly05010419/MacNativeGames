"""Replace one member of a SARC archive and pack it again the way the game stores it (shared by make_button_icons.py and the
Breath of the Wild prompt tool). Every other member stays byte-identical, in the original order and alignment."""
import struct

from make_romfs_patches import sarc_members


def repack(sarc, name, blob):
    """The SARC with member `name` replaced by `blob`, every other member byte-identical, in the original order and alignment."""
    start = struct.unpack_from('<I', sarc, 12)[0]
    fat = 0x14; count = struct.unpack_from('<H', sarc, fat + 6)[0]
    nodes = [list(struct.unpack_from('<4I', sarc, fat + 12 + i * 16)) for i in range(count)]
    members = sarc_members(sarc); by_range = {(lo, hi): k for k, (lo, hi) in members.items()}
    blobs = [blob if by_range[(start + n[2], start + n[3])] == name else sarc[start + n[2]:start + n[3]] for n in nodes]

    def alignment(offset):
        a = 1
        while offset % (a * 2) == 0 and a < 0x2000:
            a *= 2
        return a
    data, placed = bytearray(), {}
    for i in sorted(range(count), key=lambda i: nodes[i][2]):
        a = alignment(nodes[i][2]) if nodes[i][2] else 1
        while len(data) % a:
            data += b'\0'
        placed[i] = (len(data), len(data) + len(blobs[i])); data += blobs[i]
    head = bytearray(sarc[:start])
    for i in range(count):
        struct.pack_into('<II', head, fat + 12 + i * 16 + 8, *placed[i])
    out = bytearray(bytes(head) + bytes(data)); struct.pack_into('<I', out, 8, len(out)); out = bytes(out)
    check = sarc_members(out)
    for k, (lo, hi) in members.items():
        assert out[check[k][0]:check[k][1]] == (blob if k == name else sarc[lo:hi]), k
    return out
