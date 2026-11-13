#include "nvn_pool_sources.h"
typedef struct { uint64_t sources; int ok, masked, floating; } proof;
static NSArray<NSTextCheckingResult *> *matches(NSString *s, NSString *p) {
    NSRegularExpression *r=[NSRegularExpression regularExpressionWithPattern:p options:0 error:NULL];
    return [r matchesInString:s options:0 range:NSMakeRange(0,s.length)];
}
static NSString *part(NSString *s,NSTextCheckingResult *m,int i) {return [s substringWithRange:[m rangeAtIndex:i]];}
static NSString *trim(NSString *s) {return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];}
static NSString *inside(NSString *s,NSString *prefix) {
    if(![s hasPrefix:prefix]||![s hasSuffix:@")"])return nil;
    NSInteger depth=1;
    for(NSUInteger i=prefix.length;i<s.length;i++){
        unichar c=[s characterAtIndex:i];if(c=='(')depth++;else if(c==')')depth--;
        if(!depth)return i==s.length-1?[s substringWithRange:NSMakeRange(prefix.length,i-prefix.length)]:nil;
    }
    return nil;
}
static void reject_escaped_locals(NSString *src,NSMutableDictionary *defs) {
    // Passing a scalar lvalue to an unknown helper may bind an inout reference.
    // Reject that local, not every pool merely because an unrelated helper has
    // a mutable parameter. Expressions/casts are rvalues and cannot bind one.
    for(NSTextCheckingResult *m in matches(src,@"\\b([A-Za-z_]\\w*(?:<[^;\\n{}()]*>)?)\\s*\\(")){
        NSString *name=part(src,m,1);
        if([@[@"int",@"uint",@"float",@"min",@"if",@"while",@"switch",@"as_type<int>",@"as_type<uint>",@"as_type<float>"] containsObject:name])continue;
        NSUInteger start=NSMaxRange(m.range);int level=1;
        for(NSUInteger i=start;i<src.length;i++){
            unichar c=[src characterAtIndex:i];if(c=='('||c=='['||c=='{')level++;else if(c==')'||c==']'||c=='}')level--;
            if((c==','&&level==1)||!level){NSString *s=trim([src substringWithRange:NSMakeRange(start,i-start)]),*v;
                while((v=inside(s,@"(")))s=trim(v);
                if(matches(s,@"^_[0-9]+$").count)defs[s]=@[@"mutation"];
                start=i+1;
            }
            if(!level)break;
        }
    }
}
/* code / argv (2026-11-08): the operation as a number and the operands as a C array, fixed when the node is made. The
   replay evaluates these per draw (prepare_shader_pools); comparing op strings there was about 5 % of the replay thread. */
enum { PE_LITERAL, PE_UNIFORM, PE_UNION, PE_CAST, PE_NEG, PE_ADD, PE_SUB, PE_MUL, PE_AND, PE_OR, PE_XOR, PE_SHL, PE_SHR, PE_OTHER };
@interface PoolExpr : NSObject { @public NSString *op; int type, binding, code, argc; uint32_t value; NSArray<PoolExpr *> *args; __unsafe_unretained PoolExpr **argv; } @end
@implementation PoolExpr - (void)dealloc { free(argv); } @end
static int pe_code(NSString *op) {
    static NSDictionary<NSString *, NSNumber *> *codes; static dispatch_once_t once;
    dispatch_once(&once, ^{ codes = @{@"literal":@(PE_LITERAL), @"uniform":@(PE_UNIFORM), @"union":@(PE_UNION), @"cast":@(PE_CAST), @"neg":@(PE_NEG),
        @"+":@(PE_ADD), @"-":@(PE_SUB), @"*":@(PE_MUL), @"&":@(PE_AND), @"|":@(PE_OR), @"^":@(PE_XOR), @"<<":@(PE_SHL), @">>":@(PE_SHR)}; });
    NSNumber *c = op ? codes[op] : nil; return c ? c.intValue : PE_OTHER;
}
static PoolExpr *node(NSString *op,int type,uint32_t value,NSArray *args) {
    PoolExpr *p=[PoolExpr new];p->op=op;p->type=type;p->value=value;p->args=args;p->code=pe_code(op);
    p->argc=(int)args.count;if(p->argc){p->argv=(__unsafe_unretained PoolExpr **)calloc((size_t)p->argc,sizeof *p->argv);if(!p->argv)abort();for(int i=0;i<p->argc;i++)p->argv[i]=args[i];}   // args keeps them alive
    return p;
}
static NSDictionary *pool_uniforms(const nvn_refl *rf,NSArray *bindings,NSString *src) {
    NSMutableDictionary *out=[NSMutableDictionary dictionary];
    for(int i=0;i<rf->nb;i++)if(rf->b[i].kind==NVN_RB_CBUF&&rf->b[i].reg<16){
        NSDictionary *b=bindings[i];NSArray *m=matches(b[@"msl_type"]?:@"",@"^constant (\\w+)\\*$");if(m.count!=1)continue;
        NSString *pat=[NSString stringWithFormat:@"struct %@\\s*\\{\\s*float4 _m0\\[([0-9]+)\\];\\s*\\};",part(b[@"msl_type"],m[0],1)];
        NSArray *layout=matches(src,pat);if(layout.count==1)out[b[@"msl_name"]]=@[@(i),@(part(src,layout[0],1).intValue)];
    }
    return out;
}
/* A bounded interpreter for the address expression, never for the shader.
   All possible branch assignments are unioned. Unknown varying loads, aliases,
   floating arithmetic or excessive value sets keep the whole-buffer fallback. */
