// HMMA.16816 GPU matrix lowering, adapted from ChiefSZ13/nxnative ee9b7765, MIT.
using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
static class TensorMsl {
    const string Body = """
{
    threadgroup uint* s = &%(scr)s[uint(%(blk)s)];
    uint g = uint(%(lane)s) >> 2, t = uint(%(lane)s) & 3;
    s[g * 8 + t] = uint(%(a0)s); s[(g + 8) * 8 + t] = uint(%(a1)s); s[g * 8 + 4 + t] = uint(%(a2)s); s[(g + 8) * 8 + 4 + t] = uint(%(a3)s);
    s[128 + g * 8 + t] = uint(%(b0)s); s[128 + g * 8 + 4 + t] = uint(%(b1)s);
    s[192 + g * 4 + t] = uint(%(acc0)s); s[192 + (g + 8) * 4 + t] = uint(%(acc1)s);
    simdgroup_barrier(mem_flags::mem_threadgroup);
    threadgroup half* h = (threadgroup half*)s;
    simdgroup_half8x8 a00, a01, a10, a11, b0, b1, c0, c1;
    simdgroup_load(a00, h, 16, ulong2(0, 0)); simdgroup_load(a01, h, 16, ulong2(8, 0)); simdgroup_load(a10, h, 16, ulong2(0, 8)); simdgroup_load(a11, h, 16, ulong2(8, 8));
    simdgroup_load(b0, h + 256, 16, ulong2(0, 0), true); simdgroup_load(b1, h + 256, 16, ulong2(8, 0), true);
    simdgroup_load(c0, h + 384, 8, ulong2(0, 0)); simdgroup_load(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_multiply_accumulate(c0, a00, b0, c0); simdgroup_multiply_accumulate(c0, a01, b1, c0);
    simdgroup_multiply_accumulate(c1, a10, b0, c1); simdgroup_multiply_accumulate(c1, a11, b1, c1);
    simdgroup_store(c0, h + 384, 8, ulong2(0, 0)); simdgroup_store(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint d0 = s[192 + g * 4 + t], d1 = s[192 + (g + 8) * 4 + t];
    simdgroup_barrier(mem_flags::mem_threadgroup);
    %(d1)s = as_type<%(dt)s>(d1);
    return as_type<%(rt)s>(d0);
}
""";
    public static string Rewrite(string src) {
        var m=Regex.Match(src,@"\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{");if(!m.Success)return src;
        var pars=new List<string>();int depth=0,at=0;string text=m.Groups[2].Value+",";
        for(int i=0;i<text.Length;i++){if(text[i]=='<')depth++;if(text[i]=='>')depth--;if(text[i]==','&&depth==0){pars.Add(text[at..i].Trim());at=i+1;}}
        var plain=pars.FindAll(p=>!p.StartsWith("threadgroup "));var shared=pars.FindAll(p=>p.StartsWith("threadgroup "));
        if(plain.Count!=11||shared.Count!=1)throw new Exception("HMMA parameter layout");
        string Name(string p)=>Regex.Match(p,@"(\w+)$").Groups[1].Value;
        string[] keys={"lane","blk","a0","a1","a2","a3","b0","b1","acc0","acc1","d1"};
        string b=Body;for(int i=0;i<keys.Length;i++)b=b.Replace("%("+keys[i]+")s",Name(plain[i]));
        b=b.Replace("%(scr)s",Name(shared[0])).Replace("%(rt)s",m.Groups[1].Value).Replace("%(dt)s",Regex.Match(plain[10],@"thread (\w+)&").Groups[1].Value);
        int start=m.Index+m.Length-1,end=start;depth=0;
        for(;end<src.Length;end++){if(src[end]=='{')depth++;if(src[end]=='}'&&--depth==0)break;}
        if(end==src.Length)throw new Exception("HMMA body not closed");
        return src[..start]+b+src[(end+1)..];
    }
}
