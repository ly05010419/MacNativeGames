// Present crop regression: colored 8×8 texture, closed-form RGBA expectations; real Metal rendering, no dependence on the game or on cross-run determinism.
// nvn_mtl.m's nvn_game_fps is a weak default and hle_nvn.c gives the strong definition; in one translation unit that is a redefinition, so the weak one is renamed (this test does not look at the frame-rate target)
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle   // both files have a static g_ft (nvn_mtl.m's fragment argument table, hle_nvn.c's textures a flush can mark CPU-written), so hle_nvn.c's is renamed too
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int g_layer_crop[4];   // what the vi layer crop (hle_vi.c) answers: x, y, w, h, all 0 = none
void dm_vi_layer_crop(int c[4]) { memcpy(c, g_layer_crop, sizeof g_layer_crop); }
#ifdef CROP_LEGACY_HEADER
#include CROP_LEGACY_HEADER
#define ENCODE legacy_present_encode
#else
#define ENCODE present_encode
#endif
static int failed, cases;
#define EXPECT(n, w, v) do { int a=(w),b=(v); cases++; printf("%s expected %d → got %d %s\n",n,a,b,a==b?"✓":"✗");failed+=a!=b; } while(0)
static const uint8_t colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};

#ifndef CROP_LEGACY_HEADER
// Exercise the persistent scaler used by real presents, completing each command buffer before its allocator and output leave scope.
static int check_present_resize(id<MTLTexture> src, int w, int h) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> out=[g_dev newTextureWithDescriptor:d];res_add(out);res_commit();
    id<MTL4CommandAllocator> al=[g_dev newCommandAllocator];id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];id<MTLSharedEvent> ev=[g_dev newSharedEvent];
    [cb beginCommandBufferWithAllocator:al];int kind=present_encode_m4(cb,src,out,NULL,g_present_uv[0],&g_fx_present);[cb endCommandBuffer];
    [g_q4 commit:&cb count:1];[g_q4 signalEvent:ev value:1];
    EXPECT("resize present completes",1,[ev waitUntilSignaledValue:1 timeoutMS:10000]);res_remove(out);return kind;
}
#endif

static void check(id<MTLTexture> src, const char *name, const int crop[4], int w, int h, int color) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> dst=[g_dev newTextureWithDescriptor:d];id<MTLCommandBuffer> cb=[g_q commandBuffer];
    EXPECT("encode succeeds",1,ENCODE(cb,src,dst,crop));[cb commit];[cb waitUntilCompleted];
    EXPECT("GPU done",MTLCommandBufferStatusCompleted,cb.status);
    uint8_t *out=malloc(w*h*4);[dst getBytes:out bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    int bad=0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        int q=color;
        if(q==4) { // square valid area onto a 2:1 target: 4 black columns on each side, the middle 8 columns all red.
            static const uint8_t black[4]={0,0,0,255};
            const uint8_t *want=x<4||x>=12?black:colors[0];bad+=memcmp(out+4*(y*w+x),want,4)!=0;continue;
        }
        if(q==-2) { // crop=(1,1,4,4): 3×3 red, last column green, last row blue, bottom-right 1 cell yellow.
            q=(x+1>=4)+2*(y+1>=4);
        } else if(q<0) { // same-size default whole frame: the boundary is dead center.
            q=(x>=w/2)+2*(y>=h/2);
        }
        bad+=memcmp(out+4*(y*w+x),colors[q],4)!=0;
    }
    EXPECT(name,0,bad);free(out);
}

