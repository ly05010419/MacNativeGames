#!/usr/bin/env python3
"""Original SASS: load a pointer from an SSBO, dereference it, store its word.
The host fixture creates disjoint pools across the former 4095-entry limit.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

def inst(op, rd=0, ra=0, b=0, c=0):
    return struct.pack('<IIII', op | 7 << 12 | rd << 16 | ra << 24, b, c, 0x000fe200)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--work',type=Path,required=True)
    ap.add_argument('--service',type=Path,required=True);ap.add_argument('--spirv-cross',type=Path,required=True)
    a=ap.parse_args();a.work.mkdir(parents=True,exist_ok=True)
    cases=[]
    for operation in ['load','store']:
        code=inst(0xa02,0,b=0x310<<6,c=0xf00)+inst(0xa02,1,b=0x314<<6,c=0xf00)
        code+=inst(0x381,2,0,b=8<<8,c=0xb00)  # LDG.E.64 R2,[R0+8]
        if operation=='store':
            code+=inst(0x381,4,0,b=16<<8,c=0x900)  # source word supplied by host
            code+=inst(0x386,ra=2,b=4,c=0x900)     # STG.E [R2],R4
        code+=inst(0x381,4,2,c=0x900)             # LDG.E R4,[R2]
        code+=inst(0x386,ra=0,b=4,c=0x900)+inst(0x94d,c=0x03800000)
        variant='raw-pointer-'+operation
        path=a.work/(variant+'.code');path.write_bytes(code)
        rq=dict(variant=variant,stage='cs',arch='sm86',code=str(path),c1='',local=[1,1,1],depth_minus_w=False)
        p=subprocess.run([str(a.service),'--service','--spirv-cross',str(a.spirv_cross),'--out',str(a.work),'--chain','oot-raw-pool-regression'],input=json.dumps(rq)+'\n',text=True,stdout=subprocess.PIPE,check=True)
        reply=json.loads(p.stdout.splitlines()[0]);assert reply['ok'],reply
        rf=json.loads((a.work/(variant+'_b.json')).read_text())
        assert any(b['kind']=='gmem_map' or (b['kind']=='sbuf' and b.get('reg') is None and not b.get('rw')) for b in rf['bindings']),rf
        for idx in [8192,0,4093,4094,4095,4096,8191]:
            for offset in [0,128,4092,4096,8192,16380]:
                unmapped=offset>=4096
                cases.append(dict(source=str(a.work/(variant+'_b.metal')),reflection=str(a.work/(variant+'_b.json')),raw_pool_count=8193,raw_pool_index=idx,raw_offset=offset,raw_store=operation=='store',raw_unmapped=unmapped,expected=0 if unmapped else 0x12345678,label=f'raw {operation} pool={idx} offset={offset}'))
    (a.work/'cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    print(a.work/'cases.json')
if __name__=='__main__':main()
