/* FXAA 3.11 Quality preset 39, Metal adaptation. NVIDIA CORPORATION, 2014, BSD-3-Clause-style license:
   ../../../fxaa/LICENSE.txt. Reference in GameTechDev/CMAA2 071c6b0, Projects/CMAA2/FXAA/Fxaa3_11.h.
   RGB luminance is computed in perceptual display values; alpha remains the original pixel's alpha. */
static const char *totk_fxaa_quality_msl =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct V { float4 p [[position]]; float2 uv; };\n"
"vertex V fxq_vs(uint id [[vertex_id]]) { float2 uv=float2((id<<1)&2,id&2); return {float4(uv*float2(2,-2)+float2(-1,1),0,1),uv}; }\n"
"static float luma(texture2d<float> t,sampler s,float2 uv,uint srgb) { float3 c=t.sample(s,uv).rgb; if(srgb)c=select(12.92*c,1.055*pow(max(c,0.0),float3(1.0/2.4))-.055,c>.0031308); return dot(c,float3(.299,.587,.114)); }\n"
"fragment float4 fxq_fs(V in [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]],constant uint& srgb [[buffer(0)]]) {\n"
" float2 uv=in.uv,px=1.0/float2(t.get_width(),t.get_height()); float4 original=t.sample(s,uv);\n"
" float M=luma(t,s,uv,srgb),N=luma(t,s,uv+px*float2(0,-1),srgb),S=luma(t,s,uv+px*float2(0,1),srgb),W=luma(t,s,uv+px*float2(-1,0),srgb),E=luma(t,s,uv+px*float2(1,0),srgb);\n"
" float hi=max(M,max(max(N,S),max(W,E))),lo=min(M,min(min(N,S),min(W,E))),range=hi-lo; if(range<max(.0312,.125*hi))return original;\n"
" float NW=luma(t,s,uv+px*float2(-1,-1),srgb),NE=luma(t,s,uv+px*float2(1,-1),srgb),SW=luma(t,s,uv+px*float2(-1,1),srgb),SE=luma(t,s,uv+px*float2(1,1),srgb);\n"
" float horizontal=abs(-2*M+N+S)*2+abs(-2*E+NE+SE)+abs(-2*W+NW+SW);\n"
" float vertical=abs(-2*M+W+E)*2+abs(-2*N+NW+NE)+abs(-2*S+SW+SE); bool h=horizontal>=vertical;\n"
" float sub=saturate(abs((2*(N+S+W+E)+NW+NE+SW+SE)/12.0-M)/range); sub=sub*sub*(3-2*sub); sub=sub*sub*.75;\n"
" float a=h?N:W,b=h?S:E; bool pairN=abs(a-M)>=abs(b-M); float gradient=max(abs(a-M),abs(b-M))*.25;\n"
" float local=(M+(pairN?a:b))*.5,sign=(h?px.y:px.x)*(pairN?-1.0:1.0);\n"
" float2 base=uv+(h?float2(0,.5*sign):float2(.5*sign,0)),stepUV=h?float2(px.x,0):float2(0,px.y);\n"
" float2 n=base-stepUV,p=base+stepUV; float en=0,ep=0; bool dn=false,dp=false;\n"
" constexpr float steps[12]={1,1,1,1,1,1.5,2,2,2,2,4,8};\n"
" for(uint i=0;i<11;i++){ if(!dn){en=luma(t,s,n,srgb)-local;dn=abs(en)>=gradient;} if(!dp){ep=luma(t,s,p,srgb)-local;dp=abs(ep)>=gradient;}\n"
" if(dn&&dp)break; if(!dn)n-=stepUV*steps[i+1]; if(!dp)p+=stepUV*steps[i+1]; }\n"
" float distN=h?uv.x-n.x:uv.y-n.y,distP=h?p.x-uv.x:p.y-uv.y; bool good=((distN<distP?en:ep)<0)!=((M-local)<0);\n"
" float offset=good?.5-min(distN,distP)/max(distN+distP,1e-6):0; offset=max(offset,sub);\n"
" float2 at=uv+(h?float2(0,offset*sign):float2(offset*sign,0));return float4(t.sample(s,at).rgb,original.a); }\n";
