import sys,struct,lz4.block
d=open(sys.argv[1],'rb').read()
flags=struct.unpack_from('<I',d,0xc)[0]
segs=[struct.unpack_from('<III',d,0x10+i*0x10) for i in range(3)]
bss=struct.unpack_from('<I',d,0x3c)[0]
csz=struct.unpack_from('<III',d,0x60)
end=segs[2][1]+segs[2][2]
img=bytearray(end+bss)
for i,(fo,mo,sz) in enumerate(segs):
    raw=d[fo:fo+csz[i]]
    if flags>>i&1: raw=lz4.block.decompress(raw,uncompressed_size=sz)
    img[mo:mo+sz]=raw
open(sys.argv[2],'wb').write(img)
print([hex(x) for s in segs for x in s],hex(bss),'moduleid',d[0x40:0x60].hex())
