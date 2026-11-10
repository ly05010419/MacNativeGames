/* Opt-in GPU output capture of the real MeshStage-generated functions. The
 * original body, vertex values and index values still feed the original draw.
 * Captures are read only after submission completion. No gameplay decisions.
 * OOT_MESH_GEOMETRY_CAPTURE=<external directory>; prefixes share the input
 * capture's OOT_MESH_INPUT_CAPTURE_FS. First draw and requested passdump frames.
 */
static id<MTLFunction> mtk_mesh_fn(NvShader *,NvShader *);
static id<MTLBuffer> g_mtk_geometry;
static NSString *mtk_geo_dir(void) {
    static int init;static NSString *dir;
    if(!init){init=1;const char *p=getenv("OOT_MESH_GEOMETRY_CAPTURE");if(p&&*p)dir=@(p);}
    return dir;
}
static int mtk_geo_selected(NvShader *fs) {
    static int init;static const char *prefixes,*meshes;
    if(!init){init=1;prefixes=getenv("OOT_MESH_INPUT_CAPTURE_FS");meshes=getenv("OOT_MESH_GEOMETRY_CAPTURE_MS");}
    return mtk_geo_dir()&&fs&&((prefixes&&*prefixes)?diag_shader_prefix_csv_matches(fs->key,prefixes):(meshes&&*meshes));
}
static NSDictionary *mtk_geo_layout(NSString *source) {
    if(!source)return nil;
    NSRegularExpression *mesh=[NSRegularExpression regularExpressionWithPattern:@"metal::mesh<dm_vertex_t,[^,]+,(\\d+),(\\d+),metal::topology::triangle>" options:0 error:nil];
    NSTextCheckingResult *m=[mesh firstMatchInString:source options:0 range:NSMakeRange(0,source.length)];
    NSRegularExpression *decl=[NSRegularExpression regularExpressionWithPattern:@"struct dm_vertex_t\\s*\\{([^}]+)\\};" options:NSRegularExpressionDotMatchesLineSeparators error:nil];
    NSTextCheckingResult *v=[decl firstMatchInString:source options:0 range:NSMakeRange(0,source.length)];
    if(!m||!v||![source containsString:@"uint dm_lid [[thread_position_in_threadgroup]]"]||![source containsString:@"uint dm_tg [[threadgroup_position_in_grid]]"])return nil;
    NSString *body=[source substringWithRange:[v rangeAtIndex:1]];
    NSRegularExpression *field=[NSRegularExpression regularExpressionWithPattern:@"\\b(float4|int4|uint4)\\s+(\\w+)\\s*\\[\\[[^;]+\\]\\];" options:0 error:nil];
    NSArray<NSTextCheckingResult *> *found=[field matchesInString:body options:0 range:NSMakeRange(0,body.length)];
    if(!found.count||found.count!=[[body componentsSeparatedByString:@";"]count]-1)return nil;
    NSMutableArray *fields=[NSMutableArray array];
    for(NSTextCheckingResult *f in found)[fields addObject:@{@"name":[body substringWithRange:[f rangeAtIndex:2]],@"type":[body substringWithRange:[f rangeAtIndex:1]],@"word_offset":@(fields.count*4)}];
    unsigned nv=[[source substringWithRange:[m rangeAtIndex:1]]intValue],np=[[source substringWithRange:[m rangeAtIndex:2]]intValue];
    if(!nv||nv>256||!np||np>512||found.count>32)return nil;
    unsigned payload=0;
    if([source containsString:@"const object_data dm_payload_t& dm_payload"]){
        NSRegularExpression *pd=[NSRegularExpression regularExpressionWithPattern:@"struct dm_payload_t \\{ uint w\\[(\\d+)\\]; \\};" options:0 error:nil];
        NSTextCheckingResult *p=[pd firstMatchInString:source options:0 range:NSMakeRange(0,source.length)];if(!p)return nil;
        payload=[[source substringWithRange:[p rangeAtIndex:1]]intValue];if(payload>8192)return nil;
    }
    unsigned words=(unsigned)found.count*4,header=4+payload,stride=header+nv*words+np*3;
    return @{@"vertices":@(nv),@"primitives":@(np),@"vertex_words":@(words),@"payload_words":@(payload),@"record_header_words":@(header),@"record_words":@(stride),@"fields":fields};
}
static id<MTLFunction> mtk_geo_function(NvShader *ms,NvShader *fs,id<MTLFunction> base) {
    if(!mtk_geo_selected(fs))return base;
    static NSMutableDictionary<NSValue *,id<MTLFunction>> *cache;
    if(!cache)cache=[NSMutableDictionary dictionary];NSValue *key=[NSValue valueWithPointer:(__bridge void *)base];
    if(cache[key])return cache[key];
    NSString *source=fn_src(base)?:sh_src(ms);NSDictionary *layout=mtk_geo_layout(source);
    if(!layout){MLOG("[mesh-output] unsupported diagnostic layout for %s",ms->key);cache[key]=base;return base;}
    unsigned nv=[layout[@"vertices"]unsignedIntValue],words=[layout[@"vertex_words"]unsignedIntValue],stride=[layout[@"record_words"]unsignedIntValue],payload=[layout[@"payload_words"]unsignedIntValue],header=[layout[@"record_header_words"]unsignedIntValue];
    NSRegularExpression *np=[NSRegularExpression regularExpressionWithPattern:@"uint np=min\\([^;]+\\);" options:0 error:nil];
    NSRegularExpression *ix=[NSRegularExpression regularExpressionWithPattern:@"dm_out\\.set_index\\(n,([^;]+)\\);" options:0 error:nil];
    NSUInteger nnp=[np numberOfMatchesInString:source options:0 range:NSMakeRange(0,source.length)],nix=[ix numberOfMatchesInString:source options:0 range:NSMakeRange(0,source.length)];
    if(nnp!=2||nix!=2||! [source containsString:@"dm_mesh_t dm_out, "]||![source containsString:@"dm_out.set_vertex(n,v);"]){MLOG("[mesh-output] unsupported wrapper for %s",ms->key);cache[key]=base;return base;}
    source=[source stringByReplacingOccurrencesOfString:@"dm_mesh_t dm_out, " withString:@"dm_mesh_t dm_out, device uint* oot_geo [[buffer(30)]], "];
    NSString *begin=[NSString stringWithFormat:@"$0\nthreadgroup uint oot_record;\nif(dm_lid==0u)oot_record=(oot_geo[0]==%uu&&oot_geo[5]==%uu)?atomic_fetch_add_explicit((device atomic_uint*)&oot_geo[3],1u,memory_order_relaxed):0xffffffffu;\nthreadgroup_barrier(mem_flags::mem_threadgroup);\nuint oot_base=8u+oot_record*%uu;\nif(dm_lid==0u&&oot_record<oot_geo[4]){oot_geo[oot_base]=np;oot_geo[oot_base+1u]=dm_tg;oot_geo[oot_base+2u]=%uu;oot_geo[oot_base+3u]=0u;}\n",nv,stride,stride,words];
    if(payload)begin=[begin stringByAppendingFormat:@"if(oot_record<oot_geo[4])for(uint i=dm_lid;i<%uu;i+=%uu)oot_geo[oot_base+4u+i]=dm_payload.w[i];\n",payload,ms->mesh_threads];
    begin=[begin stringByAppendingFormat:@"if(dm_lid==0u&&oot_record!=0xffffffffu)atomic_fetch_add_explicit((device atomic_uint*)&oot_geo[8u+oot_geo[4]*%uu+np],1u,memory_order_relaxed);\n",stride];
    source=[np stringByReplacingMatchesInString:source options:0 range:NSMakeRange(0,source.length) withTemplate:begin];
    NSMutableString *vertex=[NSMutableString stringWithString:@"if(oot_record<oot_geo[4]){"];
    for(NSDictionary *field in layout[@"fields"]){unsigned at=[field[@"word_offset"]unsignedIntValue];
        [vertex appendFormat:@"uint4 oot_v%u=as_type<uint4>(v.%@);for(uint k=0u;k<4u;k++)oot_geo[oot_base+%uu+n*%uu+k]=oot_v%u[k];",at,field[@"name"],header+at,words,at];}
    [vertex appendString:@"}dm_out.set_vertex(n,v);"];
    source=[source stringByReplacingOccurrencesOfString:@"dm_out.set_vertex(n,v);" withString:vertex];
    NSString *index=[NSString stringWithFormat:@"uint oot_i=$1;if(oot_record<oot_geo[4])oot_geo[oot_base+%uu+n]=oot_i;dm_out.set_index(n,oot_i);",header+nv*words];
    source=[ix stringByReplacingMatchesInString:source options:0 range:NSMakeRange(0,source.length) withTemplate:index];
    NSError *error=nil;id<MTLLibrary> library=variant_library(source,&error);id<MTLFunction> function=library?[library newFunctionWithName:base.name]:nil;
    if(!function){MLOG("[mesh-output] diagnostic compile failed for %s: %s",ms->key,error.localizedDescription.UTF8String);cache[key]=base;return base;}
    fn_note_src(function,source);cache[key]=function;return function;
}
static void mtk_geo_prepare(NvShader *ms,NvShader *fs) {
    g_mtk_geometry=nil;if(!mtk_geo_selected(fs))return;
    const char *meshes=getenv("OOT_MESH_GEOMETRY_CAPTURE_MS");
    if(meshes&&*meshes&&!diag_shader_prefix_csv_matches(ms->key,meshes))return;
    static NSMutableDictionary<NSString *,NSNumber *> *done;if(!done)done=[NSMutableDictionary dictionary];
    uint64_t frame=atomic_load(&g_cur_frame);NSString *key=@(fs->key),*identity=meshes&&*meshes?@(ms->key):key;NSNumber *previous=done[identity];
    if(previous&&(!passdump()||previous.unsignedLongLongValue==frame))return;
    id<MTLFunction> base=mtk_mesh_fn(ms,fs),function=mtk_geo_function(ms,fs,base);if(function==base)return;
    NSDictionary *layout=mtk_geo_layout(fn_src(base)?:sh_src(ms));if(!layout)return;
    unsigned nv=[layout[@"vertices"]unsignedIntValue],np=[layout[@"primitives"]unsignedIntValue],words=[layout[@"vertex_words"]unsignedIntValue],stride=[layout[@"record_words"]unsignedIntValue];
    const char *capacity_env=getenv("OOT_MESH_GEOMETRY_CAPACITY");
    unsigned requested=capacity_env?(unsigned)MAX(1,MIN(4096,atoi(capacity_env))):1024u;
    unsigned capacity=MIN(requested,((64u<<20)/4-8-np-1)/stride);size_t bytes=((size_t)8+capacity*stride+np+1)*4;
    id<MTLBuffer> buffer=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];if(!buffer)return;
    memset(buffer.contents,0,bytes);uint32_t header[8]={nv,np,words,0,capacity,stride,g_mtk.first,g_mtk.count};memcpy(buffer.contents,header,sizeof header);
    res_add(buffer);g_mtk_geometry=buffer;done[identity]=@(frame);
    NSString *stem=meshes&&*meshes?[NSString stringWithFormat:@"%@-F%llu-M%.16s-mesh-output",key,frame,ms->key]:[NSString stringWithFormat:@"%@-F%llu-mesh-output",key,frame];
    NSString *directory=mtk_geo_dir();
    NSString *path=[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".bin"]];
    [fn_src(function)writeToFile:[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".metal"]]atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSMutableDictionary *metadata=[layout mutableCopy];metadata[@"mesh"]=@(ms->key);metadata[@"fragment"]=key;metadata[@"frame"]=@(frame);metadata[@"header_words"]=@8;metadata[@"capacity"]=@(capacity);metadata[@"file"]=[path lastPathComponent];
    [g_commit_blocks addObject:^{const uint32_t *p=buffer.contents;unsigned saved=MIN(p[3],capacity);size_t length=((size_t)8+saved*stride)*4;
        BOOL ok=[[NSData dataWithBytes:p length:length]writeToFile:path atomically:YES];metadata[@"workgroups"]=@(p[3]);metadata[@"saved_workgroups"]=@(saved);
        NSMutableDictionary *hist=[NSMutableDictionary dictionary];for(unsigned i=0;i<=np;i++)if(p[8+capacity*stride+i])hist[[NSString stringWithFormat:@"%u",i]]=@(p[8+capacity*stride+i]);metadata[@"all_workgroup_primitive_counts"]=hist;
        [[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".json"]]atomically:YES];
        MLOG("[mesh-output] %s frame %llu: %u measured mesh entries (%u saved), %s",ok?"saved":"FAILED",frame,p[3],saved,path.UTF8String);}];
    defer_obj_release(buffer);
}
