#!/usr/bin/env python3
"""Summarize one bounded NVN timing window without adding overlapping costs."""
import argparse
import collections
import json
import math
from pathlib import Path
import re


def distribution(values, budget=None):
    values=sorted(x for x in values if math.isfinite(x) and x>=0)
    if not values:return {'samples':0}
    def q(p):
        index=(len(values)-1)*p;low=int(index);high=min(low+1,len(values)-1)
        return values[low]+(values[high]-values[low])*(index-low)
    result=dict(samples=len(values),p50=q(.5),p95=q(.95),p99=q(.99),max=values[-1])
    if budget is not None:result['over_budget_fraction']=sum(x>budget for x in values)/len(values)
    return result


def analyze(session,start=0,end=float('inf'),fps=30):
    frames=collections.defaultdict(dict)
    for line in (session/'frames.tsv').read_text().splitlines():
        c=line.split('\t')
        if len(c)==3 and c[0].isdigit() and c[1].startswith('@'):
            frames[int(c[0])][c[1]]=float(c[2])
    selected={f:v for f,v in frames.items() if start<=v.get('@t_s',-1)<end}
    metrics=('@frame_ms','@display_ms','@presented_ms','@sub_wall_ms','@sub_cpu_ms','@gpu_ms','@upload_ms','@drawable_ms','@wait_ms','@pq_wait_ms')
    result={'window_seconds':[start,end if math.isfinite(end) else None],
            'budget_ms':1000/fps,'frames':len(selected),'percentile_method':'linear interpolation, index (N-1)*p',
            'metrics':{name:distribution([v[name] for v in selected.values() if name in v],1000/fps if name in ('@frame_ms','@display_ms','@presented_ms') else None) for name in metrics}}
    result['library_creations']=sum(v.get('@lib_new',0) for v in selected.values())
    result['pipeline_creations']=sum(v.get('@pso_new',0) for v in selected.values())
    result['discarded_present_samples']=sum(v.get('@presented_ms')==-1 for v in selected.values())
    # Diagnostic errors are whole-run counts, never reported as a frame rate.
    log=(session/'host.log').read_text(errors='replace')
    result['whole_run_red_lines']=[line for line in log.splitlines() if '[RED]' in line]
    result['runtime_used_keys']=sorted(set(re.findall(r'^S\t([0-9a-f]{16})(?:_b)?\t',(session/'pso-record.tsv').read_text(),re.M))) if (session/'pso-record.tsv').exists() else []
    result['notes']=['CPU submit, waits, GPU and whole-frame buckets overlap; do not add them.',
                     'This report cannot establish route equivalence, correctness, thermal stability or absence of concurrent compilation.']
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session',type=Path)
    parser.add_argument('--start',type=float,default=0)
    parser.add_argument('--end',type=float,default=float('inf'))
    parser.add_argument('--fps',type=float,default=30)
    args=parser.parse_args()
    if args.fps<=0 or args.end<=args.start:parser.error('invalid timing window/fps')
    result=analyze(args.session,args.start,args.end,args.fps)
    output=args.session/'timing-summary.json';output.write_text(json.dumps(result,indent=2)+'\n')
    print(output)
