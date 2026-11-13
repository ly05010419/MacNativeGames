// NVN2 task/mesh GPU stages. Adapted from ChiefSZ13/nxnative dmsh_meshstage.py,
// MIT, ee9b7765d579c46e45a7297493565e098f3bda33. Uses our shader service and host.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
sealed class MeshStage
{
    readonly string kind;
    readonly int threads, payload, maxV, maxP;
    readonly bool task, deviceAllowed;   // deviceAllowed: the request may say stage_device=false (tests reading the stage memory from a compute wrapper)
    readonly List<(int n,int c)> vertex = new(), primitive = new();
    readonly int[] layout;
    readonly int shared, blocks;
    // Device stage memory (2026-11-08). A mesh shader's stage memory as Metal threadgroup memory (the attribute area of
    // up to 128 vertices x ~25 components: 11-16 KiB a 32-thread threadgroup) let a GPU core hold two or three
    // threadgroups: the mesh stage ran at ~5 % occupancy and every raw-memory latency showed (Ocarina of Time village:
    // 6 ms of a 22 ms GPU frame in four mesh shaders). Stage memories over DeviceStageBytes live in a device buffer
    // the host binds at mesh buffer ScratchBuffer (nvn_mesh_tasks.inc mtk_scratch_gpu, same constants): header word
    // 0 a counter, 1 the slot mask, 2 the slot stride in words; one in-use flag a slot from word ScratchFlagsWord;
    // the slots from word ScratchSlotsWord. A threadgroup takes a free slot (counter, then exchange on the flag) and
    // releases it after its exports; a slot is never reused while its threadgroup runs, whatever the GPU's schedule.
    public const int ScratchBuffer = 28, ScratchForcedWord = 3, ScratchFlagsWord = 4, ScratchSlotsWord = 8192, ScratchSlotWords = 4096, DeviceStageBytes = 4096, ScratchSpinLimit = 1 << 16;
    public bool Device { get; private set; }
    public int ByteStores { get; private set; }
    // A byte store into the stage memory (a primitive's index bytes, STAGEOUT of 8 bits) comes out of the translator as a
    // compare-exchange loop inserting 8 bits into the word, the only way SPIR-V can write a byte of a uint array. Metal's
    // byte stores are byte-granular on threadgroup and device memory (lanes writing the neighbouring bytes of a word do
    // not disturb each other), so the helper becomes one store; little-endian, byte k of a word at bits 8k, as the
    // loop's shift (address & 3) << 3 says. Measured 2026-11-08 (village, fed302d7 alone): GPU frame 20.5 -> 19.7 ms.
    static readonly Regex ByteStoreLoop=new(@"void (_\d+)\(thread const int& (_\d+), thread const int& (_\d+), (threadgroup|device) spvUnsafeArray<uint, (\d+)>& (_\d+)\)\n\{\n(.*?)\n\}\n",RegexOptions.Singleline);
    string ByteStores_(string src){
        int n=0;
        src=ByteStoreLoop.Replace(src,m=>{
            string body=m.Groups[7].Value;
            if(!body.Contains("insert_bits(")||!body.Contains(", 8u)")||!body.Contains("atomic_compare_exchange_weak_explicit")||!body.Contains(">> uint(2)")||!body.Contains("& 3")||!body.Contains("<< 3"))return m.Value;
            n++;
            return $"void {m.Groups[1].Value}(thread const int& {m.Groups[2].Value}, thread const int& {m.Groups[3].Value}, {m.Groups[4].Value} spvUnsafeArray<uint, {m.Groups[5].Value}>& {m.Groups[6].Value})\n{{\n    reinterpret_cast<{m.Groups[4].Value} uchar*>(&{m.Groups[6].Value}[0])[uint({m.Groups[2].Value})] = uchar(uint({m.Groups[3].Value}));\n}}\n";
        });
        ByteStores=n;return src;
    }
    static int Align(int n,int a)=>(n+a-1)&-a;
    static List<(int n,int c)> Outputs(byte[] a,int at) {
        if(at<0 || at+0x80>a.Length) throw new Exception("mesh SPH outside code area");
        var r=new List<(int,int)>();
        for(int c=0;c<4;c++) if((a[at+0x35]>>(4+c)&1)!=0) r.Add((-1,c));
        for(int n=0;n<32;n++) for(int c=0;c<4;c++) if((a[at+0x36+n/2]>>(4*(n%2)+c)&1)!=0) r.Add((n,c));
        return r;
    }
    public MeshStage(JsonObject rq,JsonObject job,string outDir) {
        byte[] ctl=Convert.FromHexString((string)rq["mesh_ctl"]), code=File.ReadAllBytes((string)rq["code"]);
        kind=(string)rq["stage"]; threads=BitConverter.ToUInt16(ctl,0x58);payload=BitConverter.ToUInt16(ctl,0x5a);
        deviceAllowed=!(rq["stage_device"] is JsonValue sd && sd.TryGetValue<bool>(out bool allowed) && !allowed);
        if(threads<1||threads>1024||code.Length<0x80)throw new Exception("invalid task/mesh control");
        int output=16+Align(payload,16), prim;
        if(kind=="ts") {prim=output+payload;shared=output+Align(payload,4);}
        else {
            task=ctl[0x5e]!=0;maxV=BitConverter.ToUInt16(ctl,0x60);maxP=BitConverter.ToUInt16(ctl,0x62);
            vertex=Outputs(code,0);
            if((ctl[0x68]&0x80)!=0) {
                int at=checked((int)BitConverter.ToUInt32(ctl,0x64));
                if(at+0x80<=code.Length)primitive=Outputs(code,at).Where(x=>x.n>=0).ToList();
                else {
                    // Primitive SPH metadata may live in the constant area;
                    // runtime code files contain instructions only, while c1
                    // is supplied separately and verified against the control.
                    byte[] constants=Convert.FromHexString((string)rq["c1"]);
                    int first=checked((int)BitConverter.ToUInt32(ctl,0x40));
                    primitive=Outputs(constants,at-first).Where(x=>x.n>=0).ToList();
                }
            }
            if(maxV<1||maxV>256||maxP<1||maxP>512||!vertex.Any(x=>x.n==-1))throw new Exception("mesh outputs exceed Metal limits or have no position");
            blocks=(Math.Max(Math.Max(maxV,maxP),1)+31)/32;
            // The guest exports complete 32-primitive blocks, even when the
            // declared bound ends within the final block (for example 126).
            // Keep the raster bound unchanged; reserve every byte it can write.
            int indexPrimitives=Align(maxP,32);
            prim=output+blocks*Math.Max(vertex.Count+primitive.Count,1)*128;shared=prim+Align(4+3*indexPrimitives,4);
        }
        if(shared>32768)throw new Exception("mesh stage memory exceeds Metal threadgroup limit");
        for(int o=0x80;o+15<code.Length;o+=16) {
            int op=BitConverter.ToUInt16(code,o)&0xfff;
            if(op==0x984||op==0x388||op==0x988||op==0x38c||op==0x98c||op==0xf8c)throw new Exception("mesh has overlapping explicit shared memory");
        }
        layout=new[]{0,16,output,prim};
        string path=Path.Combine(outDir,(string)rq["variant"]+".mesh.code");File.WriteAllBytes(path,code[0x80..]);job["code"]=path;
        job["local"]=new JsonArray(threads,1,1);job["shared"]=shared;job["stage_memory"]=new JsonArray(layout.Select(x=>(JsonNode)JsonValue.Create(x)).ToArray());
    }
    public JsonObject Reflection()=>new(){["kind"]=kind,["threads"]=threads,["payload"]=payload,["task"]=task,["device"]=Device,["byte_stores"]=ByteStores};
    static List<string> Params(string t) {
        var r=new List<string>();int depth=0,at=0;
        for(int i=0;i<t.Length;i++){char c=t[i];if(c==','&&depth==0){r.Add(t[at..i].Trim());at=i+1;}if("([<".Contains(c))depth++;if(")]>".Contains(c))depth--;}
        if(t[at..].Trim().Length>0)r.Add(t[at..].Trim());return r;
    }
    public string Wrap(string msl) {
        var m=Regex.Match(msl,@"kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{\s*threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\n");
        if(!m.Success)throw new Exception(kind+": no kernel stage memory declaration");
        var pars=Params(m.Groups[1].Value);int words=int.Parse(m.Groups[2].Value);string mem=m.Groups[3].Value;
        var names=pars.Select(p=>Regex.Match(p,@"(\w+)\s*(?:\[\[.*\]\])?$").Groups[1].Value).ToList();
        bool device=deviceAllowed&&kind=="ms"&&words*4>DeviceStageBytes&&words<=ScratchSlotWords;
        string space=device?"device":"threadgroup";
        var plain=pars.Select(p=>Regex.Replace(p,@"\s*\[\[.*\]\]$","")).ToList();plain.Add($"{space} spvUnsafeArray<uint, {words}>& {mem}");
        string src=msl[..m.Index]+$"static inline __attribute__((always_inline)) void dm_stage_body({string.Join(", ",plain)})\n{{\n"+msl[(m.Index+m.Length)..];
        if(device){
            // The body and its helpers address the stage memory through references to this array type, atomics on it
            // (byte stores) and threadgroup barriers; all three move to the device address space. Anything else in the
            // threadgroup address space is not expected (explicit shared memory is rejected above): keep that shader
            // in threadgroup memory rather than compile something unverified.
            src=src.Replace($"threadgroup spvUnsafeArray<uint, {words}>&",$"device spvUnsafeArray<uint, {words}>&")
                   .Replace("(threadgroup atomic_uint*)","(device atomic_uint*)")
                   .Replace("threadgroup_barrier(mem_flags::mem_threadgroup)","threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device)");
            string probe=Regex.Replace(src,@"threadgroup_barrier\(|(const )?threadgroup T& operator \[\] \(size_t pos\)( const)? threadgroup","");
            if(Regex.IsMatch(probe,@"\bthreadgroup\b")){device=false;space="threadgroup";
                plain[^1]=$"threadgroup spvUnsafeArray<uint, {words}>& {mem}";
                src=msl[..m.Index]+$"static inline __attribute__((always_inline)) void dm_stage_body({string.Join(", ",plain)})\n{{\n"+msl[(m.Index+m.Length)..];}
        }
        Device=device;
        src=ByteStores_(src);
        string lid="dm_lid",tg="dm_tg";var extras=new List<string>();
        foreach(string attr in new[]{"thread_position_in_threadgroup","threadgroup_position_in_grid"}){
            var t=pars.Select(p=>Regex.Match(p,@"(uint3?)\s+(\w+)\s*\[\[("+attr+@")\]\]$")).FirstOrDefault(x=>x.Success);
            string name=t!=null?t.Groups[2].Value+(t.Groups[1].Value=="uint3"?".x":""):attr=="thread_position_in_threadgroup"?"dm_lid":"dm_tg";
            if(t==null)extras.Add($"uint {name} [[{attr}]]");if(attr=="thread_position_in_threadgroup")lid=name;else tg=name;
        }
        int pay=(payload+3)/4,output=layout[2]/4,prim=layout[3],input=4;
        string call=$"dm_stage_body({string.Join(", ",names.Append(mem))});";
        string bar=device?"threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);":"threadgroup_barrier(mem_flags::mem_threadgroup);";
        string start=device
            ?$"    threadgroup uint dm_slot_tg;\n    if({lid}==0u){{ uint s, tries=0u; for(;;){{ s=atomic_fetch_add_explicit((device atomic_uint*)dm_scratch,1u,memory_order_relaxed)&dm_scratch[1]; if(atomic_exchange_explicit((device atomic_uint*)(dm_scratch+{ScratchFlagsWord}u+s),1u,memory_order_relaxed)==0u)break; if(++tries=={ScratchSpinLimit}u){{atomic_fetch_add_explicit((device atomic_uint*)(dm_scratch+{ScratchForcedWord}u),1u,memory_order_relaxed);break;}} }} dm_slot_tg=s; }}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n    device spvUnsafeArray<uint, {words}>& {mem}=*reinterpret_cast<device spvUnsafeArray<uint, {words}>*>(dm_scratch+{ScratchSlotsWord}u+dm_slot_tg*dm_scratch[2]);\n    for(uint i={lid}; i<{words}u;i+={threads}u) {mem}[i]=0u;\n    {bar}\n"
            :$"    threadgroup spvUnsafeArray<uint, {words}> {mem};\n    for(uint i={lid}; i<{words}u;i+={threads}u) {mem}[i]=0u;\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n";
        string scratch=device?$", device uint* dm_scratch [[buffer({ScratchBuffer})]]":"";
        string release=device?$"dm_release_slot(dm_scratch,dm_slot_tg,{lid});\n":"";
        string head=(device?$"static inline __attribute__((always_inline)) void dm_release_slot(device uint* dm_scratch, uint slot, uint lid) {{ threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device); if(lid==0u) atomic_store_explicit((device atomic_uint*)(dm_scratch+{ScratchFlagsWord}u+slot),0u,memory_order_relaxed); }}\n":"")
            +$"struct dm_payload_t {{ uint w[{Math.Max(pay,1)}]; }};\n",sig=string.Join(", ",pars.Concat(extras)),first="constant uint* dm_first [[buffer(29)]]";
        if(kind=="ts")return src+"\n"+head+$"[[object]] void main0({sig}, object_data dm_payload_t& dm_payload [[payload]], mesh_grid_properties dm_grid, {first})\n{{\n{start}    if({lid}==0u){mem}[0]=dm_first[0]+{tg};\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n    {call}\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n    for(uint i={lid};i<{pay}u;i+={threads}u)dm_payload.w[i]={mem}[{output}u+i];\n    if({lid}==0u)dm_grid.set_threadgroups_per_grid(uint3(min({mem}[{output+1}],1024u),1u,1u));\n}}\n";
        var vn=vertex.Select(x=>x.n).Distinct().ToList();var pn=primitive.Select(x=>x.n).Distinct().ToList();
        head+="struct dm_vertex_t { float4 position [[position, invariant]];\n"+string.Concat(vn.Where(n=>n>=0).Select(n=>$"float4 a{n} [[user(locn{n})]];\n"))+"};\n";
        if(pn.Count>0)head+="struct dm_prim_t {\n"+string.Concat(pn.Select(n=>$"float4 p{n} [[user(locn{n})]];\n"))+"};\n";
        head+=$"using dm_mesh_t=metal::mesh<dm_vertex_t,{(pn.Count>0?"dm_prim_t":"void")},{maxV},{maxP},metal::topology::triangle>;\n";
        /* The guest's attribute area: every vertex block first, then every primitive block. A block holds 32 vertices
           (or primitives), one 32-word row per output component: vertex component k of vertex v at word
           output + (v/32)*V*32 + k*32 + v%32, primitive component k of primitive p at output + blocks*V*32 + (p/32)*P*32
           + k*32 + p%32 (V, P: the vertex / primitive output components, blocks for the larger of maxV / maxP; the
           index bytes follow at blocks*(V+P) rows, layout[3]). Until 2026-11-13 the export read the blocks interleaved,
           (V+P)*32 words a block, the primitive rows after each vertex block: the same addresses for a shader without
           primitive outputs or with a single block, but for 128 vertices / 64 primitives every vertex from 32 on and
           every primitive output came from the wrong words. Ocarina of Time's Forest Temple paintings (task 2d324024,
           mesh ee4ef51a, fragment a75838cd: 12,000 brush strokes, each coloured from the Poe picture as a primitive
           output) were left black or showed uninitialised memory (green). */
        int vstride=Math.Max(vertex.Count,1)*32, pstride=Math.Max(primitive.Count,1)*32, pbase=output+blocks*vertex.Count*32;
        string Vec(List<(int n,int c)> comps,int n)=>"float4("+string.Join(", ",Enumerable.Range(0,4).Select(c=>{int k=comps.IndexOf((n,c));return k<0?(c==3?"1.0":"0.0"):$"as_type<float>({mem}[b+{k*32}u])";}))+")";
        string sets=string.Concat(vn.Select(n=>$"v.{(n<0?"position":"a"+n)}={Vec(vertex,n)};\n"));
        string psets=string.Concat(pn.Select(n=>$"p.p{n}={Vec(primitive,n)};\n"));
        string Entry(string name,bool gl){
            string fill=task?$"for(uint i={lid};i<{pay}u;i+={threads}u){mem}[{input}u+i]=dm_payload.w[i];\n{bar}\nif({lid}==0u){{{mem}[0]={tg};{mem}[{input}]={tg};}}\n":$"if({lid}==0u){mem}[0]=dm_first[0]+{tg};\n";
            return $"[[mesh]] void {name}({sig}, dm_mesh_t dm_out, "+(task?"const object_data dm_payload_t& dm_payload [[payload]]":first)+scratch+$")\n{{\n{start}{fill}{bar}\n{call}\n{bar}\nuint np=min({mem}[{prim/4}]>>24,{maxP}u);\nif({lid}==0u)dm_out.set_primitive_count(np);\nfor(uint n={lid};n<{maxV}u;n+={threads}u){{uint b={output}u+(n/32u)*{vstride}u+n%32u; dm_vertex_t v;\n{sets}"+(gl?"v.position.z=fma(v.position.z,0.5,v.position.w*0.5);\n":"")+"dm_out.set_vertex(n,v);}\n"+(pn.Count>0?$"for(uint n={lid};n<np;n+={threads}u){{uint b={pbase}u+(n/32u)*{pstride}u+n%32u;dm_prim_t p;{psets}dm_out.set_primitive(n,p);}}\n":"")+$"for(uint n={lid};n<np*3u;n+={threads}u){{uint at={prim+4}u+n;dm_out.set_index(n,({mem}[at>>2]>>((at&3u)*8u))&255u);}}\n{release}}}\n";
        }
        return src+"\n"+head+Entry("main0",true)+"\n"+Entry("main0_z",false);
    }
}
