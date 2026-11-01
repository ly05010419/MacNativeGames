/* Controlled GPU/ColorSync comparison. Optional args: SDR PNG, output prefix.
 * All samples are <= SDR white: identical tone mapping, only primaries differ.
 * No game data is required or written into the repository. Run separately on Mac. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include "totk_hdr_present.h"
#include "probe-render.h"

static void match(CGColorSpaceRef from,CGColorSpaceRef to,const CGFloat rgb[4],CGFloat out[4]){
 CGColorRef c=CGColorCreate(from,rgb),m=CGColorCreateCopyByMatchingToColorSpace(to,kCGRenderingIntentRelativeColorimetric,c,NULL);
 check(m!=NULL,"ColorSync conversion");const CGFloat *v=CGColorGetComponents(m);
 for(int i=0;i<3;i++){out[i]=v[i];check(isfinite(out[i]),"finite converted components");}out[3]=1;
 CGColorRelease(m);CGColorRelease(c);
}
static double distance(const CGFloat a[4],const CGFloat b[4]){return sqrt(pow(a[0]-b[0],2)+pow(a[1]-b[1],2)+pow(a[2]-b[2],2));}
static NSData *imagePixels(NSString *path,size_t *w,size_t *h){
 CGImageSourceRef src=CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],NULL);check(src!=NULL,"open optional SDR image");
 CGImageRef img=CGImageSourceCreateImageAtIndex(src,0,NULL);check(img!=NULL,"decode optional SDR image");*w=CGImageGetWidth(img);*h=CGImageGetHeight(img);
 NSMutableData *data=[NSMutableData dataWithLength:*w**h*4];CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
 CGContextRef ctx=CGBitmapContextCreate(data.mutableBytes,*w,*h,8,*w*4,cs,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);check(ctx!=NULL,"sRGB image context");
 CGContextDrawImage(ctx,CGRectMake(0,0,*w,*h),img);CGContextRelease(ctx);CGColorSpaceRelease(cs);CGImageRelease(img);CFRelease(src);return data;
}
static void chart(NSString *path,NSArray *colors,NSArray *names){
 const int w=1200,h=410;NSMutableData *data=[NSMutableData dataWithLength:w*h*4];CGColorSpaceRef p3=CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
 CGContextRef ctx=CGBitmapContextCreate(data.mutableBytes,w,h,8,w*4,p3,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);check(ctx!=NULL,"P3 chart context");
 CGContextSetRGBFillColor(ctx,.06,.06,.06,1);CGContextFillRect(ctx,CGRectMake(0,0,w,h));
 [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO]];
 NSDictionary *style=@{NSFontAttributeName:[NSFont systemFontOfSize:17],NSForegroundColorAttributeName:NSColor.whiteColor};
 [@"Same SDR values; identical brightness treatment. Compare colour only." drawAtPoint:NSMakePoint(22,h-32) withAttributes:style];
 for(int row=0;row<4;row++){
  [names[row] drawAtPoint:NSMakePoint(22,h-80-row*80) withAttributes:style];
  for(int col=0;col<8;col++){NSArray *v=colors[row*8+col];CGContextSetRGBFillColor(ctx,[v[0] doubleValue],[v[1] doubleValue],[v[2] doubleValue],1);CGContextFillRect(ctx,CGRectMake(370+col*100,h-108-row*80,92,62));}
 }
 [@"Display P3-tagged SDR chart; not an HDR brightness measurement." drawAtPoint:NSMakePoint(22,10) withAttributes:style];
 [NSGraphicsContext restoreGraphicsState];CGImageRef img=CGBitmapContextCreateImage(ctx);
 CGImageDestinationRef dest=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],CFSTR("public.png"),1,NULL);check(dest!=NULL,"chart destination");CGImageDestinationAddImage(dest,img,NULL);check(CGImageDestinationFinalize(dest),"write tagged PNG");
 CFRelease(dest);CGImageRelease(img);CGContextRelease(ctx);CGColorSpaceRelease(p3);
}
int main(int argc,const char **argv){@autoreleasepool{
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();check(d!=nil,"Metal device");
 enum{GRID=729,NEUTRAL=256,PATCHES=8};uint8_t patches[PATCHES][4]={{230,40,30,255},{35,210,50,255},{40,80,230,255},{230,145,40,255},{65,135,65,255},{200,145,115,255},{118,118,118,255},{255,255,255,255}};
 NSMutableData *pixels=[NSMutableData data];[pixels appendBytes:patches length:sizeof patches];
 for(int r=0;r<=8;r++)for(int g=0;g<=8;g++)for(int b=0;b<=8;b++){uint8_t v[4]={(r*255+4)/8,(g*255+4)/8,(b*255+4)/8,255};[pixels appendBytes:v length:4];}
 for(int i=0;i<NEUTRAL;i++){uint8_t v[4]={i,i,i,255};[pixels appendBytes:v length:4];}
 if(argc>1){size_t w,h;NSData *image=imagePixels(@(argv[1]),&w,&h);const uint8_t *v=image.bytes;for(int y=0;y<32;y++)for(int x=0;x<32;x++){size_t i=(((y*2+1)*h/64)*w+(x*2+1)*w/64)*4;[pixels appendBytes:v+i length:4];}}
 size_t n=pixels.length/4;MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:n height:1 mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
 id<MTLTexture> input=[d newTextureWithDescriptor:td];[input replaceRegion:MTLRegionMake2D(0,0,n,1) mipmapLevel:0 withBytes:pixels.bytes bytesPerRow:pixels.length];
 NSData *sdr=render(d,input,MTLPixelFormatBGRA8Unorm,0,4,NO,nil),*linear=render(d,input,MTLPixelFormatRGBA16Float,0,4,YES,nil);
 /* Exercise the scene path too: <=1 must pass through without altering grading. */
 td.pixelFormat=MTLPixelFormatRGBA16Float;id<MTLTexture> scene=[d newTextureWithDescriptor:td];
 [scene replaceRegion:MTLRegionMake2D(0,0,n,1) mipmapLevel:0 withBytes:linear.bytes bytesPerRow:linear.length];
 check([render(d,scene,MTLPixelFormatRGBA16Float,1,4,YES,nil) isEqualToData:linear],"scene and SDR fallback agree below reference white");
 /* W3C CSS Color 4: XYZ_to_lin_P3 * lin_sRGB_to_XYZ (D65).
  * https://www.w3.org/TR/css-color-4/#color-conversion-code
  * Matrix exists only in this comparison, not in the production shader. */
 NSString *source=totk_hdr_present_source(MTLPixelFormatRGBA16Float,YES);
 NSString *matrix=@"float3 hdr_to_p3(float3 c){return float3(dot(c,float3(.822461968714,.177538031286,0)),dot(c,float3(.033194198851,.966805801149,0)),dot(c,float3(.017082630721,.072397440664,.910519928615)));}\n";
 source=[source stringByReplacingOccurrencesOfString:@"struct Crop" withString:[matrix stringByAppendingString:@"struct Crop"]];
 check([source containsString:@"float4(hdr_decode(v.rgb),v.a)"],"comparison source anchor");
 source=[source stringByReplacingOccurrencesOfString:@"float4(hdr_decode(v.rgb),v.a)" withString:@"float4(hdr_to_p3(hdr_decode(v.rgb)),v.a)"];
 NSData *converted=render(d,input,MTLPixelFormatRGBA16Float,0,4,YES,source);
 CGColorSpaceRef srgb=CGColorSpaceCreateWithName(kCGColorSpaceSRGB),ls=CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB),lp=CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3),lab=CGColorSpaceCreateWithName(kCGColorSpaceGenericLab),p3=CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
 const uint8_t *original=pixels.bytes,*ref=sdr.bytes;const __fp16 *a=linear.bytes,*b=converted.bytes;
 NSMutableArray *groups=[NSMutableArray array],*swatches=[NSMutableArray array];
 for(int row=0;row<4;row++)for(int i=0;i<PATCHES;i++){
  CGFloat v[4]={0,0,0,1},out[4];CGColorSpaceRef cs=srgb;
  for(int c=0;c<3;c++)v[c]=row==0?ref[i*4+2-c]/255.:row==2?b[i*4+c]:a[i*4+c];
  if(row)cs=row==1?ls:lp;match(cs,p3,v,out);[swatches addObject:@[@(out[0]),@(out[1]),@(out[2])]];
 }
 size_t starts[]={PATCHES,PATCHES+GRID,PATCHES+GRID+NEUTRAL},ends[]={PATCHES+GRID,PATCHES+GRID+NEUTRAL,n};
 for(int group=0;group<3;group++){
  if(starts[group]==ends[group])continue;double sum[3]={0},maximum[3]={0};
  for(size_t i=starts[group];i<ends[group];i++){
   CGFloat rgb[4]={0,0,0,1},baseline[4],values[3][4]={{0,0,0,1},{0,0,0,1},{0,0,0,1}};
   for(int c=0;c<3;c++){check(ref[i*4+2-c]==original[i*4+c],"SDR byte identity");rgb[c]=ref[i*4+2-c]/255.;values[0][c]=values[2][c]=a[i*4+c];values[1][c]=b[i*4+c];}
   match(srgb,lab,rgb,baseline);
   for(int mode=0;mode<3;mode++){CGFloat result[4];match(mode==0?ls:lp,lab,values[mode],result);double delta=distance(baseline,result);sum[mode]+=delta;maximum[mode]=fmax(maximum[mode],delta);}
  }
  NSMutableArray *m=[NSMutableArray array];for(int mode=0;mode<3;mode++)[m addObject:@{@"mean_delta_e76":@(sum[mode]/(ends[group]-starts[group])),@"max_delta_e76":@(maximum[mode])}];
  [groups addObject:@{@"group":@[@"9x9x9 RGB cube",@"256 neutral grays",@"1024 sampled SDR gameplay pixels"][group],@"samples":@(ends[group]-starts[group]),@"results":m}];
  check(maximum[0]<.15 && maximum[1]<.15,"correctly labelled/converted colors preserve the SDR reference");
  if(group==0)check(sum[2]/GRID>2,"direct P3 interpretation measurably changes colors");
 }
 /* Above-white brightness is independent of the color-space tag. */
 __fp16 highlight[4]={2,2,2,1};td.width=1;td.pixelFormat=MTLPixelFormatRGBA16Float;input=[d newTextureWithDescriptor:td];[input replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:highlight bytesPerRow:8];
 NSData *edr=render(d,input,MTLPixelFormatRGBA16Float,1,4,YES,nil);check(((const __fp16*)edr.bytes)[0]==2,"extended sRGB retains 2x reference white");
 NSDictionary *report=@{@"passed":@YES,@"device":d.name,@"metric":@"CIE76 delta E in ColorSync Generic Lab, relative colorimetric",@"modes":@[@"extended linear sRGB",@"linear sRGB converted to linear P3",@"linear sRGB directly labelled P3"],@"groups":groups,@"highlight_sample":@2,@"notes":@"GPU production presentation source. Identical samples <= 1 isolate gamut from highlight reconstruction. SDR PNG is optional; no meter measurements or HDR screenshot comparison."};
 NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
 if(argc>2){NSString *prefix=@(argv[2]);check([json writeToFile:[prefix stringByAppendingString:@".json"] atomically:YES],"write report");chart([prefix stringByAppendingString:@".png"],swatches,@[@"Original SDR reference",@"Extended linear sRGB",@"Converted linear P3",@"Direct P3 (unconverted)"]);}
 for(int i=0;i<5;i++)CGColorSpaceRelease((CGColorSpaceRef[]){srgb,ls,lp,lab,p3}[i]);
}}
