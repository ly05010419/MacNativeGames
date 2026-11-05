#!/usr/bin/env python3
"""Generate independent Ampere SURED fixtures; all outputs stay in --work.

Semantics: NVIDIA PTX sured; scope fields: official Mesa NAK SM80 encoder.
The CPU oracle applies each integer reduction independently of the translator.
"""
import argparse,json,struct,subprocess
from pathlib import Path
from f2ip_gpu import instruction,mov

def signed(value):return value if value<0x80000000 else value-0x100000000
def reduce_value(old,value,operation,is_signed):
    if operation==0:return (old+value)&0xffffffff
    if operation in (1,2):
        a,b=(signed(old),signed(value))if is_signed else(old,value)
        return (min(a,b)if operation==1 else max(a,b))&0xffffffff
    if operation==3:return 0 if old>=value else old+1
    if operation==4:return value if old==0 or old>value else old-1
    return (old&value,old|value,old^value)[operation-5]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--work',type=Path,required=True);ap.add_argument('--service',type=Path,required=True);ap.add_argument('--spirv-cross',type=Path,required=True);a=ap.parse_args()
    work=a.work.resolve();repo=Path(__file__).resolve().parents[3]
    if work==repo or repo in work.parents:raise ValueError('fixtures must remain outside the source tree')
    work.mkdir(parents=True,exist_ok=True)
    p=subprocess.Popen([str(a.service),'--service','--spirv-cross',str(a.spirv_cross),'--out',str(work),'--chain','oot-sured-regression'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    cases=[];rejected=0
    base=[(False,0,1,7),(False,1,0xfffffffe,7),(False,2,0xfffffffe,7),(False,3,7,2),(False,4,7,2),(False,5,0xfffffff0,0xffffffff),(False,6,None,0),(False,7,None,0),(True,1,0xfffffffe,7),(True,2,0xfffffffe,7)]
    configs=[(s,o,v,i,order,byte,2,3)for order in(0,5,7)for byte in(False,True)for s,o,v,i in base]
    configs += [(False,6,None,0x12345678,7,byte,x,y)for byte in(False,True)for x,y in((-1,3),(4,3),(2,4))]
    for is_signed,operation,value,initial,order,byte,x,y in configs:
        key=f'sured-{len(cases)+rejected:03d}';coords_x=x*4 if byte else x
        code=mov(4,coords_x&0xffffffff)+mov(5,y&0xffffffff)
        if value is None:
            code+=mov(7,1)+instruction(0x919,6,c=33<<8)+instruction(0x219,0,7,b=6,c=0xff|(3<<9))
        else:code+=mov(0,value)
        # A c[2] image handle at +0x120 names image slot zero. The same
        # descriptor layout is used by NVN2 after its c[0] table remapping.
        b=(2<<22)|(0x48<<8)|(3<<29)
        c=(int(byte)<<8)|(int(is_signed)<<9)|(order<<13)|(1<<20)|(operation<<23)
        code+=instruction(0x59f,ra=4,b=b,c=c)+instruction(0x94d,c=0x03800000)
        path=work/(key+'.code');path.write_bytes(code)
        rq=dict(variant=key,stage='cs',code=str(path),arch='sm86',c1='',local=[32,1,1],depth_minus_w=True,img={'0':dict(type='2d',fmt='R32Sint'if is_signed else'R32Uint')})
        p.stdin.write(json.dumps(rq)+'\n');p.stdin.flush();reply=json.loads(p.stdout.readline())
        if operation in (3,4):
            if reply['ok']:raise RuntimeError('unverified increment/decrement mode was silently accepted')
            rejected+=1;continue
        if not reply['ok']:raise RuntimeError((key,reply))
        expected=initial
        if 0<=x<4 and 0<=y<4:
            for lane in range(32):expected=reduce_value(expected,(1<<lane)if value is None else value,operation,is_signed)
            target=y*4+x
        else:target=-1
        cases.append(dict(source=str(work/(key+'_b.metal')),reflection=str(work/(key+'_b.json')),initial=initial,expected=expected,target=target,signed=is_signed,label=f'op={operation} signed={is_signed} scope={order} bytes={byte} xy={x},{y}'))
    p.stdin.close()
    if p.wait():raise RuntimeError('shader service exited unsuccessfully')
    (work/'cases.json').write_text(json.dumps(cases)+'\n');print('surface reduction GPU fixtures',len(cases),'unsupported modes explicitly rejected',rejected,flush=True)
if __name__=='__main__':main()