int main(void) { @autoreleasepool {
    mtl_init();
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> src=[g_dev newTextureWithDescriptor:d];uint8_t px[8*8*4];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)memcpy(px+4*(y*8+x),colors[(x>=4)+2*(y>=4)],4);
    [src replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:px bytesPerRow:32];
    int zero[4]={0},tl[4]={0,0,4,4},tr[4]={4,0,4,4},bl[4]={0,4,4,4},br[4]={4,4,4,4},one[4]={3,3,1,1},inset[4]={1,1,4,4};
    check(src,"default whole frame 64 pixels",zero,8,8,-1);
    check(src,"NULL default whole frame 64 pixels",NULL,8,8,-1);
    check(src,"top-left crop scaled up, 256 pixels all red",tl,16,16,0);
    check(src,"non-zero x crop scaled up, 256 pixels all green",tr,16,16,1);
    check(src,"non-zero y crop scaled up, 256 pixels all blue",bl,16,16,2);
    check(src,"bottom-right crop at the edge, 256 pixels all yellow",br,16,16,3);
    check(src,"single texel scaled up without crossing edges, 256 pixels all red",one,16,16,0);
    check(src,"non-zero x and y offset, 16 pixels byte-identical to a hand-written sub-rectangle",inset,4,4,-2);
    check(src,"aspect of the valid area kept, 128 pixels red in the middle / black on both sides",tl,16,8,4);
#ifndef CROP_LEGACY_HEADER
    {
        // sRGB swapchain: present sends the raw bytes (as the Switch's scan-out does) without decoding first; unfixed, 128/64/200 decode to about 55/13/148
        MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:8 height:8 mipmapped:NO];sd.storageMode=MTLStorageModeShared;sd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> s8=[g_dev newTextureWithDescriptor:sd];uint8_t g[8*8*4];
        for(int i=0;i<64;i++){g[4*i]=128;g[4*i+1]=64;g[4*i+2]=200;g[4*i+3]=255;}
        [s8 replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:g bytesPerRow:32];
        MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:od];id<MTLCommandBuffer> cb=[g_q commandBuffer];
        EXPECT("sRGB source encode succeeds",1,ENCODE(cb,s8,dst,zero));[cb commit];[cb waitUntilCompleted];
        uint8_t o[8*8*4];[dst getBytes:o bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        int bad=0;for(int i=0;i<64;i++)bad+=abs(o[4*i]-128)>1||abs(o[4*i+1]-64)>1||abs(o[4*i+2]-200)>1;
        EXPECT("sRGB swapchain present sends raw bytes, 64 pixels",0,bad);
    }
