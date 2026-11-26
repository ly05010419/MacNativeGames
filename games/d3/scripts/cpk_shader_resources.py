#!/usr/bin/env python3
"""Bounded D3 Switch base CPK v7 reader (not CRI CPK).

Only flags=2 base archives are supported. Patch flags=10 are deliberately
rejected until their separate offset/layout rules have been verified.
"""
import bisect
import struct
import zlib

MAX_METADATA = 128 << 20
MAX_RESOURCE = 64 << 20
PAGE = 16384


def bits(data, at, count):
    if at < 0 or not 0 < count <= 64 or at+count > len(data)*8:
        raise ValueError('packed table outside metadata')
    lo, shift = divmod(at, 8)
    size = (shift+count+7)//8
    return (int.from_bytes(data[lo:lo+size], 'big') >> (size*8-shift-count)) & ((1<<count)-1)


class BaseArchive:
    def __init__(self, read, size):
        self.read, self.size = read, size
        header = read(0, 72)
        v = struct.unpack_from('<16I', header)
        if v[:2] != (0xa1b2c3d4, 7) or v[4] != 2 or v[15] != 65536 or struct.unpack_from('<Q', header, 64)[0] != PAGE:
            raise ValueError('unsupported CPK header/layout (patch archives require a separate reader)')
        self.logical = struct.unpack_from('<Q', header, 8)[0]
        count, aux, self.start = v[5], v[6], v[7]*65536
        if not 0 < count == aux <= 2000000 or not 72 <= self.start <= min(size, MAX_METADATA):
            raise ValueError('invalid CPK metadata size/count')
        widths = v[8:11]
        if not all(0 < x <= 64 for x in (*widths, v[11], v[12], v[13])):
            raise ValueError('invalid packed field widths')
        data = read(0, self.start)
        if len(data) != self.start:
            raise ValueError('short CPK metadata')
        record_width = 64+sum(widths)
        table2 = 72+(record_width*count+7)//8
        table3 = table2+(aux*v[11]+7)//8
        pages = (size-self.start+PAGE-1)//PAGE
        table4 = table3+(pages*v[12]+7)//8
        logical_pages = (self.logical+PAGE-1)//PAGE
        names = table4+(logical_pages*v[13]+7)//8
        base = names+4*count
        if base > len(data): raise ValueError('CPK names outside metadata')
        self.starts = [bits(data,table3*8+i*v[12],v[12]) for i in range(pages)]
        if not self.starts or self.starts[0] != 0 or any(a >= b for a,b in zip(self.starts,self.starts[1:])) or self.starts[-1] >= self.logical:
            raise ValueError('invalid logical page offsets')
        self.entries = {}
        for i in range(count):
            off = struct.unpack_from('<I',data,names+4*i)[0]
            if base+off >= len(data): raise ValueError('filename outside metadata')
            end = data.find(b'\0',base+off)
            if end < 0: raise ValueError('unterminated filename')
            name = data[base+off:end].decode('utf-8')
            at = 72*8+i*record_width+64
            length = bits(data,at,widths[0]);at+=widths[0]
            storage = bits(data,at,widths[1]);at+=widths[1]
            idx = bits(data,at,widths[2])
            if storage != 1 or idx >= aux: raise ValueError('unsupported file storage')
            offset = bits(data,table2*8+idx*v[11],v[11])
            if offset+length > self.logical or name in self.entries:
                raise ValueError('invalid or duplicate file entry')
            self.entries[name] = (offset,length)

    def page(self, index):
        raw = self.read(self.start+index*PAGE, min(PAGE,self.size-self.start-index*PAGE))
        expected = (self.starts[index+1] if index+1<len(self.starts) else self.logical)-self.starts[index]
        if expected > MAX_RESOURCE: raise ValueError('unbounded logical page')
        result, at = bytearray(), 0
        while at+6 <= len(raw):
            length, flags, packed = struct.unpack_from('<HHH',raw,at)
            if not packed: break
            if flags or not length or at+6+packed>len(raw): raise ValueError('unsupported compressed block')
            d = zlib.decompressobj()
            decoded = d.decompress(raw[at+6:at+6+packed], length+1)
            if len(decoded)!=length or not d.eof or d.unused_data or d.unconsumed_tail:
                raise ValueError('invalid zlib block')
            result.extend(decoded);at+=6+packed
            if len(result)>expected: raise ValueError('logical page overflow')
        if len(result)!=expected: raise ValueError('logical page length mismatch')
        return bytes(result)

    def extract(self, name):
        offset, length = self.entries[name]
        if not 0 < length <= MAX_RESOURCE: raise ValueError('unbounded resource')
        index = bisect.bisect_right(self.starts,offset)-1
        result = bytearray()
        while len(result)<length:
            data = self.page(index)
            skip = max(0,offset-self.starts[index])
            result.extend(data[skip:skip+length-len(result)])
            index+=1
        return bytes(result)
