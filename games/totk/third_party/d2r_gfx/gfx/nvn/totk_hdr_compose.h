/* Exact translated compose layout only; disabling the uniform executes original SDR math. */
#pragma once
#import <Foundation/Foundation.h>
#define TOTK_HDR_COMPOSE_KEY "0ac9a54938974396134d911dc0f29c79ce43e3d8cd50b11ee8b85a48ead6d2a5-"
static inline NSString *totk_hdr_compose_source(NSString *source) {
    NSString *anchor=@"    out.m_34.w = 0.0;", *param=@"[[buffer(0)]])";
    if(![source containsString:anchor] || ![source containsString:param] ||
       [source containsString:@"[[buffer(30)]]"] ||
       ![source containsString:@"float _69 = _63 + _57;"] ||
       ![source containsString:@"float _71 = _65 + _59;"] ||
       ![source containsString:@"float _73 = _67 + _61;"])return nil;
    NSString *body=@"    if (hdrEnabled > 0.5) {\n"
      "        float hdr_y = max(dot(max(float3(_69,_71,_73),float3(0)),float3(0.2126,0.7152,0.0722))-1.0,0.0);\n"
      "        float hdr_gain = 1.0 + 2.5 * hdr_y / (hdr_y + 2.0);\n"
      "        out.m_34.xyz = max(out.m_34.xyz,float3(0)) * hdr_gain;\n    }\n";
    NSString *s=[source stringByReplacingOccurrencesOfString:param withString:@"[[buffer(0)]], constant float& hdrEnabled [[buffer(30)]])"];
    return [s stringByReplacingOccurrencesOfString:anchor withString:[body stringByAppendingString:anchor]];
}