#endif
#ifndef CROP_LEGACY_HEADER
    {   // DM_PRESENT_UPSCALE: which content MetalFX scales, and to which size (the picture's rectangle on the canvas)
        const struct { unsigned cw,ch,dw,dh,w,h; int upscale; } sizes[] = {
            {1280,720,2868,1320,2347,1320,1},   // handheld, letterboxed on an iPhone
            {1952,900,2868,1320,2868,1320,1},   // ratios within 1 % fill the canvas
            {1280,720,2560,1440,2560,1440,1},   // Retina window
            {1280,720,1280,720,1280,720,0},     // native size
            {1600,900,1280,720,1280,720,0},     // downscale
            {8,8,16,8,8,8,0},                  // larger canvas, native-size picture
        };
        for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
            MTLViewport vp=present_viewport(sizes[i].cw,sizes[i].ch,sizes[i].dw,sizes[i].dh);
            EXPECT("MetalFX only upscales the picture",sizes[i].upscale,fx_wanted(vp,sizes[i].cw,sizes[i].ch));
            EXPECT("picture width",sizes[i].w,(int)vp.width);EXPECT("picture height",sizes[i].h,(int)vp.height);
        }
        EXPECT("empty content is not upscaled",0,fx_wanted((MTLViewport){0,0,2868,1320,0,1},0,720));
    }
    if([MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) {   // the present pass through MetalFX into an offscreen canvas (present_offscreen_m4, as DM_NVN_SHOT_OUT uses it)
        g_fx_mode=1;
        const int N=64;MTLTextureDescriptor *bd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:N height:N mipmapped:NO];bd.storageMode=MTLStorageModeShared;bd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> big=[g_dev newTextureWithDescriptor:bd];uint8_t *bp=malloc(N*N*4);
        for(int y=0;y<N;y++)for(int x=0;x<N;x++)memcpy(bp+4*(y*N+x),colors[(x>=N/2)+2*(y>=N/2)],4);
        for(int y=N/2;y<N;y++)for(int x=0;x<N/2;x++){uint8_t g[4]={128,64,200,255};memcpy(bp+4*(y*N+x),g,4);}   // lower-left quadrant: a mid grey-violet the sRGB bytes must keep
        [big replaceRegion:MTLRegionMake2D(0,0,N,N) mipmapLevel:0 withBytes:bp bytesPerRow:N*4];free(bp);
        res_add(big);res_commit();
        const int W=4*N,H=2*N;MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        od.storageMode=MTLStorageModePrivate;od.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;id<MTLTexture> out=[g_dev newTextureWithDescriptor:od];
        const int crops[4][4]={{0},{8,8,48,48},{8,8,48,48},{8,8,48,48}};
        for (int mode=0;mode<4;mode++) {
            g_fx_mode=mode==3?0:1;atomic_store(&g_origin_ll,mode==2);
            EXPECT("offscreen present reports the scaler actually used",mode==3?PRESENT_STANDARD:PRESENT_METALFX,present_offscreen_wait(big,crops[mode],out));
            id<MTLBuffer> rb=[g_dev newBufferWithLength:W*H*4 options:MTLResourceStorageModeShared];id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
            [bl copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:W*4 destinationBytesPerImage:W*H*4];
            [bl endEncoding];[cb commit];[cb waitUntilCompleted];
            const uint8_t *o=rb.contents;   // BGRA
        #define PX(x,y,c) o[4*((y)*W+(x))+(c)]
            int bars=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++)if(x<64||x>=192)bars+=PX(x,y,0)||PX(x,y,1)||PX(x,y,2);
            EXPECT("  black bars beside the square picture, 0 lit pixels",0,bars);
            int bad=0;const int cxs[2]={64+32,64+96},cys[2]={32,96};
            for(int q=0;q<4;q++){int x=cxs[q&1],y=cys[q>>1],sq=mode==2?q^2:q;const uint8_t *want=sq==2?(const uint8_t[4]){128,64,200,255}:colors[sq];
                bad+=abs(PX(x,y,2)-want[0])>3||abs(PX(x,y,1)-want[1])>3||abs(PX(x,y,0)-want[2])>3;}
            EXPECT("  quadrant centres keep their colours, raw bytes of the sRGB source (4 checked)",0,bad);
        #undef PX
        }
        atomic_store(&g_origin_ll,0);g_fx_mode=1;
        MTLTextureDescriptor *smallD=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:NO];
        smallD.storageMode=MTLStorageModePrivate;smallD.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> small=[g_dev newTextureWithDescriptor:smallD];int whole[4]={0},invalidCrop[4]={63,0,2,2};
        EXPECT("MetalFX requested but downscaling uses standard",PRESENT_STANDARD,present_offscreen_wait(big,whole,small));
        EXPECT("offscreen rejects an invalid crop before submission",PRESENT_FAILED,present_offscreen_wait(big,invalidCrop,out));

        uint64_t savedPresent=g_present_n;g_present_n=1;
        EXPECT("initial present makes its scaler",PRESENT_METALFX,check_present_resize(big,W,H));
        id<MTL4FXSpatialScaler> first=g_fx_present.sc;
        g_present_n=2;
        EXPECT("resizing within 30 presents falls back to standard",PRESENT_STANDARD,check_present_resize(big,192,96));
        EXPECT("resizing keeps the previous scaler until replacement",1,g_fx_present.sc==first);
        g_present_n=31;
        EXPECT("stable new size makes its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("replacement has the new picture size",96,(int)g_fx_present.out.width);
        EXPECT("exactly two scalers made",2,g_fx_present.made);
        g_present_n=32;
        EXPECT("same size reuses its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("reuse makes no new scaler",2,g_fx_present.made);
        g_present_n=savedPresent;

        // Whole-window capture uses the same upscale and remains asynchronous; its completion includes the final BGRA readback.
        dispatch_semaphore_t grabbed=dispatch_semaphore_create(0);
        mtl_grab_next_present(^(CGImageRef img) {
            EXPECT("whole-window capture produces an image",1,img!=NULL);
            if (img) {
                EXPECT("whole-window image width",W,(int)CGImageGetWidth(img));
                CFDataRef data=CGDataProviderCopyData(CGImageGetDataProvider(img));const uint8_t *p=CFDataGetBytePtr(data);
                size_t off=96*CGImageGetBytesPerRow(img)+96*4;
                EXPECT("whole-window capture preserves sRGB bytes",1,abs(p[off]-200)<=3&&abs(p[off+1]-64)<=3&&abs(p[off+2]-128)<=3);
                CFRelease(data);
            }
            dispatch_semaphore_signal(grabbed);
        });
        grab_present(big,whole,CGSizeMake(W,H));
        EXPECT("whole-window capture callback completes",0,(int)dispatch_semaphore_wait(grabbed,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)));
        res_remove(big);g_fx_mode=0;
    } else printf("no Metal 4 MetalFX spatial scaler on this GPU: the MetalFX present is not checked\n");
