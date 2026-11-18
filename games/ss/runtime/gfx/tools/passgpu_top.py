# Per-pass GPU: average the timeline increment inc_ms by in-frame index; compare against @gpu_ms (for the columns see the DM_NVN_PASSGPU comment in nvn_mtl.m)
import sys, collections, statistics as S
X=sys.argv[1]; F0=int(sys.argv[2]) if len(sys.argv)>2 else 300; N=int(sys.argv[3]) if len(sys.argv)>3 else 10
rows=[l.rstrip('\n').split('\t') for l in open(f'{X}/passgpu.tsv')][1:]
inc=collections.defaultdict(list); per=collections.defaultdict(float); meta={}
for r in rows:
    f=int(r[0])
    if f<=F0: continue
    inc[int(r[1])].append(float(r[4])); per[f]+=float(r[4]); meta[int(r[1])]=r
g={}
try:
    for l in open(f'{X}/frames.tsv'):
        p=l.rstrip('\n').split('\t')
        if len(p)==3 and p[0].isdigit() and p[1]=='@gpu_ms': g[int(p[0])]=float(p[2])
except FileNotFoundError: pass
c=[f for f in per if f in g]
print('frames %d; per-frame sum of increments p50 %.2f / p95 %.2f ms%s'%(len(per),S.median(per.values()),sorted(per.values())[int(len(per)*.95)],'; same-frame @gpu_ms p50 %.2f ms'%S.median(g[f] for f in c) if c else ''))
tot=sum(S.mean(v) for v in inc.values())
print('top passes (in-frame index: mean increment ms, share | size, color format, depth format, load, draws, kind)')
for k in sorted(inc,key=lambda k:-S.mean(inc[k]))[:N]:
    r=meta[k]; m=S.mean(inc[k]); print('  #%d: %.3f ms %4.1f%% | %sx%s fmt%s d%s %s %s draws %s'%(k,m,100*m/tot,r[5],r[6],r[7],r[8],r[9],r[10],r[2]))
print('passes / frame: %d; total %.2f ms'%(len(inc),tot))
