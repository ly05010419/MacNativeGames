import os, re, signal, subprocess, sys, time
def gpu_stats():
  out = subprocess.run(['ioreg','-r','-d','1','-w','0','-c','AGXAccelerator'], capture_output=True, text=True).stdout
  a = re.search(r'"Alloc system memory"=(\d+)', out); u = re.search(r'"In use system memory"=(\d+)', out)
  pb = re.search(r'"Allocated PB Size"=(\d+)', out)
  return (int(a.group(1)) >> 20 if a else -1, int(u.group(1)) >> 20 if u else -1, int(pb.group(1)) >> 20 if pb else -1)
def wired_mb():
  out = subprocess.run(['vm_stat'], capture_output=True, text=True).stdout
  page = int(re.search(r'page size of (\d+)', out).group(1)); w = int(re.search(r'Pages wired down:\s+(\d+)', out).group(1))
  return w * page >> 20
log_path = sys.argv[1]; bound = float(sys.argv[2]) if len(sys.argv) > 2 else 20
env = os.environ.copy()
env.update(DOTNET_ROOT='/Users/luis/.dotnet',
  MKW_DMSH_XLATE='/private/tmp/mkw-nxnative-mesh/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate',
  MKW_AMPERE_SERVICE='/private/tmp/mkw-ampere-service-mesh/ampere_service',
  MKW_MESH_STAGE_WRAPPER='/Users/luis/gitrepos/MacNativeGames/games/mkw/tools/wrap_mesh_stage.py',
  MKW_SPIRV_CROSS='/private/tmp/mkw-nxnative-mesh/dm/gfx/tools/spirv_cross_bin/spirv-cross',
  TOTK_ASSUME_FOCUS='1', TOTK_RENDER_STREAM='0', TOTK_GPU_MIDCLEAR='1', TOTK_GPU_ALLOW_SUBMIT='1')
for kv in sys.argv[3:]:
  k, v = kv.split('=', 1); env[k] = v
subprocess.Popen(['caffeinate','-d','-u','-t',str(int(bound)+10)]); time.sleep(2)
t0 = time.time()
with open(log_path, 'w') as log:
  p = subprocess.Popen(['build/mkw-native', 'rewritten/mkw', 'boot'], cwd='/private/tmp/mkw-codex-run', env=env,
                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
  wired_trace = []; peak = 0; limit = int(env.get('RUN_WIRED_LIMIT_MB', '11000'))
  try:
    while True:
      try: print('exit', p.wait(timeout=0.25)); break
      except subprocess.TimeoutExpired: pass
      w = wired_mb(); peak = max(peak, w); wired_trace.append((round(time.time()-t0,1), w, os.path.getsize(log_path)) + gpu_stats())
      if w > int(env.get('RUN_FOOTPRINT_MB', '4500')) and not os.path.exists(log_path + '.fp'):
        subprocess.Popen(['footprint', '-p', str(p.pid)], stdout=open(log_path + '.fp', 'w'), stderr=subprocess.STDOUT)
      if w > limit: print('WIRED LIMIT %d MB exceeded (%d): killing' % (limit, w)); time.sleep(1.5); break
      sa = float(env.get('RUN_SAMPLE_AT', '0'))
      if sa and time.time() - t0 > sa and not os.path.exists(log_path + '.sample'):
        subprocess.Popen(['sample', str(p.pid), '2', '-file', log_path + '.sample'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
      if time.time() - t0 > bound: print('bound reached'); break
  finally:
    print('peak wired %d MB' % peak); open(log_path + '.wired', 'w').write('\n'.join(' '.join(map(str, x)) for x in wired_trace))
    try: os.killpg(p.pid, signal.SIGTERM)
    except ProcessLookupError: pass
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired:
      os.killpg(p.pid, signal.SIGKILL); p.wait()
print('elapsed %.1f' % (time.time() - t0))
