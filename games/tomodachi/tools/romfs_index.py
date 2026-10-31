"""Index an already decrypted RomFS without extraction or cryptography dependencies.

Split from extract_romfs.py; upstream provenance is recorded in UPSTREAM.json.
"""
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_at(stream, offset, length):
    require(0 <= offset and 0 <= length <= 512 * 1024 * 1024, "Invalid/big RomFS read")
    stream.seek(offset)
    data = stream.read(length)
    require(len(data) == length, "Short RomFS read")
    return data


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
