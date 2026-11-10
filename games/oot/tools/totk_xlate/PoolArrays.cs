// Typed immutable GPU descriptor tables. Layout reference: ChiefSZ13/nxnative
// ee9b7765, dmsh_build.py pool_arrays (MIT). Host implementation is ours.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
static partial class ShaderService
{
    static string PoolArrays(string src)
    {
        Match body=Regex.Match(src,@"struct spvDescriptorSetBuffer0\s*\{(.*?)\};",RegexOptions.Singleline);
        if(!body.Success)return src;
        var pools=new Dictionary<string,string>();
        string members=Regex.Replace(body.Groups[1].Value,@"^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(",m=>{
            if(!m.Groups[4].Value.EndsWith("_cb0_40000000",StringComparison.Ordinal)||int.Parse(m.Groups[3].Value)<4096)return m.Value;
            pools.Add(m.Groups[4].Value,m.Groups[2].Value);
            return m.Groups[1].Value+"constant spvPool_"+m.Groups[4].Value+"* "+m.Groups[4].Value+" [[id(";
        },RegexOptions.Multiline);
        if(pools.Count==0)return src;
        int aid=0;
        members=Regex.Replace(members,@"^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];",m=>{
            string text=$"{m.Groups[1].Value}{m.Groups[2].Value} {m.Groups[3].Value} [[id({aid})]];";
            Match a=Regex.Match(m.Groups[2].Value,@"^array<.+, (\d+)>$");aid+=a.Success?int.Parse(a.Groups[1].Value):1;return text;
        },RegexOptions.Multiline);
        string structs=string.Concat(pools.Select(p=>$"struct spvPool_{p.Key} {{ uint n; array<{p.Value}, 1> e; }};\n"));
        src=src[..body.Index]+structs+src[body.Index..body.Groups[1].Index]+members+src[(body.Groups[1].Index+body.Groups[1].Length)..];
        foreach(string name in pools.Keys){
            int pos=0;var output=new System.Text.StringBuilder();
            foreach(Match m in Regex.Matches(src,@"([\w.]+)\."+Regex.Escape(name)+@"\[")){
                if(m.Index<pos)continue;int depth=1,end=m.Index+m.Length;
                while(depth!=0){if(end>=src.Length)throw new RuntimeError("unbalanced pool index: "+name);char c=src[end++];depth+=c=='['?1:c==']'?-1:0;}
                string b=m.Groups[1].Value+"."+name,index=src[(m.Index+m.Length)..(end-1)];
                output.Append(src[pos..m.Index]).Append($"(&{b}->e[0])[min(uint({index}), {b}->n)]");pos=end;
            }
            src=output.Append(src[pos..]).ToString();
            int reads=Regex.Matches(src,@"\."+Regex.Escape(name)+@"->e\[0\]\)\[").Count;
            if(Regex.Matches(src,@"\b"+Regex.Escape(name)+@"\b").Count!=reads*2+1)throw new RuntimeError("unhandled pool resource use: "+name);
        }
        return src;
    }
}
