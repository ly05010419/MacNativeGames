# Adjacent passes with the same set of attachments (serial + mip level + layer) where the later one loads all of them: if the split in between is not necessary, merging saves one full load and one full store
import re, sys, collections
BPP={70:4,71:4,80:4,81:4,90:4,92:4,93:4,115:8,125:16,55:4,25:2,65:4,105:4,103:4,252:4,250:2,260:5,255:4,10:1,20:2,30:2,60:4,53:4}
passes=collections.OrderedDict(); order=[]; between=collections.defaultdict(set)
FA=None; last=None
for l in open(sys.argv[1]):
    m=re.match(r'\[pass\] F(\d+) P (\d+) (c\d+|d) ser=(\d+) fmt=(\d+) (\d+)x(\d+)(?:.*? lv(\d+))?(?:.*? sl(\d+))?.*?(clear|load)$',l.strip())
    if m:
        f,p,slot,s,fmt,w,h,lv,sl,act=m.groups(); f=int(f); p=int(p)
        if FA is None: FA=f
        if f!=FA: break
        lv=int(lv or 0); b=max(int(w)>>lv,1)*max(int(h)>>lv,1)*BPP.get(int(fmt),4)
        if p not in passes: passes[p]=[]; order.append(p)
        passes[p].append((slot,int(s),lv,int(sl or 0),act,b)); last=p
    m=re.match(r'\[pass\] F(\d+) R (\d+) st\d+ ser=(\d+)',l.strip())
    if m and int(m.group(1))==FA: between[int(m.group(2))].add(int(m.group(3)))   # textures sampled in this pass
key=lambda p: tuple(sorted((a[0],a[1],a[2],a[3]) for a in passes[p]))
n=0; sv=0; ex=[]
for a,b in zip(order,order[1:]):
    if key(a)==key(b) and all(x[4]=='load' for x in passes[b]):
        sers={x[1] for x in passes[a]}
        hazard = bool(sers & between[b])                       # the later pass samples its own attachment as a texture: cannot merge
        bb=sum(x[5] for x in passes[b])
        if not hazard: n+=1; sv+=2*bb; ex.append((a,b,bb))
print('frame %d: %d passes; splits between adjacent passes with the same attachments where the later pass loads them all and does not sample them: %d; merging saves %.1f MB/frame (one load and one store)'%(FA,len(order),n,sv/1e6))
for a,b,bb in ex[:12]: print('  %d→%d: %s  %.1f MB'%(a,b,' '.join('%s:ser%d'%(x[0],x[1]) for x in passes[b]),bb/1e6))
