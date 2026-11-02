#!/usr/bin/env python3
"""Verify all six IVFC levels, stream a local RomFS image, and index its files."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import modes
if __package__:
    from .extract_exefs import (decrypt, read_at, require, load_keys, container_entries,
                                common_tickets, nca_inventory, program_nca, nca_ctr_key, record_offset)
else:
    from extract_exefs import (decrypt, read_at, require, load_keys, container_entries,
                               common_tickets, nca_inventory, program_nca, nca_ctr_key, record_offset)

def main(default_manifest=None, default_output=None, default_title_id=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keys',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,default=default_manifest)
    parser.add_argument('--source',type=Path,help='DLC NSP with a PublicData NCA; requires --title-id')
    parser.add_argument('--title-id',type=lambda value:value.upper(),help='expected DLC title ID with --source')
    parser.add_argument('--output',type=Path,required=default_output is None,default=default_output)
    args=parser.parse_args(); require(not args.output.exists(),'Output already exists')
    require(bool(args.source)==bool(args.title_id),'--source and --title-id must be used together')
    require(args.source or args.manifest is not None,'Provide --manifest for base RomFS')
    if args.title_id:
        require(len(args.title_id)==16 and all(c in '0123456789ABCDEF' for c in args.title_id),
                'Expected a 16-digit uppercase title ID')
    manifest=None if args.source else json.loads(args.manifest.read_text())
    keys=load_keys(args.keys)
    source=args.source if args.source else Path(manifest['source'])
    if manifest:require(source.stat().st_size==manifest['source_size'],'Source size changed')
    with source.open('rb') as f:
        kind,entries=container_entries(f,source.stat().st_size)
        records=nca_inventory(f,entries,keys)
        if args.source:
            require(kind=='nsp','Expected a DLC NSP')
            require(all(r['title_id']==args.title_id for r,_ in records),'Mixed-title DLC package')
            selected=[(r,h) for r,h in records if r['content_type']==5 and r['title_id']==args.title_id]
            require(len(selected)==1,'Expected one PublicData NCA for DLC title')
            actual,hdr=selected[0]
            manifest=dict(source=str(source.resolve()),source_size=source.stat().st_size,
                          container=kind,title_id=args.title_id,program=actual)
        else:
            require(kind==manifest['container'],'Source container type changed')
            title_id=manifest.get('title_id',default_title_id)
            require(title_id is not None,'Missing title ID in manifest')
            actual,hdr=program_nca(records,
                                   title_id=title_id,
                                   allow_combined=kind=='xci')
            record=manifest['program']
            require(actual['name']==record['name'] and actual['size']==record['size'] and
                    record_offset(actual)==record_offset(record),'Source Program NCA changed; extract ExeFS again')
        record=manifest['program']
        ctr_key=nca_ctr_key(hdr,keys,common_tickets(f,entries))
        for i in range(4):
            start,end=struct.unpack_from('<II',hdr,0x240+i*16)
            if not start:continue
            fs=hdr[0x400+512*i:0x600+512*i]
            require(hashlib.sha256(fs).digest()==hdr[0x280+32*i:0x2a0+32*i],'FS header hash')
            if fs[2:5]!=bytes([0,3,3]):continue
            require(fs[8:12]==b'IVFC' and struct.unpack_from('<III',fs,12)==(0x20000,32,7),'Unsupported IVFC layout')
            levels=[struct.unpack_from('<QQII',fs,0x18+j*24) for j in range(6)]
            useful=[0]*6
            useful[-1]=levels[-1][1]
            for level in range(4,-1,-1):
                child_block=1<<levels[level+1][2]
                useful[level]=((useful[level+1]+child_block-1)//child_block)*32
                require(useful[level]<=levels[level][1],'IVFC hash storage too small')
            def read(relative,size):
                require(0<=relative<=relative+size<=(end-start)*512,'Section bounds')
                absolute=start*512+relative;aligned=absolute&~15;skip=absolute-aligned
                iv=fs[0x140:0x148][::-1]+(aligned//16).to_bytes(8,'big')
                return decrypt(ctr_key,modes.CTR(iv),read_at(f,record_offset(record)+aligned,size+skip))[skip:]
            args.output.mkdir(parents=True);image_path=args.output/'romfs.bin';digest=hashlib.sha256();counts=[]
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
                            expected=parent[index*32:index*32+32]
                            if index*block<useful[level]:
                                require(hashlib.sha256(payload+bytes(block-len(payload))).digest()==expected,
                                        f'IVFC level {level} block {index} failed')
                            else:
                                require(expected==bytes(32) and not any(payload),
                                        f'IVFC level {level} unused block {index} is not zero')
                            index+=1
                        if captured is not None:captured.extend(chunk)
                        else:image.write(chunk);digest.update(chunk)
                    counts.append(index)
                    if captured is not None:parent=bytes(captured)
                    print(f'IVFC level {level}: {index} blocks verified',flush=True)
            files=index_romfs(image_path)
            report=dict(status='pass',source_size=source.stat().st_size,program=record['name'],section=i,ivfc_blocks=counts,romfs_size=levels[-1][1],romfs_sha256=digest.hexdigest(),files=files,integrity='FS header and all six IVFC levels verified; RSA signature not checked')
            (args.output/'index.json').write_text(json.dumps(report,indent=2)+'\n')
            if args.source:(args.output/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            print(json.dumps(dict(status='pass',file_count=len(files),size=levels[-1][1],sha256=digest.hexdigest())),flush=True);return
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
