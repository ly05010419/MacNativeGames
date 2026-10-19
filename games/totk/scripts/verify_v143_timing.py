#!/usr/bin/env python3
"""Execute production v143 timing patches, including their real emitted veneer."""
import hashlib,json,os,struct,subprocess,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,
 UC_ARM64_REG_X8,UC_ARM64_REG_X9,UC_ARM64_REG_X19,UC_ARM64_REG_X22,
 UC_ARM64_REG_S0,UC_ARM64_REG_S8,UC_ARM64_REG_S9)
ROOT=Path(__file__).resolve().parents[1]
def main():
 os.chdir(ROOT);out=ROOT/'analysis/v143-port';out.mkdir(exist_ok=True)
 expected=json.loads((ROOT/'assets/expected-v1.4.3.json').read_text())
 source=ROOT/'rewritten/v143/main.patched.bin';raw=source.read_bytes()
 assert hashlib.sha256(raw).hexdigest()==expected['rewritten_sha256']['main.patched.bin']
 layout=json.loads((out/'runtime-layout.json').read_text())[0]['layout']
 subprocess.run(['xcrun','clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Iruntime','scripts/v143_timing_fixture.c','runtime/game_patches_v143.c','-o','build/v143-timing-fixture'],check=True)
 target=out/'main-timing-fixture.bin'
 run=subprocess.run(['build/v143-timing-fixture',str(source),str(target),str(layout['TEXT_END'])],capture_output=True,text=True,check=True,env=os.environ|{'TOTK_REALTIME':'1','TOTK_FPS60':'1','TOTK_OUTPUT_RES':'0'})
 (out/'timing-fixture.log').write_text(run.stdout+run.stderr);samples=json.loads(run.stdout)
 for name,seconds in [('pin',0.01),('sample',0.025)]:
  assert all(abs(x-y)<1e-6 for x,y in zip(samples[name],[seconds*30,seconds,seconds*60]))
 # Same placement as game_patches_v143.c: the producer veneer (4 words) sits in
 # the zero padding after the text segment, so iOS can embed it in the signed RX
 # segment. It must end before read-only data starts, which is stricter than the
 # runtime's 16 KiB page bound.
 patched=target.read_bytes();data=(len(raw)+0x3fff)&~0x3fff;stub=(layout['TEXT_END']+15)&~15
 assert stub+16<=layout['RO_OFF'],'frame-step veneer would overlap read-only data'
 assert raw[stub:stub+16]==bytes(16),'frame-step veneer padding not empty in the rewritten image'
 changes=[o for o in range(0,len(raw),4) if raw[o:o+4]!=patched[o:o+4]]
 allowed={0x2db204,0x2db208,0x2db22c,0x2db360,0x2db36c,0x2db340,0x2db34c,0x3e5658,0x2daa54,0x2daa58,0x2daa6c,0x2daa70,0x2daa74,0x2daa78}|{stub+4*i for i in range(4)}
 assert set(changes)==allowed,[hex(o) for o in sorted(set(changes)^allowed)]
 def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]
 def execute(seconds,apply,half=False,call=0x3e5720,nominal_fps=60):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(patched));mu.mem_write(0,patched if apply else raw)
  mu.mem_map(0x10000000,0x20000);mu.mem_write(data,struct.pack('<fff',seconds*30,seconds,seconds*60))
  mu.reg_write(UC_ARM64_REG_SP,0x10013000)
  # Execute the actual selector and producer, not a synthesized callee input.
  # x8 points to the FrameRateMgr singleton, x9 is a LIVE task flag after this
  # producer. A veneer that uses x9 as scratch would fail this check.
  mu.reg_write(UC_ARM64_REG_X8,0x1001f000);mu.mem_write(0x1001f000,struct.pack('<Q',0x10000000))
  mu.reg_write(UC_ARM64_REG_X19,0x10003000);mu.mem_write(0x100034f8,struct.pack('<I',0x13572468))
  mu.reg_write(UC_ARM64_REG_X22,0x10002000);mu.mem_write(0x10002250,struct.pack('<I',nominal_fps))
  mu.mem_write(0x100021da,bytes([int(half)]));mu.reg_write(UC_ARM64_REG_S9,bits(1.0))
  mu.emu_start(0x3e562c,0x3e565c,count=100);assert mu.reg_read(UC_ARM64_REG_PC)==0x3e565c
  assert mu.reg_read(UC_ARM64_REG_X9)==0x13572468,'live producer flags clobbered'
  interval=struct.unpack('<f',struct.pack('<I',mu.reg_read(UC_ARM64_REG_S8)))[0]
  want_interval=(seconds*60 if apply else 60/nominal_fps)*(.5 if half else 1)
  assert abs(interval-want_interval)<1e-6,(interval,want_interval)
  # Both original call paths copy s8 and call the untouched FrameRateMgr.
  mu.emu_start(call,0x3e684c,count=100);assert mu.reg_read(UC_ARM64_REG_PC)==0x3e684c
  ratio,inverse,dt,fps=struct.unpack('<fffi',mu.mem_read(0x10000010,16))
  mu.reg_write(UC_ARM64_REG_X19,0x10000000);mu.mem_write(0x10000250,struct.pack('<I',60))
  mu.emu_start(0x2daa54,0x2daa80,count=40);assert mu.reg_read(UC_ARM64_REG_PC)==0x2daa80
  global_ratio=struct.unpack('<f',mu.mem_read(0x3a22128,4))[0]
  return ratio,inverse,dt,fps,global_ratio
 cases=[]
 for hz in (15,20,25,30,50,60,90,100,120):
  for half in (False,True):
   scale=.5 if half else 1
   for call in (0x3e5720,0x3e583c):
    for nominal in (30,60,100):
     result=execute(1/hz,True,half,call,nominal);want=(30/hz*scale,hz/30/scale,scale/hz,int(hz/scale),30/hz)
     assert all(abs(x-y)<1e-5 for x,y in zip(result,want)),(hz,half,call,nominal,result,want)
     cases.append(dict(hz=hz,half_step=half,call=hex(call),nominal_fps=nominal,ratio=result[0],inverse=result[1],seconds=result[2],rounded_fps=result[3],global_ratio=result[4]))
 negative=[execute(1/50,False,half) for half in (False,True)]
 assert abs(negative[0][2]-1/60)<1e-6 and abs(negative[1][2]-1/120)<1e-6
 assert abs(negative[0][2]-1/50)>0.001 and abs(negative[1][2]-1/100)>0.001
 report=dict(status='pass',version='v143',scope='production C patch emission, original half-step selector, both original callers and untouched ratio/inverse/seconds/FPS calculation; live flags, independent global ratio writer, pin and measured-step sampling',cases=cases,negative_control=negative,veneer_offset=hex(stub),changed_original_words=len(changes),sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in ('runtime/game_patches_v143.c','scripts/v143_timing_fixture.c','scripts/verify_v143_timing.py')})
 (out/'realtime-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(status='pass',cases=len(cases),negative_control=negative,changed_original_words=len(changes))))
if __name__=='__main__':main()