#endif
    int invalid[4][4]={{-1,0,4,4},{0,-1,4,4},{0,0,0,4},{7,0,4,4}};
    for(int i=0;i<4;i++) {
        MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];dd.usage=MTLTextureUsageRenderTarget;dd.storageMode=MTLStorageModeShared;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:dd];
        EXPECT("invalid crop is not encoded and does not fake success by stretching the whole image",0,ENCODE([g_q commandBuffer],src,dst,invalid[i]));
    }
    // Real QueuePresent entry: set crop and enqueue first, then immediately change the window; the present ticket must keep that frame's copy.
    PFNNVNDEVICEGETPROCADDRESSPROC get=(PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNT_nvnWindowSetCrop set=(NVNT_nvnWindowSetCrop)get(NULL,"nvnWindowSetCrop");
    NVNT_nvnQueuePresentTexture present=(NVNT_nvnQueuePresentTexture)get(NULL,"nvnQueuePresentTexture");
    tex_t tex={.magic=MAGIC('T', 'E', 'X', 0), .serial =10001};tex.d.width=tex.d.height=8;tex.d.depth=tex.d.levels=1;tex.d.format=NVN_FORMAT_RGBA8;tex.d.target=NVN_TEXTURE_TARGET_2D;serial_put(tex.serial,src);
    win_t win={.ntex=1};win.tex[0]=(NVNtexture *)&tex;set((NVNwindow *)&win,4,4,4,4);
    mtl_present_test_stall(100000000);present(NULL,(NVNwindow *)&win,0);uint64_t tk=win.ticket[0];set((NVNwindow *)&win,0,0,8,8);
#ifdef CROP_LEGACY_HEADER
    EXPECT("old queue struct has no room for a four-value copy (struct check)",1,sizeof(pq_item)>=sizeof(tex_t)+sizeof(double)+sizeof(uint64_t)+sizeof br);
#else
    pthread_mutex_lock(&g_pq_mu);if(!tk)tk=g_pq_in;   // a present snapshot (DM_PRESENT_SNAP; with the encode thread too) hands back no ticket: the frame is the last one queued
    int saved=memcmp(g_pq[(tk-1)%PQ].crop,br,sizeof br)==0;pthread_mutex_unlock(&g_pq_mu);
    EXPECT("real QueuePresent: the frame's crop copy does not change with the window",1,saved);
#endif
    mtl_present_wait(tk);mtl_present_test_stall(0);
#ifndef CROP_LEGACY_HEADER
    {   // A window without a crop of its own presents by the vi layer crop (Link's Awakening in handheld mode: 1280x720 of its 1920x1080 window textures, cropped through vi::SetLayerCrop)
        int none[4]={0},lay[4]={0,4,4,4},over[4]={0,0,16,16},lc[4];
        memset(g_layer_crop,0,sizeof g_layer_crop);EXPECT("no window or layer crop: the whole texture",1,present_crop(none,8,8,lc)==none);
        memcpy(g_layer_crop,lay,sizeof lay);const int *r=present_crop(none,8,8,lc);EXPECT("no window crop: the layer crop",1,r==lc&&!memcmp(lc,lay,sizeof lay));
        EXPECT("the window's own crop wins over the layer crop",1,present_crop(br,8,8,lc)==br);
        memcpy(g_layer_crop,over,sizeof over);EXPECT("a layer crop larger than the texture: the whole texture",1,present_crop(none,8,8,lc)==none);
        memcpy(g_layer_crop,lay,sizeof lay);win_t w2={.ntex=1};w2.tex[0]=(NVNtexture *)&tex;
        mtl_present_test_stall(100000000);present(NULL,(NVNwindow *)&w2,0);uint64_t t2=w2.ticket[0];memset(g_layer_crop,0,sizeof g_layer_crop);
        pthread_mutex_lock(&g_pq_mu);if(!t2)t2=g_pq_in;int got=memcmp(g_pq[(t2-1)%PQ].crop,lay,sizeof lay)==0;pthread_mutex_unlock(&g_pq_mu);
        EXPECT("real QueuePresent: a window without a crop queues the layer crop",1,got);
        mtl_present_wait(t2);mtl_present_test_stall(0);
    }
#endif
    printf("== %d checks, %d failed ==\n",cases,failed);return failed!=0;
} }
