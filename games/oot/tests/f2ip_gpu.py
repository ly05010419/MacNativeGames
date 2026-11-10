#!/usr/bin/env python3
"""Independent FP32 conversion/packing oracle against native GPU shader execution.

Encoding example: NVIDIA Developer Forums /255571/4; packing order and saturation:
https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt-pack
All machine code fixtures and generated shader output remain in --work.
"""
import argparse,json,math,os,struct,subprocess
from pathlib import Path

def instruction(op,rd=0,ra=0,b=0,c=0):return struct.pack('<IIII',0x7000|op|rd<<16|ra<<24,b,c,0x000fe200)
def float_word(value):return struct.unpack('<I',struct.pack('<f',value))[0]
def mov(r,value):return instruction(0x802,r,b=value,c=0xf00)
def convert(value,signed,rounding):
    low,high=(-128,127)if signed else(0,255)
    if math.isnan(value):return 0
    if math.isinf(value):return (high if value>0 else low)&255
    value=(round,math.floor,math.ceil,math.trunc)[rounding](value)
    return max(low,min(high,value))&255

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--work',type=Path,required=True);ap.add_argument('--service',type=Path,required=True);ap.add_argument('--spirv-cross',type=Path,required=True);ap.add_argument('--runtime',type=Path,required=True);ap.add_argument('--modules',type=Path,required=True);ap.add_argument('--game-data',type=Path,required=True);a=ap.parse_args()
    root=Path(__file__).resolve().parents[3];work=a.work.resolve()
    if work==root or root in work.parents:raise ValueError('fixtures must remain outside the repository')
    work.mkdir(parents=True,exist_ok=True);cases=[]
    process=subprocess.Popen([str(a.service),'--service','--spirv-cross',str(a.spirv_cross),'--out',str(work),'--chain','oot-f2ip-regression'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    values=[-129.0,-128.5,-0.5,0.5,1.5,12.5,254.5,255.5,256.0,float('inf'),-float('inf'),float('nan')]
    for signed in (False,True):
      for rounding in range(4):
       for i,first in enumerate(values):
        second=values[-i-1]
        for constant in (False,True):
         key=f'pack-{len(cases):03d}';rd=4+len(cases)%4;merge=0x1234abcd
         code=instruction(0xa02,0,b=0x310<<6,c=0xf00)+instruction(0xa02,1,b=0x314<<6,c=0xf00)
         code+=mov(4,float_word(first))+mov(5,float_word(second))+mov(6,merge)
         mode=0x400|(int(signed)<<12)|(rounding<<14)|6
         code+=instruction(0xa43 if constant else 0x243,rd,4,b=(1<<22)if constant else 5,c=mode)
         code+=instruction(0x386,ra=0,b=rd,c=0x900)+instruction(0x94d,c=0x03800000)
         path=work/(key+'.code');path.write_bytes(code)
         rq=dict(variant=key,stage='cs',arch='sm86',code=str(path),c1=struct.pack('<f',second).hex()if constant else'',local=[1,1,1],depth_minus_w=False)
         process.stdin.write(json.dumps(rq)+'\n');process.stdin.flush();reply=json.loads(process.stdout.readline())
         if not reply['ok']:raise RuntimeError(reply)
         expected=(merge<<16&0xffffffff)|(convert(first,signed,rounding)<<8)|convert(second,signed,rounding)
         cases.append(dict(source=str(work/(key+'_b.metal')),reflection=str(work/(key+'_b.json')),expected=expected,label=f'{first}/{second} signed={signed} rounding={rounding} constant={constant} rd={rd}'))
    process.stdin.close()
    if process.wait():raise RuntimeError('shader service exited unsuccessfully')
    manifest=work/'cases.json';manifest.write_text(json.dumps(cases)+'\n')
    env=dict(os.environ,OOT_F2IP_CASES=str(manifest),TOTK_GAME_DATA=str(a.game_data),TOTK_SLOT_OWNER='codex',TOTK_SLOT_PURPOSE='oot-f2ip-gpu-regression',DM_PSO_MANIFEST='off',DM_PSO_USER='/dev/null',MTL_DEBUG_LAYER='1')
    subprocess.run([str(a.runtime),str(a.modules),'probe-gpu-f2ip'],cwd=work,env=env,check=True)
if __name__=='__main__':main()
