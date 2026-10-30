#!/usr/bin/env python3
"""Write an observed title/battle prewarm manifest into the private profile."""
import argparse
import collections
import json
from pathlib import Path
import re

from build_aoc import GAME,TITLE


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',required=True,type=Path)
    parser.add_argument('sessions',type=Path,nargs='+')
    args=parser.parse_args();w=args.work_dir.expanduser().resolve()
    if w==GAME.parents[1] or GAME.parents[1] in w.parents:parser.error('use an external workspace')
    profile=w/f'source/dm/games/{TITLE}'
    known={c[3] for line in (profile/'shader_map.tsv').read_text().splitlines() if len(c:=line.split('\t'))==4 and c[0].endswith('.bin')}
    rows=set();provenance=[]
    for session in args.sessions:
        run=json.loads((session/'run.json').read_text())
        provenance.append({'session':session.name,'host_sha256':run['host_sha256']})
        for line in (session/'pso-record.tsv').read_text().splitlines():
            if not line or line.startswith('#'):continue
            s=re.fullmatch(r'S\t([0-9a-f]{16})(?:_b)?\t[0-5]',line)
            c=re.fullmatch(r'C\t([0-9a-f]{16})(?:_b)?',line)
            if s or c:
                key=(s or c).group(1)
                if key not in known:raise ValueError(f'observed key absent from inventory: {key}')
            elif not re.fullmatch(r'R\t(?:[0-9a-f]{2})+',line):raise ValueError('unexpected pipeline record')
            rows.add(line)
    order={'S':0,'R':1,'C':2}
    output=profile/'pso_manifest.tsv'
    output.write_text('# Observed title/first-battle seed; metadata only, not a whole-game coverage claim.\n'+
                      '\n'.join(sorted(rows,key=lambda r:(order[r[0]],r)))+'\n')
    report={'sources':provenance,'counts':dict(collections.Counter(r[0] for r in rows))}
    (w/'validation/prewarm-seed.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