static PoolExpr *pool_expr(NSString *expr,NSDictionary *defs,NSDictionary *types,NSDictionary *uniforms,int depth) {
    if(depth>64)return nil;
    NSString *s=trim(expr),*v=inside(s,@"(");if(v)return pool_expr(v,defs,types,uniforms,depth+1);
    for(NSString *cast in @[@"as_type<uint>(",@"as_type<int>(",@"as_type<float>(",@"uint(",@"int("]){
        v=inside(s,cast);if(!v)continue;PoolExpr *p=pool_expr(v,defs,types,uniforms,depth+1);
        if(!p||(![cast hasPrefix:@"as_type"]&&p->type==2))return nil;
        return node(@"cast",[cast containsString:@"float"]?2:[cast containsString:@"uint"]?0:1,0,@[p]);
    }
    NSArray *m=matches(s,@"^(0x[0-9a-fA-F]+|[0-9]+)(u?)$");
    if(m.count){NSString *digits=part(s,m[0],1);char *end=NULL;unsigned long long n=strtoull(digits.UTF8String,&end,0);
        if(*end||n>UINT32_MAX||(n>INT32_MAX&&!part(s,m[0],2).length&&![digits hasPrefix:@"0x"]))return nil; // reject invalid/wider signed literals
        return node(@"literal",part(s,m[0],2).length||n>INT32_MAX?0:1,(uint32_t)n,nil);}
    m=matches(s,@"^\\(\\*spvDescriptorSet0\\.(\\w+)\\)\\._m0\\[([0-9]+)\\]\\.([xyzw])$");
    if(m.count){NSArray *u=uniforms[part(s,m[0],1)];uint32_t ix=(uint32_t)part(s,m[0],2).intValue;
        if(!u||ix>=[u[1] unsignedIntValue])return nil;
        PoolExpr *p=node(@"uniform",2,ix*16+(uint32_t)[@"xyzw" rangeOfString:part(s,m[0],3)].location*4,nil);p->binding=[u[0] intValue];return p;
    }
    // Ignore the condition but retain both outcomes; nested top-level ternaries
    // are handled recursively and cannot omit a possible source index.
    int level=0,nested=0;NSInteger question=-1,colon=-1;
    for(NSUInteger i=0;i<s.length;i++){unichar c=[s characterAtIndex:i];if(c=='('||c=='[')level++;else if(c==')'||c==']')level--;else if(!level){if(c=='?'){if(question<0)question=(NSInteger)i;else nested++;}else if(c==':'&&question>=0){if(!nested){colon=(NSInteger)i;break;}nested--;}}}
    if(colon>=0){PoolExpr *a=pool_expr([s substringWithRange:NSMakeRange(question+1,colon-question-1)],defs,types,uniforms,depth+1),*b=pool_expr([s substringFromIndex:colon+1],defs,types,uniforms,depth+1);if(!a||!b||a->type==2||b->type==2)return nil;return node(@"union",a->type&&b->type,0,@[a,b]);}
    for(NSArray *group in @[@[@" | "],@[@" ^ "],@[@" & "],@[@" << ",@" >> "],@[@" + ",@" - "],@[@" * "]]){
        NSInteger at=-1;NSString *found=nil;level=0;
        for(NSUInteger i=0;i<s.length;i++){unichar c=[s characterAtIndex:i];if(c=='('||c=='[')level++;else if(c==')'||c==']')level--;if(level)continue;
            for(NSString *token in group)if(i+token.length<=s.length&&[[s substringWithRange:NSMakeRange(i,token.length)] isEqualToString:token]){at=(NSInteger)i;found=token;}}
        if(found){PoolExpr *a=pool_expr([s substringToIndex:at],defs,types,uniforms,depth+1),*b=pool_expr([s substringFromIndex:at+found.length],defs,types,uniforms,depth+1);if(!a||!b||a->type==2||b->type==2)return nil;
            NSString *op=trim(found);return node(op,[op isEqual:@">>"]||[op isEqual:@"<<"]?a->type:a->type&&b->type,0,@[a,b]);}
    }
    if([s hasPrefix:@"-"]){PoolExpr *p=pool_expr([s substringFromIndex:1],defs,types,uniforms,depth+1);return p&&p->type!=2?node(@"neg",p->type,0,@[p]):nil;}
    NSArray *ds=defs[s];NSString *type=types[s];if(!ds.count||!type||[type isEqual:@"duplicate"])return nil;
    NSMutableArray *ps=[NSMutableArray array];int ty=[type isEqual:@"float"]?2:[type isEqual:@"uint"]?0:1;
    for(NSString *d in ds){PoolExpr *p=pool_expr(d,defs,types,uniforms,depth+1);if(!p||(p->type==2)!=(ty==2))return nil;[ps addObject:p];}
    return node(@"union",ty,0,ps);
}
typedef struct {uint32_t v[64];int n;} pool_values;
static int value_add(pool_values *v,uint32_t n){for(int i=0;i<v->n;i++)if(v->v[i]==n)return 1;if(v->n==64)return 0;v->v[v->n++]=n;return 1;}
static int eval_index(PoolExpr *p,nvn_pool_uniform_read read,void *context,pool_values *out) {
    switch(p->code){
    case PE_LITERAL: return value_add(out,p->value);
    case PE_UNIFORM: {uint32_t v;return read(context,p->binding,p->value,&v)&&value_add(out,v);}
    case PE_UNION: for(int i=0;i<p->argc;i++)if(!eval_index(p->argv[i],read,context,out))return 0;return 1;
    case PE_OTHER: return 0;
    default: break;
    }
    if(p->argc<1)return 0;
    pool_values a={0},b={0};if(!eval_index(p->argv[0],read,context,&a))return 0;
    if(p->code==PE_CAST||p->code==PE_NEG){for(int i=0;i<a.n;i++)if(!value_add(out,p->code==PE_NEG?0u-a.v[i]:a.v[i]))return 0;return 1;}
    if(p->argc<2||!eval_index(p->argv[1],read,context,&b))return 0;
    for(int i=0;i<a.n;i++)for(int j=0;j<b.n;j++){uint32_t x=a.v[i],y=b.v[j],v;
        switch(p->code){
        case PE_ADD: v=x+y;break; case PE_SUB: v=x-y;break; case PE_MUL: v=x*y;break;
        case PE_AND: v=x&y;break; case PE_OR: v=x|y;break; case PE_XOR: v=x^y;break;
        case PE_SHL: if(y>=32)return 0;v=x<<y;break;
        case PE_SHR: if(y>=32)return 0;v=p->type?(uint32_t)((int32_t)x>>y):x>>y;break;
        default: return 0;
        }
        if(!value_add(out,v))return 0;
    }
    return 1;
}
int nvn_pool_index_values(NSArray *programs,nvn_pool_uniform_read read,void *context,uint32_t *values,int capacity) {
    if(!programs.count)return -1;pool_values out={0};
    for(PoolExpr *p in programs)if(!eval_index(p,read,context,&out))return -1;
    if(out.n>capacity)return -1;memcpy(values,out.v,out.n*sizeof *values);return out.n;
}
/* A deliberately small proof language, not an MSL evaluator. Accept handle
   loads, bit-preserving casts, the NVN low-20 mask and redundant clamps.
   All branch assignments must have proved origins; cycles, arithmetic, mutable
   references and unrecognised syntax fail closed. */
