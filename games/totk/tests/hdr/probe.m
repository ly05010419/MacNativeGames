/* Independent GPU checks of the production presentation shaders. No game data. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#include "totk_hdr.h"
#include "totk_hdr_present.h"
#include <stdio.h>
#include "probe-render.h"
int main(void){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();check(d!=nil,"Metal device");
 enum{N=8};__fp16 pixels[N][4]={{0,0,0,1},{.18,.18,.18,1},{1,1,1,1},{2,1,.5,1},{3.5,3.5,3.5,1},{100,1,0,1},{NAN,0,0,1},{INFINITY,1,0,1}};
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:N height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
 id<MTLTexture> input=[d newTextureWithDescriptor:td];[input replaceRegion:MTLRegionMake2D(0,0,N,1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];
 int checks=0;
 for(int h=1;h<=8;h++){
  NSData *out=render(d,input,MTLPixelFormatRGBA16Float,1,h,YES,nil);const __fp16 *v=out.bytes;
  for(int i=0;i<N;i++)for(int j=0;j<3;j++){check(isfinite((float)v[i*4+j]) && v[i*4+j]>=0 && v[i*4+j]<=h+0.005,"finite bounded EDR pixels");checks++;}
  check(fabsf((float)v[4]-.18f)<.001,"diffuse gray stays unchanged");check(v[8]==1,"UI/reference white remains 1");checks+=2;
  if(h>=4){check(fabsf((float)v[12]-2)<.002,"native highlight brightness passes through with headroom");checks++;}
  if(h>1){check(v[16]>1,"highlights survive above SDR white");checks++;}
 }
 NSData *sdr=render(d,input,MTLPixelFormatBGRA8Unorm,1,1,YES,nil);const uint8_t *v=sdr.bytes;check(abs(v[4]-118)<=1,"SDR screenshot encodes linear gray to sRGB");checks++;
 uint8_t ui[4]={118,118,118,255};td.pixelFormat=MTLPixelFormatRGBA8Unorm;td.width=1;id<MTLTexture> uit=[d newTextureWithDescriptor:td];[uit replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:ui bytesPerRow:4];
 NSData *edr=render(d,uit,MTLPixelFormatRGBA16Float,0,4,YES,nil);check(fabsf((float)((const __fp16*)edr.bytes)[0]-.181f)<.002,"SDR overlays decode once in EDR");checks++;
 NSData *unchanged=render(d,uit,MTLPixelFormatBGRA8Unorm,0,1,NO,nil);check(((const uint8_t*)unchanged.bytes)[0]==118,"ordinary SDR path preserves original bytes");checks++;
 /* Full byte ramps exercise the normal SDR source with the upstream compiler defaults. */
 uint8_t ramp[256][4];for(int i=0;i<256;i++){ramp[i][0]=i;ramp[i][1]=255-i;ramp[i][2]=(i*73)&255;ramp[i][3]=(i*19)&255;}
 td.width=256;id<MTLTexture> rt=[d newTextureWithDescriptor:td];[rt replaceRegion:MTLRegionMake2D(0,0,256,1) mipmapLevel:0 withBytes:ramp bytesPerRow:sizeof ramp];
 NSData *bytes=render(d,rt,MTLPixelFormatBGRA8Unorm,0,1,NO,nil);const uint8_t *rb=bytes.bytes;
 for(int i=0;i<256;i++)for(int c=0;c<4;c++){check(rb[i*4+c]==ramp[i][c<3?2-c:3],"ordinary SDR preserves all byte values and channels");checks++;}
 NSError *error=nil;id<MTLLibrary> plain=[d newLibraryWithSource:totk_hdr_present_source(MTLPixelFormatBGRA8Unorm,NO) options:nil error:&error];
 check(plain && [plain newFunctionWithName:@"fs"] && ![plain newFunctionWithName:@"fshdr"],"ordinary SDR has no HDR presentation function");checks++;
 for(int h=1;h<=8;h++){float last=0;for(int i=0;i<1000;i++){float p=i*.02f,q=totk_hdr_rolloff(p,h);check(q>=last-1e-6 && q<=h+1e-5 && isfinite(q),"monotone bounded headroom mapping");last=q;checks++;}}
 NSScreen *screen=NSScreen.mainScreen;
 printf("{\"passed\":true,\"checks\":%d,\"device\":\"%s\",\"current_edr_headroom\":%.3f,\"potential_edr_headroom\":%.3f}\n",checks,d.name.UTF8String,(double)screen.maximumExtendedDynamicRangeColorComponentValue,(double)screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
}}
