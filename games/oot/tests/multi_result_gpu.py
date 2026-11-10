#!/usr/bin/env python3
"""Independent SULD.RGBA fixtures: dead x/live w and dead w/live x.
Coordinates come from guest SSBO memory, not literals that constant folding can
hide. Only the multi-result image load uses them. Store the surviving component
at output pixel (0,0), so pruning the load's inputs changes an observable result.
"""
import argparse,json,struct,subprocess
from pathlib import Path

def ins(op,rd=0,ra=0,b=0,c=0):
    return struct.pack('<IIII',0x7000|op|(rd<<16)|(ra<<24),b,c,0x000fe200)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--work',type=Path,required=True)
    ap.add_argument('--service',type=Path,required=True);ap.add_argument('--spirv-cross',type=Path,required=True)
    a=ap.parse_args();a.work.mkdir(parents=True,exist_ok=True)
    p=subprocess.Popen([str(a.service),'--service','--spirv-cross',str(a.spirv_cross),'--out',str(a.work),'--chain','oot-live-image-input-regression'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    cases=[]
    for dead,live in [(8,11),(11,8)]:
        key=f'multi-result-dead{dead}-live{live}'
        code=ins(0xa02,0,b=0x310<<6,c=0xf00)+ins(0xa02,1,b=0x314<<6,c=0xf00)
        code+=ins(0x381,4,0,b=8<<8,c=0xb00) # dynamic uint2 coordinates
        code+=ins(0xb97,8,4,b=(2<<22)|((0x120//4)<<8)|(3<<29),c=0x001e0f00)
        code+=ins(0x810,16,dead,b=1,c=0x07ffe0ff) # unused IADD3 result
        code+=ins(0xb9b,ra=255,b=live|(2<<22)|((0x128//4)<<8)|(3<<29),c=0x00100100)
        code+=ins(0x94d,c=0x03800000)
        path=a.work/(key+'.code');path.write_bytes(code)
        request=dict(variant=key,stage='cs',arch='sm86',code=str(path),c1='',local=[1,1,1],depth_minus_w=False,img={'0':dict(type='2d',fmt='R32G32B32A32Float'),'1':dict(type='2d',fmt='R32Float')})
        p.stdin.write(json.dumps(request)+'\n');p.stdin.flush();reply=json.loads(p.stdout.readline())
        if not reply.get('ok'):raise RuntimeError(reply)
        for x,y in [(2,3),(3,1),(1,2),(0,0)]:
            cases.append(dict(source=str(a.work/(key+'_b.metal')),reflection=str(a.work/(key+'_b.json')),multi_result=True,coord=[x,y],component=live-8,expected=0x3f800000+4*(y*4+x)+live-8,label=f'{key} at {x},{y}'))
    p.stdin.close()
    if p.wait():raise RuntimeError('shader service failed')
    (a.work/'cases.json').write_text(json.dumps(cases,indent=2)+'\n')
    print(a.work/'cases.json')
if __name__=='__main__':main()
