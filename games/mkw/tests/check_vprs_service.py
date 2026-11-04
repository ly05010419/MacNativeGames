#!/usr/bin/env python3
from pathlib import Path
import json,subprocess,os,struct
import argparse,tempfile
from shader_tools import service_command
parser=argparse.ArgumentParser(description='Check the Ampere service full-rate shading rewrite against a private archive corpus')
parser.add_argument('jobs',type=Path);parser.add_argument('--limit',type=int,default=16)
args=parser.parse_args()
p=args.jobs.parent; jobs={j['key']:j for j in json.load(open(args.jobs))}; bad=[]
for f in p.glob('*.res.json'):
 if any('Attribute offset' in n for n in json.loads(f.read_text()).get('notes',[])):bad.append(jobs[f.name.removesuffix('.res.json')])
requests=[dict(j,variant=j['key'],tex={},img={},attr={},fout={}) for j in bad[:args.limit]]
temp=tempfile.TemporaryDirectory(prefix='mkw-vprs-');out=Path(temp.name)
env=os.environ.copy()
r=subprocess.run(service_command(out,'mkw-sm86-v2-unverified'),input=''.join(json.dumps(j)+'\n' for j in requests),text=True,capture_output=True,env=env,check=True)
replies=[json.loads(s) for s in r.stdout.splitlines()]; assert len(replies)==len(requests);assert all(x['ok'] for x in replies),r.stdout
assert requests, 'No invalid-attribute shaders in the supplied corpus'
changed=0
for j in requests:
 original=Path(j['code']).read_bytes();lowered=(out/(j['key']+'.fullrate.code')).read_bytes();assert len(original)==len(lowered)
 for at in range(0,len(original),16):
  a,b=original[at:at+16],lowered[at:at+16]
  if a==b:continue
  assert at>=0x80
  lo,hi=struct.unpack('<QQ',a);nlo,nhi=struct.unpack('<QQ',b)
  assert (lo&4095)==0x322 and ((lo>>40)&1023)==0x5c and ((lo>>24)&255)==255 and ((hi>>10)&15)==0
  assert nlo==0x918|(lo&0xf000) and nhi==hi&0xffffffff00000000
  changed+=1
print('accepted',len(requests),'previously rejected shaders; changed',changed,'targeted stores; all other bytes preserved')

temp.cleanup()
