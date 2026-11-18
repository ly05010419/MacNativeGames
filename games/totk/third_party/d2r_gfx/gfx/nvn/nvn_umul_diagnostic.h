/* Temporary Mac OOT characterization only. The production compiler fix belongs
   in SPIRV-Cross's typed OpUMulExtended emitter, after full-game validation. */
static int oot_umul_diagnostic_on(void) {
#if TARGET_OS_IPHONE
    return 0;
#else
    static int on=-1;if(on<0){const char *v=getenv("OOT_UMUL_INLINE");on=v&&atoi(v)!=0;}return on;
#endif
}
static NSString *oot_umul_diagnostic_source(NSString *source) {
    if(!oot_umul_diagnostic_on())return source;
    static NSRegularExpression *pattern;static dispatch_once_t once;
    dispatch_once(&once,^{pattern=[NSRegularExpression regularExpressionWithPattern:@"spvMulExtended<([A-Za-z_]\\w*), uint>" options:0 error:nil];});
    NSUInteger count=[pattern numberOfMatchesInString:source options:0 range:NSMakeRange(0,source.length)];if(!count)return source;
    NSString *marker=@"template<typename T, typename U, typename V>";NSRange insert=[source rangeOfString:marker];
    if(insert.location==NSNotFound){MLOG("[umul-diagnostic] no generic helper definition, source unchanged");return source;}
    NSString *modified=[pattern stringByReplacingMatchesInString:source options:0 range:NSMakeRange(0,source.length) withTemplate:@"ootMulExtendedU32<$1>"];
    NSString *helper=@"template<typename T> __attribute__((always_inline)) inline T ootMulExtendedU32(uint l,uint r) { return T{l*r,mulhi(l,r)}; }\ntemplate<typename T> inline T ootMulExtendedU32(int l,int r) { return spvMulExtended<T,uint>(l,r); }\n";
    NSRange where=[modified rangeOfString:marker];NSRange end=[modified rangeOfString:@"\n}\n" options:0 range:NSMakeRange(where.location,modified.length-where.location)];if(end.location==NSNotFound)return source;NSMutableString *out=[modified mutableCopy];[out insertString:helper atIndex:NSMaxRange(end)];
    unsigned char hash[32];NSData *bytes=[out dataUsingEncoding:NSUTF8StringEncoding];CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,hash);char hex[65];for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",hash[i]);
    MLOG("[umul-diagnostic] patched source=%s calls=%lu safe-math; normal/prebuilt pipeline archives excluded",hex,(unsigned long)count);return out;
}
