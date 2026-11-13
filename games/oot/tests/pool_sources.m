#import <Foundation/Foundation.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_pool_sources.h"
#include <assert.h>

static NSString *fixture(NSString *statements,NSString *index) {
    return [NSString stringWithFormat:@"struct Words { uint _m0[1]; };\nstruct Args { constant Pool* textures; };\n%@\nfloat x = (&spvDescriptorSet0.textures->e[0])[min(uint(%@), spvDescriptorSet0.textures->n)].read(uint2(0)).x;",statements,index];
}
static uint64_t check(NSString *source) {
    nvn_refl r={.nb=2};r.b[0].kind=NVN_RB_SBUFP;r.b[1].kind=NVN_RB_TEXP;
    nvn_pool_sources(&r,@[@{@"msl_name":@"data",@"msl_type":@"const device Words*"},@{@"msl_name":@"textures"}],source);
    return r.b[1].pool_sources;
}
static int uniform_read(void *context,int binding,uint32_t offset,uint32_t *value) {
    if(!context||binding!=2||offset>=32)return 0;*value=((uint32_t *)context)[offset/4];return 1;
}
static NSArray *index_program(NSString *index,NSString *statements) {
    nvn_refl r={.nb=3};r.b[0].kind=NVN_RB_SBUFP;r.b[1].kind=NVN_RB_TEXP;r.b[1].aid=7;r.b[2].kind=NVN_RB_CBUF;
    NSString *s=[NSString stringWithFormat:@"struct Uniform { float4 _m0[2]; };\n%@\nuint _90 = %@;\nfloat _1 = as_type<float>((*spvDescriptorSet0.data)._m0[_90]);\nint _2 = as_type<int>(_1) & 1048575;",statements,index];
    NSDictionary *p=nvn_pool_sources(&r,@[@{@"msl_name":@"data",@"msl_type":@"const device Words*"},@{@"msl_name":@"textures"},@{@"msl_name":@"cb",@"msl_type":@"constant Uniform*"}],fixture(s,@"_2"));
    assert(r.b[1].pool_sources==1);return p[@7][@0];
}
static int has_value(const uint32_t *values,int n,uint32_t value) {for(int i=0;i<n;i++)if(values[i]==value)return 1;return 0;}
int main(int argc,const char **argv) { @autoreleasepool {
    NSString *base=@"float _1 = as_type<float>((*spvDescriptorSet0.data)._m0[int(_999)]);\nint _2 = as_type<int>(_1) & 1048575;\nuint _3 = min(uint(_2), 1048575u);";
    int checks=0;assert(check(fixture(base,@"int(_3)"))==1);checks++;
    for(NSString *mutation in @[@"_1 = 1.0;",@"_2 += 1;",@"_2 -= 1;",@"++_2;",@"_2++;",@"thread int& ref = _2;",@"int _2 = 42;"]){assert(!check(fixture([base stringByAppendingString:mutation],@"int(_3)")));checks++;}
    for(NSString *replacement in @[@"((*spvDescriptorSet0.other)._m0[0])",@"((*spvDescriptorSet0.data)._m0[0] + (*spvDescriptorSet0.other)._m0[0])",@"((*spvDescriptorSet0.data)._m0[0] + 1u)"]){NSString *s=[base stringByReplacingOccurrencesOfString:@"(*spvDescriptorSet0.data)._m0[int(_999)]" withString:replacement];assert(!check(fixture(s,@"int(_3)")));checks++;}
    assert(!check(fixture(base,@"int(_3) + 1")));checks++;
    assert(!check(fixture([base stringByReplacingOccurrencesOfString:@"as_type<int>(_1)" withString:@"int(_1)"],@"int(_3)")));checks++;
    assert(!check(fixture(@"uint _1 = (*spvDescriptorSet0.data)._m0[0]; uint _3 = min(_1, 1048575u);",@"_3")));checks++;
    NSString *branch=@"float _1 = 0.0; if (condition) { _1 = as_type<float>((*spvDescriptorSet0.data)._m0[9]); } int _2 = as_type<int>(_1) & 1048575;";
    assert(check(fixture(branch,@"_2"))==1);checks++;
    assert(!check(fixture([branch stringByAppendingString:@"_1 = as_type<float>((*spvDescriptorSet0.unknown)._m0[2]);"],@"_2")));checks++;
    assert(!check(fixture([branch stringByAppendingString:@"_1 = _1;"],@"_2")));checks++;
    assert(check([@"template<class T> void read_depth(thread const depth2d<T> & t) {}\n" stringByAppendingString:fixture(base,@"int(_3)")])==1);checks++;
    assert(!check([@"const_cast<uint&>(x);\n" stringByAppendingString:fixture(base,@"int(_3)")]));checks++;
    assert(check([@"void unrelated(thread int& _999) { _999 += 2; }\n" stringByAppendingString:fixture(base,@"int(_3)")])==1);checks++;
    for(NSString *call in @[@"mutate(_2);",@"mutate(0, ((_2)));",@"mutate<int>(_2);",@"mutate(as_type<thread int&>(_2));"]){assert(!check(fixture([base stringByAppendingString:call],@"int(_3)")));checks++;}
    NSString *address=@"int _10 = as_type<int>((*spvDescriptorSet0.cb)._m0[0].z) & 1048575; int _11 = _10 + as_type<int>((*spvDescriptorSet0.cb)._m0[1].w); int _12 = condition ? _11 : 0; int _13 = (_12 << 5) + 24; uint _14 = uint(int(uint(_13) >> uint(2)));";
    NSArray *program=index_program(@"int(_14)",address);assert(program.count);checks++;
    uint32_t uniform[8]={0},values[64];
    for(uint32_t i=0;i<1000;i++){uniform[2]=0x80000000u+i*8179u;uniform[7]=i*7;int n=nvn_pool_index_values(program,uniform_read,uniform,values,64);
        assert(n>0&&has_value(values,n,((uniform[2]&1048575u)+uniform[7])*8u+6u)&&has_value(values,n,6));checks++;}
    assert(nvn_pool_index_values(program,uniform_read,NULL,values,64)==-1);checks++;
    assert(!index_program(@"int((*spvDescriptorSet0.cb)._m0[0].z)",@"").count);checks++;
    assert(!index_program(@"varying_index",@"").count);checks++;
    program=index_program(@"uint((as_type<int>((*spvDescriptorSet0.cb)._m0[0].z) >> 3) + 1)",@"");uniform[2]=0xfffffff0u;
    int count=nvn_pool_index_values(program,uniform_read,uniform,values,64);assert(count==1&&values[0]==UINT32_MAX);checks++;
    assert(nvn_pool_index_values(index_program(@"uint(1) << 32",@""),uniform_read,uniform,values,64)==-1);checks++;
    assert(!index_program(@"2147483648 >> 1",@"").count);checks++;
    /* No pooled texture read: an empty result straight away (2026-11-13: the scans of the whole source are skipped), and
       a read of another pool's name does not count as this one's. */
    {nvn_refl r={.nb=2};r.b[0].kind=NVN_RB_SBUFP;r.b[1].kind=NVN_RB_TEXP;r.b[1].pool_sources=5;
     NSArray *b=@[@{@"msl_name":@"data",@"msl_type":@"const device Words*"},@{@"msl_name":@"textures"}];
     NSString *none=[base stringByAppendingString:@"\nfloat x = spvDescriptorSet0.other.read(uint2(0)).x;"];
     assert(!nvn_pool_sources(&r,b,none).count&&!r.b[1].pool_sources);checks++;
     NSString *other=[fixture(base,@"int(_3)") stringByReplacingOccurrencesOfString:@"spvDescriptorSet0.textures" withString:@"spvDescriptorSet0.texturesX"];
     assert(!nvn_pool_sources(&r,b,other).count&&!r.b[1].pool_sources);checks++;
     assert(nvn_pool_sources(&r,b,fixture(base,@"int(_3)")).count==1&&r.b[1].pool_sources==1);checks++;}
    printf("pool-sources: PASS %d proof and rejection checks\n",checks);
    if(argc>1){NSUInteger shaders=0,pools=0,proven=0;NSString *root=@(argv[1]);
        for(NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:NULL]){@autoreleasepool {
            if(![f hasSuffix:@"_b.json"])continue;
            NSDictionary *j=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:f]] options:0 error:NULL];
            NSArray *bs=j[@"bindings"];if(bs.count>64)continue;nvn_refl rf={.nb=(int)bs.count};
            for(NSUInteger k=0;k<bs.count;k++){NSString *kind=bs[k][@"kind"];rf.b[k].kind=[kind isEqual:@"texpool"]?NVN_RB_TEXP:[kind isEqual:@"sbuf"]?NVN_RB_SBUFP:0;rf.b[k].rw=[bs[k][@"rw"] boolValue];rf.b[k].aid=(int)k;if([kind isEqual:@"cbuf"])rf.b[k].kind=NVN_RB_CBUF;}
            NSString *src=[NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:[[f stringByDeletingPathExtension] stringByAppendingPathExtension:@"metal"]] encoding:NSUTF8StringEncoding error:NULL];if(!src)continue;
            NSDictionary *programs=nvn_pool_sources(&rf,bs,src);shaders++; if(argc>2&&[f hasPrefix:@(argv[2])])for(int k=0;k<rf.nb;k++)if(rf.b[k].kind==NVN_RB_TEXP)printf("%s %s sources=%llx index-inputs=%lu\n",f.UTF8String,[bs[k][@"msl_name"] UTF8String],(unsigned long long)rf.b[k].pool_sources,(unsigned long)[programs[@(rf.b[k].aid)] count]);
            for(int k=0;k<rf.nb;k++)if(rf.b[k].kind==NVN_RB_TEXP){pools++;proven+=rf.b[k].pool_sources!=0;}
        }}printf("corpus: shaders=%lu pools=%lu proven=%lu\n",shaders,pools,proven);
    }
    return 0;
}}
