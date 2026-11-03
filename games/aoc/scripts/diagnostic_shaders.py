#!/usr/bin/env python3
"""Compile observed shader keys in a separate, explicitly incomplete offline cache."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from build_aoc import GAME, TITLE, environment


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--rebuild',action='store_true',help='rebuild every observed key instead of only missing outputs')
    parser.add_argument('logs',type=Path,nargs='+')
    args=parser.parse_args();w=args.work_dir.expanduser().resolve()
    if w==GAME.parents[1] or GAME.parents[1] in w.parents:parser.error('use an external workspace')
    source=w/'source';profile=source/f'dm/games/{TITLE}';out=w/'validation/shader-cache'
    keys=set()
    for path in args.logs:
        text=path.read_text()
        keys.update(re.findall(r'shader ([0-9a-f]{16})',text))
        keys.update(re.findall(r'^S\t([0-9a-f]{16})(?:_b)?\t',text,re.M))
    # Vertex padding reads its paired pixel reflection/MSL. Include dependencies
    # even when they were not directly requested by the running scene.
    pairs=[line.split() for line in (profile/'shader_pairs.tsv').read_text().splitlines() if line and not line.startswith('#')]
    for vertex,pixel in pairs:
        if vertex in keys:keys.add(pixel)
    available={c[3] for line in (profile/'shader_map.tsv').read_text().splitlines() if len(c:=line.split('\t'))==4 and c[0].endswith('.bin')}
    if not keys or keys-available:raise ValueError(f'no known keys or keys absent from inventory: {sorted(keys-available)}')
    out.mkdir(parents=True,exist_ok=True)
    env=environment(w);env['J']='2';env['WORK']=str(w/'build/tmp');env['DM_ROOT']=str(w/'build')
    # Dedicated CAS: partial diagnostic output never becomes a full-build seed.
    env['DMSH_CAS']=str(w/'validation/shader-cas')
    chain=subprocess.check_output([sys.executable,str(source/'dm/gfx/tools/dmsh_chain.py')],env=env,text=True).strip()
    libraries=out/f'b-{chain}'
    selected=set(keys)
    if not args.rebuild:
        for key in keys:
            library=libraries/f'{key}_b.metallib';reflection=libraries/f'{key}_b.json'
            if library.is_file() and reflection.is_file():
                with library.open('rb') as f:real=f.read(4)==b'MTLB'
                if real and json.loads(reflection.read_text()).get('chain')==chain:selected.discard(key)
    command=[sys.executable,str(source/'dm/gfx/tools/own_compilers.py'),str(w/'build/shader/pkg'),str(out),*sorted(selected)]
    record=out/'observed-keys.json';record.write_text(json.dumps(sorted(keys),indent=2)+'\n')
    if selected:subprocess.run(command,env=env,check=True)
    print(f'{len(keys)} observed/dependency keys; {len(selected)} built; incomplete diagnostic cache: {out}')


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.CalledProcessError) as error:sys.exit(f'error: {error}')
