"""An NCA's RomFS section: where it is, and the base section read on demand for a merge.

Both scripts of this folder that open a Program or data NCA use these (merge_nsp_romfs.py, extract_dlc.py). They were
imported from another port's scripts until 2026-11-25, when that port left the repository; the code is the same.
"""
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'pla/scripts'))
from extract_exefs import require  # noqa: E402


class CardBase:
    """The base Program NCA's RomFS section, decrypted from its container at the section's virtual offsets (what the
    update's indirect table addresses as storage 0: the section's hash levels and its RomFS data alike)."""

    def __init__(self, ctr, section_start, section_size, fs):
        require(fs[3] == 3 and fs[4] == 3, 'Expected a plain AES-CTR RomFS section in the base NCA')
        self.ctr, self.start, self.size, self.upper = ctr, section_start, section_size, fs[0x140:0x148][::-1]

    def read(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'Base section read out of bounds')
        return self.ctr.read(self.start + offset, size, self.upper)


def romfs_section(hdr):
    """(start, end, fs header) of the NCA's RomFS section (hash type 3), its FS header hash checked."""
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
        fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
        if start and fs[3] == 3:
            require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
            return start * 512, end * 512, fs
    raise ValueError('No RomFS section in the NCA')
