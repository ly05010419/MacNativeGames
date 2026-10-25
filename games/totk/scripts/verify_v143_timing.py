#!/usr/bin/env python3
"""Execute production v143 timing patches, including their real emitted veneer."""
import hashlib,json,os,struct,subprocess,sys
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,
 UC_ARM64_REG_X8,UC_ARM64_REG_X9,UC_ARM64_REG_X19,UC_ARM64_REG_X22,
 UC_ARM64_REG_S0,UC_ARM64_REG_S1,UC_ARM64_REG_S2,UC_ARM64_REG_S3,
 UC_ARM64_REG_S8,UC_ARM64_REG_S9,UC_ARM64_REG_S10,UC_ARM64_REG_LR,
 UC_ARM64_REG_NZCV,UC_ARM64_REG_X25)
ROOT=Path(__file__).resolve().parents[1]
def main():
 os.chdir(ROOT);out=ROOT/'analysis/v143-port';out.mkdir(exist_ok=True)
 expected=json.loads((ROOT/'assets/expected-v1.4.3.json').read_text())
 source=ROOT/'rewritten/v143/main.patched.bin';raw=source.read_bytes()
 assert hashlib.sha256(raw).hexdigest()==expected['rewritten_sha256']['main.patched.bin']
 layout=json.loads((out/'runtime-layout.json').read_text())[0]['layout']
 subprocess.run(['xcrun','clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Iruntime','scripts/v143_timing_fixture.c','runtime/game_patches_v143.c','-o','build/v143-timing-fixture'],check=True)
 target=out/'main-timing-fixture.bin'
 # The render-resolution patch is on by default on the Mac (e8edbcd: the scene at the canvas size); the fixture runs it as
 # shipped, so a TOTK_RENDER_RES left in the caller's environment is dropped.
 fixture_env={k:v for k,v in os.environ.items() if k!='TOTK_RENDER_RES'}|{'TOTK_REALTIME':'1','TOTK_FPS60':'1','TOTK_OUTPUT_RES':'0','TOTK_ASPECT':'0','TOTK_CLOTH_REALTIME':'1','TOTK_HIGHFPS_FIX':'1'}
 # Real-time frame step (runtime_frame_step_update): steady 120, 144, 160 and 240 fps, and short and long presents
 # alternating (around the 120 cap, and above it), must give game time = real time. A 1/120 s floor per interval made
 # 144 fps run 1.2x and 240 fps 2x fast, and alternating 1/240 + 1/80 s 1.33x fast. A hitch is cut at 1/15 s per
 # interval (as in 1.0-1.2.1): a 200 ms present among 120 fps ones, and steady 10 fps, count 1/15 s each.
 patterns=[(1/120,),(1/144,),(1/160,),(1/240,),(1/240,1/80),(.004,.0127),(1/288,1/96),(.003,.0045,.006),(1/240,1/240,1/60),
           (1/120,)*15+(.2,),(.1,)]
 run=subprocess.run(['build/v143-timing-fixture',str(source),str(target),str(layout['TEXT_END'])]+[','.join(str(round(x*1e9)) for x in p) for p in patterns],capture_output=True,text=True,check=True,env=fixture_env)
 (out/'timing-fixture.log').write_text(run.stdout+run.stderr);samples=json.loads(run.stdout)
 for name,seconds in [('pin',0.01),('sample',0.025)]:
  assert all(abs(x-y)<1e-6 for x,y in zip(samples[name],[seconds*30,seconds,seconds*60]))
 realtime_cases=[]
 assert len(samples['realtime'])==len(patterns)
 for pattern,(real,game) in zip(patterns,samples['realtime']):
  # The 5-sample average lags by at most a few presents at the start and the end of the run.
  capped=[min(x,1/15) for x in pattern];expect=real*sum(capped)/sum(pattern)
  assert abs(game-expect)<=3*max(capped),('game time is not real time (hitches cut at 1/15 s)',pattern,real,game,expect)
  realtime_cases.append(dict(intervals_ms=[round(x*1e3,4) for x in pattern],real=real,game=game,ratio=game/real))
 # Same placement as game_patches_v143.c: the producer veneer (4 words) sits in
 # the zero padding after the text segment, so iOS can embed it in the signed RX
 # segment. It must end before read-only data starts, which is stricter than the
 # runtime's 16 KiB page bound.
 patched=target.read_bytes();data=(len(raw)+0x3fff)&~0x3fff;stub=(layout['TEXT_END']+15)&~15
 assert stub+16<=layout['RO_OFF'],'frame-step veneer would overlap read-only data'
 assert raw[stub:stub+16]==bytes(16),'frame-step veneer padding not empty in the rewritten image'
 changes=[o for o in range(0,len(raw),4) if raw[o:o+4]!=patched[o:o+4]]
 allowed={0x2db204,0x2db208,0x2db22c,0x2db360,0x2db36c,0x2db340,0x2db34c,0x3e5658,0x2daa54,0x2daa58,0x2daa6c,0x2daa70,0x2daa74,0x2daa78,
          0x246eac,0x345904,0x24944c,0x28bc98,0x628c74,0xb43460,0x1ff04f4,0x253fbb0,0x562a64,
          0x585e48,0x585e50,0x585e54,0x585e60}|{stub+4*i for i in range(4)}|{stub+48+4*i for i in range(32)}|{stub+176+4*i for i in range(19)}|{stub+256+4*i for i in range(5)}
 # 0x585e48..0x585e60: patch_render_res, the scene size read from the data page (on by default since e8edbcd)
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
 for hz in (15,20,25,30,50,60,90,100,120,144,165,240,360):
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
 # Execute the original callers, including their fixed 1/30 constructors and
 # the production veneers. Checking only FrameRateMgr missed this second clock.
 assert stub+176+19*4<=layout['RO_OFF'],'cloth veneers overlap read-only data'
 def cloth_clock(hz,factor,site,apply=True):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(patched));mu.mem_write(0,patched if apply else raw)
  mu.mem_map(0x10000000,0x20000);sp=0x10010000;mu.reg_write(UC_ARM64_REG_SP,sp)
  mu.mem_write(data,struct.pack('<fff',30/hz,1/hz,60/hz))
  def put(reg,value):mu.reg_write(reg,bits(value))
  def get(reg):return struct.unpack('<f',struct.pack('<I',mu.reg_read(reg)))[0]
  # Ordinary animation has already been scaled: it must not be scaled twice.
  ordinary=(.7/hz,.7*30/hz)
  mu.reg_write(UC_ARM64_REG_X9,0x12345678)
  mu.reg_write(UC_ARM64_REG_NZCV,0xa0000000)
  if site==0x246eac:
   start,end=0x246e78,0x246ebc
   put(UC_ARM64_REG_S10,factor);mu.mem_write(sp+0x138,struct.pack('<ff',ordinary[1],ordinary[0]))
   mu.emu_start(start,end,count=100)
   result=struct.unpack('<ff',mu.mem_read(sp+0x360,8))
   normal=struct.unpack('<ff',mu.mem_read(sp+0x2e0,8))
   assert abs(get(UC_ARM64_REG_S10)-factor)<1e-6,'callee-saved time factor changed'
  elif site==0x345904:
   start,end=0x3458d8,0x345910
   put(UC_ARM64_REG_S0,factor);put(UC_ARM64_REG_S9,ordinary[0]);put(UC_ARM64_REG_S8,ordinary[1])
   mu.emu_start(start,end,count=100)
   result=struct.unpack('<ff',mu.mem_read(sp+0x110,8))
   normal=struct.unpack('<ff',mu.mem_read(sp+0x90,8))
   assert abs(get(UC_ARM64_REG_S0)-factor)<1e-6,'original factor changed'
  else:
   start,end=site,0x2a0e40
   put(UC_ARM64_REG_S0,ordinary[0]);put(UC_ARM64_REG_S1,ordinary[1])
   put(UC_ARM64_REG_S2,factor/30);put(UC_ARM64_REG_S3,factor)
   mu.emu_start(start,end,count=100)
   result=(get(UC_ARM64_REG_S2),get(UC_ARM64_REG_S3))
   normal=(get(UC_ARM64_REG_S0),get(UC_ARM64_REG_S1))
   assert mu.reg_read(UC_ARM64_REG_LR)==site+4,'original callee return address changed'
  assert mu.reg_read(UC_ARM64_REG_PC)==end,'veneer did not return to original code'
  assert mu.reg_read(UC_ARM64_REG_SP)==sp,'guest stack changed'
  assert mu.reg_read(UC_ARM64_REG_NZCV)==0xa0000000,'live condition flags changed'
  if site!=0x345904:assert mu.reg_read(UC_ARM64_REG_X9)==0x12345678,'live integer register changed'
  assert all(abs(a-b)<1e-6 for a,b in zip(normal,ordinary)),'ordinary clock was scaled twice'
  return result
 cloth_cases=[]
 for hz in (15,20,30,40,50,60,90,100,120,144,165,240,360):
  for factor in (0,.25,1,2):
   for site in (0x246eac,0x345904,0x24944c,0x28bc98):
    result=cloth_clock(hz,factor,site);want=(factor/hz,factor*30/hz)
    assert all(abs(a-b)<1e-6 for a,b in zip(result,want)),(hz,factor,hex(site),result,want)
    cloth_cases.append(dict(hz=hz,factor=factor,site=hex(site),seconds=result[0],ratio=result[1]))
 cloth_negative=[cloth_clock(120,1,site,False) for site in (0x246eac,0x345904,0x24944c,0x28bc98)]
 assert all(abs(dt*120-4)<1e-5 for dt,ratio in cloth_negative),'negative control must reproduce 4x wind clock at 120 FPS'
 # The third out-of-line caller already uses measured time. Leave it alone.
 assert patched[0x2afac18:0x2afac1c]==raw[0x2afac18:0x2afac1c]
 # A mismatched game instruction must fail closed, for each new patch site.
 bad_source=out/'invalid-cloth-fixture.bin'
 try:
  for site in (0x246eac,0x345904,0x24944c,0x28bc98):
   bad=bytearray(raw);bad[site]^=1;bad_source.write_bytes(bad)
   rejected=subprocess.run(['build/v143-timing-fixture',str(bad_source),str(out/'rejected-cloth-fixture.bin'),str(layout['TEXT_END'])],capture_output=True,text=True,env=fixture_env)
   assert rejected.returncode!=0 and 'secondary animation instruction mismatch' in rejected.stderr,(hex(site),rejected.stderr)
 finally:bad_source.unlink(missing_ok=True)
 # Trampoline slow-motion check (#61): the contact code compares the time object's FPS (X+0x28) with 100. The veneer
 # must compare FPS * 30 Hz ratio (X+0x38) = 30 / time scale, keep the original 100.0 in s1 and return to 0x562a6c.
 def bounce_check(hz,scale,apply=True):
  mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);mu.mem_map(0,len(patched));mu.mem_write(0,patched if apply else raw)
  mu.mem_map(0x10000000,0x10000);holder,obj,x=0x10000000,0x10001000,0x10002000
  ratio=30/hz;mu.mem_write(holder,struct.pack('<Q',obj));mu.mem_write(obj+0xc8,struct.pack('<Q',x))
  mu.mem_write(x+0x28,struct.pack('<f',30/(ratio*scale)));mu.mem_write(x+0x38,struct.pack('<f',ratio))
  mu.reg_write(UC_ARM64_REG_X25,holder);mu.emu_start(0x562a5c,0x562a70,count=20)
  assert mu.reg_read(UC_ARM64_REG_PC)==0x562a70,'bounce veneer did not return to original code'
  assert mu.reg_read(UC_ARM64_REG_S1)==0x42c80000,'bounce check threshold changed'
  return struct.unpack('<f',struct.pack('<I',mu.reg_read(UC_ARM64_REG_S0)&0xffffffff))[0]
 bounce_cases=[]
 for hz in (30,60,100,120,144,240,360):
  for scale in (1,.5,.25,.1):
   got=bounce_check(hz,scale);assert abs(got-30/scale)<1e-3,(hz,scale,got)
   bounce_cases.append(dict(hz=hz,scale=scale,compared=got))
 assert bounce_check(120,1,False)>100,'negative control: the original check fires at 120 FPS with time running normally'
 assert abs(bounce_check(30,.25,False)-bounce_check(30,.25))<1e-3,'at 30 Hz the patched check must equal the original'
 report=dict(status='pass',version='v143',scope='production ARM64 timing and secondary cloth/hair clock veneers; original clock arithmetic, half-step selector, slow motion, zero factor, both inline/delegated actor paths, ordinary clock and ABI preservation',cases=cases,realtime_cases=realtime_cases,negative_control=negative,cloth_cases=cloth_cases,cloth_negative_control=cloth_negative,cloth_instruction_guard_negative_controls=4,bounce_cases=bounce_cases,veneer_offset=hex(stub),changed_original_words=len(changes),sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in ('runtime/game_patches_v143.c','scripts/v143_timing_fixture.c','scripts/verify_v143_timing.py')})
 (out/'realtime-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(status='pass',cases=len(cases),realtime_ratios=[round(c['ratio'],6) for c in realtime_cases],cloth_cases=len(cloth_cases),cloth_negative_control=cloth_negative,bounce_cases=len(bounce_cases),changed_original_words=len(changes))))
if __name__=='__main__':main()
