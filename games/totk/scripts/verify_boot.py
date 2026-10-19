#!/usr/bin/env python3
"""Validate phase 4 startup milestones, not rendering or playability."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--version',choices=('v0','v112','v121','v143'),default='v0');version=p.parse_args().version
exe=root/('build/totk-native' if version=='v0' else f'build/totk-native-{version}')
out=root/('analysis' if version=='v0' else f'analysis/{version}-port')
checkpoint_pc={'v0':'0xe995c4','v112':'0xea7e14','v121':'0xe7a954','v143':'0xbdb48c'}[version]
expected=['ENTER nnosInitialize','RETURN nnosInitialize','ENTER nninitInitializeSdkModule','RETURN nninitInitializeSdkModule','ENTER nninitStartup','RETURN nninitStartup','ENTER main DT_INIT','RETURN main DT_INIT','ENTER subsdk0 DT_INIT','RETURN subsdk0 DT_INIT','ENTER sdk DT_INIT','RETURN sdk DT_INIT','ENTER nnMain']
trials=[]
for i in range(2):
    env=os.environ.copy();env['TOTK_STARTUP_ENV_PADDING']='x'*(i*1031);env['TOTK_VERIFY_STARTUP']='1'
    run=subprocess.run([str(exe),f'rewritten/{version}','boot'],cwd=root,env=env,capture_output=True,text=True,timeout=180)
    log=run.stderr;(out/f'native-boot-trial-{i}.log').write_text(log)
    position=0
    for milestone in expected:
        position=log.index('[totk] '+milestone,position)+len(milestone)
    assert run.returncode==0, log[-4000:]
    checkpoint=json.loads(run.stdout);assert checkpoint['status']=='pass' and checkpoint['original_nnMain_return_pc']==checkpoint_pc
    states=re.findall(r'RETURN (main|sdk|subsdk0) DT_INIT state=(\d+) init_array_count=(\d+)',log)
    assert len(states)==3 and all(state=='2' and int(count)>0 for _,state,count in states)
    trials.append(dict(exit_code=run.returncode,log_sha256=hashlib.sha256(log.encode()).hexdigest(),constructor_states=states,checkpoint=checkpoint))
negative=subprocess.run([str(exe),f'rewritten/{version}','probe-identity-negative'],cwd=root,capture_output=True,text=True)
assert negative.returncode==1 and 'guest nnMain address identity changed' in negative.stderr
report=dict(identity_negative_control='rejected before execution',status='pass',phase=4,milestones=expected,trials=trials,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),scope='original SDK Start, SDK initialization, all three constructor loops with DSO state 2, original nnMain prefix confirmed by original return PC; stops at a diagnostic checkpoint, no frame/gameplay')
(out/'phase4-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
