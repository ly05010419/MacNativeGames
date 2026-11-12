/* Bounded per-draw native mesh and fragment timestamps. Precise markers
 * serialize diagnostic work. Their durations are never normal-game FPS. */
#define MS_TIME_CAP 4096
typedef struct { uint64_t frame,pass,indirect;uint32_t first,count,threads;int grid;char mesh[128],task[128],fragment[128]; } ms_time;
#define MS_DRAWS_PER_HEAP 1024
static id<MTL4CounterHeap> g_ms_heaps[MS_TIME_CAP/MS_DRAWS_PER_HEAP];
static id<MTLBuffer> g_ms_grids;static int g_ms_grid_count;
static NSMutableArray<id<MTLBuffer>> *g_ms_grid_sources;
static size_t g_ms_grid_offsets[MS_TIME_CAP];
static int ms_grid_capture(uint64_t gpu);
static void ms_grid_flush(void);
static ms_time g_ms_times[MS_TIME_CAP];static int g_ms_count;
static void ms_time_stamp(id<MTL4RenderCommandEncoder> e,int index,int end) {
    if(index<0)return;
    for(int stage=0;stage<2;stage++){
        MTLRenderStages mask=stage?MTLRenderStageFragment:MTLRenderStageMesh;
        id<MTL4CounterHeap> heap=g_ms_heaps[index/MS_DRAWS_PER_HEAP];NSUInteger at=(NSUInteger)(index%MS_DRAWS_PER_HEAP)*4+stage*2+end;
        if(rs_using(e))rs_put((nvn_encode_packet){.op=RS_MESH_TIMESTAMP,.u={(uint64_t)(__bridge void *)heap,at,mask}},0);
        else [e writeTimestampWithGranularity:MTL4TimestampGranularityPrecise afterStage:mask intoHeap:heap atIndex:at];
    }
}
static int ms_time_begin(id<MTL4RenderCommandEncoder> e,const char *mesh,const char *task,const char *fragment,uint64_t indirect,uint32_t first,uint32_t count,uint32_t threads,int grid) {
    static int enabled=-1;if(enabled<0)enabled=getenv("OOT_MESH_GPU_FILE")!=NULL;
    if(!enabled||!g_pt_heap||g_ms_count==MS_TIME_CAP)return -1;
    int h=g_ms_count/MS_DRAWS_PER_HEAP;
    if(!g_ms_heaps[h]){MTL4CounterHeapDescriptor *d=[MTL4CounterHeapDescriptor new];d.type=MTL4CounterHeapTypeTimestamp;d.count=MS_DRAWS_PER_HEAP*4;NSError *error=nil;g_ms_heaps[h]=[g_dev newCounterHeapWithDescriptor:d error:&error];if(!g_ms_heaps[h])MDIE("mesh timing allocation: %s",error.localizedDescription.UTF8String);}
    int i=g_ms_count++;ms_time *r=&g_ms_times[i];*r=(ms_time){.frame=atomic_load(&g_cur_frame),.pass=g_stat_pass,.indirect=indirect,.first=first,.count=count,.threads=threads,.grid=grid};
    snprintf(r->mesh,sizeof r->mesh,"%s",mesh);snprintf(r->task,sizeof r->task,"%s",task?task:"-");snprintf(r->fragment,sizeof r->fragment,"%s",fragment?fragment:"-");ms_time_stamp(e,i,0);return i;
}
static void ms_time_finish(TotkCommitOptions *options,uint64_t fence) {
    if(!g_ms_count){g_ms_grids=nil;g_ms_grid_sources=nil;g_ms_grid_count=0;return;}
    int count=g_ms_count;ms_time *records=malloc((size_t)count*sizeof *records);memcpy(records,g_ms_times,(size_t)count*sizeof *records);
    NSMutableArray<id<MTL4CounterHeap>> *heaps=[NSMutableArray array];for(int i=0;i<(count+MS_DRAWS_PER_HEAP-1)/MS_DRAWS_PER_HEAP;i++){[heaps addObject:g_ms_heaps[i]];g_ms_heaps[i]=nil;}
    id<MTLBuffer> grids=g_ms_grids;g_ms_grids=nil;g_ms_grid_sources=nil;g_ms_count=g_ms_grid_count=0;double hz=(double)[g_dev queryTimestampFrequency];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback){
        NSMutableData *data=[NSMutableData data];for(NSUInteger h=0;h<heaps.count;h++){NSUInteger entries=MIN(MS_DRAWS_PER_HEAP,count-h*MS_DRAWS_PER_HEAP)*4;NSData *part=[heaps[h] resolveCounterRange:NSMakeRange(0,entries)];if(part)[data appendData:part];}
        if(feedback.error||data.length!=(NSUInteger)count*4*sizeof(MTL4TimestampHeapEntry)||hz<=0){MLOG("[mesh-gpu] invalid feedback/counters fence=%llu",(unsigned long long)fence);free(records);return;}
        const MTL4TimestampHeapEntry *t=data.bytes;const uint32_t *grid=grids.contents;FILE *f=fopen(getenv("OOT_MESH_GPU_FILE"),"a");
        if(f){for(int i=0;i<count;i++){
            const ms_time *r=&records[i];uint64_t a=t[4*i].timestamp,b=t[4*i+1].timestamp,c=t[4*i+2].timestamp,d=t[4*i+3].timestamp;
            fprintf(f,"%llu\t%llu\t%llu\t%d\t%s\t%s\t%s\t%.6f\t%.6f\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\n",(unsigned long long)r->frame,(unsigned long long)fence,(unsigned long long)r->pass,i,r->mesh,r->task,r->fragment,b>=a?(b-a)/hz*1e3:-1.,d>=c?(d-c)/hz*1e3:-1.,r->first,r->count,r->threads,(unsigned long long)r->indirect,(unsigned long long)a,(unsigned long long)b,(unsigned long long)c,(unsigned long long)d);
            if(r->grid>=0&&grid){const uint32_t *v=grid+r->grid*4;fprintf(f,"grid\t%llu\t%llu\t%d\t%u\t%u\t%u\t%u\n",(unsigned long long)r->frame,(unsigned long long)fence,i,v[0],v[1],v[2],v[3]);}
        }fclose(f);}free(records);
    }];
}