static proof trace(NSString *expr,NSDictionary *defs,NSDictionary *types,NSDictionary *loads,NSMutableDictionary *reads,int depth) {
    proof bad={0};if(depth>64)return bad;
    NSString *s=trim(expr),*inner;
    if((inner=inside(s,@"(")))return trace(inner,defs,types,loads,reads,depth+1);
    for(NSString *cast in @[@"as_type<uint>(",@"as_type<int>(",@"as_type<float>(",@"uint(",@"int("]){
        if(!(inner=inside(s,cast)))continue;
        proof p=trace(inner,defs,types,loads,reads,depth+1);
        if(!p.ok||(![cast hasPrefix:@"as_type"]&&p.floating))return bad;
        p.floating=[cast isEqualToString:@"as_type<float>("];return p;
    }
    NSArray *m=matches(s,@"^(.+) & 1048575u?$");
    if(m.count){proof p=trace(part(s,m[0],1),defs,types,loads,reads,depth+1);if(p.floating)return bad;p.masked=1;return p;}
    m=matches(s,@"^min\\((.+), 1048575u\\)$");
    if(m.count){proof p=trace(part(s,m[0],1),defs,types,loads,reads,depth+1);return p.masked&&!p.floating?p:bad;}
    m=matches(s,@"^\\(\\*spvDescriptorSet0\\.(\\w+)\\)\\._m0\\[([^\\[\\]]+)\\]$");
    if(m.count){NSNumber *n=loads[part(s,m[0],1)];if(n){NSMutableArray *a=reads[n];if(!a)reads[n]=a=[NSMutableArray array];NSString *index=part(s,m[0],2);if(![a containsObject:index])[a addObject:index];return (proof){n.unsignedLongLongValue,1,0,0};}return bad;}
    if([s isEqualToString:@"0"]||[s isEqualToString:@"0u"])return (proof){0,1,1,0};
    if([s isEqualToString:@"0.0"])return (proof){0,1,1,1};
    NSArray *d=defs[s];NSString *type=types[s];
    if(!d.count||!type||[type isEqualToString:@"duplicate"])return bad;
    proof all={0,1,1,[type isEqualToString:@"float"]};
    for(NSString *value in d){
        proof p=trace(value,defs,types,loads,reads,depth+1);
        if(!p.ok||p.floating!=all.floating)return bad;
        all.sources|=p.sources;all.masked&=p.masked;
    }
    return all;
}
NSDictionary *nvn_pool_sources(nvn_refl *rf,NSArray *bindings,NSString *src) {
    for(int i=0;i<rf->nb;i++)rf->b[i].pool_sources=0;
    if(bindings.count!=(NSUInteger)rf->nb)return @{};
    /* Only a pooled texture read "(&spvDescriptorSet0.<name>->e[0])[min(" has anything to prove (the reads pattern
       below starts with that text): without one in the source the result is empty, so the scans of the whole source
       are skipped. They were the shader load's cost (2026-11-13, iPad A16 CPU report: about 30 of 31 samples of
       shader_load_mode in this function's regexes, matching rather than compiling them: 4.16 against 0.02 ms per
       shader on an M4); 2,304 of Ocarina of Time's 18,661 iOS shaders have such a read. */
    BOOL pooled=NO;
    for(int i=0;i<rf->nb&&!pooled;i++){
        if(rf->b[i].kind!=NVN_RB_TEXP)continue;
        NSString *name=[bindings[i] isKindOfClass:NSDictionary.class]?bindings[i][@"msl_name"]:nil;if(![name isKindOfClass:NSString.class]||!name.length)continue;
        pooled=src&&[src rangeOfString:[NSString stringWithFormat:@"(&spvDescriptorSet0.%@->e[0])[min(",name]].location!=NSNotFound;
    }
    if(!pooled)return @{};
    // A bound mutable alias needs interprocedural analysis. Helper parameters
    // alone are harmless; direct lvalue arguments are rejected below instead.
    if(matches(src,@"(?:thread|device)\\s+(?!const\\b)[^;\\n{}]*&\\s*\\w+\\s*=").count||[src containsString:@"const_cast"]||[src containsString:@"[&"])return @{};
    NSMutableDictionary *defs=[NSMutableDictionary dictionary],*types=[NSMutableDictionary dictionary],*loads=[NSMutableDictionary dictionary];
    for(NSTextCheckingResult *m in matches(src,@"\\b(int|uint|float)\\s+(_[0-9]+)\\b")){
        NSString *n=part(src,m,2);types[n]=types[n]?@"duplicate":part(src,m,1);
    }
    for(NSTextCheckingResult *m in matches(src,@"\\b(_[0-9]+)\\s*([+*/%&|^<>!-]*=)\\s*([^;]*);")){
        NSString *n=part(src,m,1);NSMutableArray *a=defs[n];if(!a)defs[n]=a=[NSMutableArray array];
        [a addObject:[part(src,m,2) isEqualToString:@"="]?part(src,m,3):@"mutation"];
    }
    for(NSTextCheckingResult *m in matches(src,@"(?:\\+\\+|--|&)\\s*(_[0-9]+)\\b|\\b(_[0-9]+)\\s*(?:\\+\\+|--)")){
        for(int j=1;j<=2;j++)if([m rangeAtIndex:j].location!=NSNotFound)defs[part(src,m,j)]=@[@"mutation"];
    }
    reject_escaped_locals(src,defs);
    for(int i=0;i<rf->nb;i++){
        const nvn_rbind *b=&rf->b[i];NSDictionary *d=bindings[i];
        if(b->kind!=NVN_RB_SBUFP||b->rw)continue;
        NSString *name=d[@"msl_name"],*type=d[@"msl_type"];
        NSArray *m=matches(type?:@"",@"^const device (\\w+)\\*$");if(m.count!=1)continue;
        NSString *pat=[NSString stringWithFormat:@"struct %@\\s*\\{\\s*uint _m0\\[1\\];\\s*\\};",part(type,m[0],1)];
        if(matches(src,pat).count==1)loads[name]=@(UINT64_C(1)<<i);
    }
    NSMutableDictionary *result=[NSMutableDictionary dictionary];
    NSDictionary *uniforms=pool_uniforms(rf,bindings,src);
    for(int i=0;i<rf->nb;i++){
        nvn_rbind *b=&rf->b[i];if(b->kind!=NVN_RB_TEXP)continue;
        NSString *name=bindings[i][@"msl_name"];if(!name.length)continue;
        NSString *e=[NSRegularExpression escapedPatternForString:name];
        NSString *pat=[NSString stringWithFormat:@"\\(&spvDescriptorSet0\\.%@->e\\[0\\]\\)\\[min\\((.+?), spvDescriptorSet0\\.%@->n\\)\\]",e,e];
        NSArray *reads=matches(src,pat);
        if(!reads.count||matches(src,[@"\\b" stringByAppendingString:[e stringByAppendingString:@"\\b"]]).count!=1+2*reads.count)continue;
        uint64_t all=0;int valid=1;NSMutableDictionary *origins=[NSMutableDictionary dictionary];
        for(NSTextCheckingResult *m in reads){proof p=trace(part(src,m,1),defs,types,loads,origins,0);if(!p.ok||!p.masked||p.floating){valid=0;break;}all|=p.sources;}
        if(valid){
            b->pool_sources=all;NSMutableDictionary *indices=[NSMutableDictionary dictionary];
            for(NSNumber *source in origins){NSMutableArray *programs=[NSMutableArray array];
                for(NSString *expr in origins[source]){PoolExpr *p=pool_expr(expr,defs,types,uniforms,0);if(!p){[programs removeAllObjects];break;}[programs addObject:p];}
                if(programs.count)indices[@(__builtin_ctzll(source.unsignedLongLongValue))]=programs;
            }
            result[@(b->aid)]=indices;
        }
    }
    return result;
}
