#!/usr/bin/env python3
"""Verify all six IVFC levels, stream a local RomFS image, and index its files."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import modes
from extract_exefs import (decrypt, read_at, require, load_keys, container_entries, common_tickets,
                           nca_inventory, program_nca, nca_ctr_key, record_offset)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keys',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,default=Path('extracted/v0/extraction-manifest.json'))
    parser.add_argument('--output',type=Path,default=Path('extracted/v0/romfs'))
    args=parser.parse_args(); require(not args.output.exists(),'Output already exists')
    manifest=json.loads(args.manifest.read_text());record=manifest['program'];keys=load_keys(args.keys)
    source=Path(manifest['source']);require(source.stat().st_size==manifest['source_size'],'Source size changed')
    with source.open('rb') as f:
        _,entries=container_entries(f,source.stat().st_size)
        actual,hdr=program_nca(nca_inventory(f,entries,keys))
        require(actual['name']==record['name'] and actual['size']==record['size'] and
                record_offset(actual)==record_offset(record),'Source Program NCA changed; extract ExeFS again')
        ctr_key=nca_ctr_key(hdr,keys,common_tickets(f,entries))
        result=verify_ivfc(f,record_offset(record),hdr,ctr_key,args.output/'romfs.bin')
    files=index_romfs(args.output/'romfs.bin')
    report=dict(status='pass',source_size=source.stat().st_size,program=record['name'],section=result['section'],ivfc_blocks=result['ivfc_blocks'],romfs_size=result['romfs_size'],romfs_sha256=result['romfs_sha256'],files=files,integrity='FS header and all six IVFC levels verified; RSA signature not checked')
    (args.output/'index.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(status='pass',file_count=len(files),size=result['romfs_size'],sha256=result['romfs_sha256'])),flush=True)

def verify_ivfc(f,nca_offset,hdr,ctr_key,image_path):
    """Check the FS header hashes and all six IVFC levels of the first RomFS section of the NCA at nca_offset in f,
    and write its last level (the RomFS image) to image_path. Returns dict(section, ivfc_blocks, romfs_size, romfs_sha256)."""
    for i in range(4):
        start,end=struct.unpack_from('<II',hdr,0x240+i*16)
        if not start:continue
        fs=hdr[0x400+512*i:0x600+512*i]
        require(hashlib.sha256(fs).digest()==hdr[0x280+32*i:0x2a0+32*i],'FS header hash')
        if fs[2:5]!=bytes([0,3,3]):continue
        require(fs[8:12]==b'IVFC' and struct.unpack_from('<III',fs,12)==(0x20000,32,7),'Unsupported IVFC layout')
        levels=[struct.unpack_from('<QQII',fs,0x18+j*24) for j in range(6)]
        def read(relative,size):
            require(0<=relative<=relative+size<=(end-start)*512,'Section bounds')
            absolute=start*512+relative;aligned=absolute&~15;skip=absolute-aligned
            iv=fs[0x140:0x148][::-1]+(aligned//16).to_bytes(8,'big')
            return decrypt(ctr_key,modes.CTR(iv),read_at(f,nca_offset+aligned,size+skip))[skip:]
        image_path.parent.mkdir(parents=True);digest=hashlib.sha256();counts=[]
        parent=fs[0xc8:0xe8]
        with image_path.open('wb') as image:
            for level,(offset,size,power,reserved) in enumerate(levels):
                require(12<=power<=24 and reserved==0,'IVFC block size');block=1<<power
                require(len(parent)>=(size+block-1)//block*32,'IVFC parent size')
                captured=bytearray() if level<5 else None
                index=0
                for pos in range(0,size,8*1024*1024):
                    chunk=read(offset+pos,min(8*1024*1024,size-pos))
                    for j in range(0,len(chunk),block):
                        payload=chunk[j:j+block]
                        require(hashlib.sha256(payload+bytes(block-len(payload))).digest()==parent[index*32:index*32+32],f'IVFC level {level} block {index} failed')
                        index+=1
                    if captured is not None:captured.extend(chunk)
                    else:image.write(chunk);digest.update(chunk)
                counts.append(index)
                if captured is not None:parent=bytes(captured)
                print(f'IVFC level {level}: {index} blocks verified',flush=True)
        return dict(section=i,ivfc_blocks=counts,romfs_size=levels[-1][1],romfs_sha256=digest.hexdigest())
    raise ValueError('No supported RomFS found')

def index_romfs(path):
    with path.open('rb') as f:
        h=struct.unpack('<10Q',read_at(f,0,80));require(h[0]==80,'RomFS header');length=path.stat().st_size
        for off,size in [(h[1],h[2]),(h[3],h[4]),(h[5],h[6]),(h[7],h[8])]:require(off+size<=length,'RomFS table bounds')
        dirs=read_at(f,h[3],h[4]);entries=read_at(f,h[7],h[8]);files=[];seen_dirs=set();seen_files=set();paths=set()
        def name(table,off,n):
            require(off+n<=len(table),'Name bounds');s=table[off:off+n].decode('utf-8');require(s not in ['.','..'] and '/' not in s and '\\' not in s and '\0' not in s,'Unsafe path');return s
        todo=[(0,'')]
        while todo:
            off,parent=todo.pop();require(off not in seen_dirs and off+24<=len(dirs),'Invalid/cyclic directory');seen_dirs.add(off)
            p,sibling,child,file,hash_next,n=struct.unpack_from('<6I',dirs,off);part=name(dirs,off+24,n);prefix=parent+part+'/' if part else parent
            if sibling!=0xffffffff:todo.append((sibling,parent))
            if child!=0xffffffff:todo.append((child,prefix))
            while file!=0xffffffff:
                require(file not in seen_files and file+32<=len(entries),'Invalid/cyclic file');seen_files.add(file)
                p,next_file,data,size,hash_next,n=struct.unpack_from('<IIQQII',entries,file);part=name(entries,file+32,n);require(part!='','Empty file name');filename=prefix+part
                require(filename not in paths and h[9]+data+size<=length,'Duplicate/out-of-bounds file');paths.add(filename)
                files.append(dict(path=filename,offset=h[9]+data,size=size));file=next_file
        return sorted(files,key=lambda item:item['path'])
if __name__=='__main__':main()
