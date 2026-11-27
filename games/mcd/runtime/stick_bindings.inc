/* Main queue owns persistence. Event/guest readers use atomic routing values.
   A separate file leaves existing button modes and keyboard bindings intact. */
static _Atomic int stickSources[RUNTIME_MAX_GAMEPADS][2];
static _Atomic int mouseStickTarget = 2;
static _Atomic int gyroSources[RUNTIME_MAX_GAMEPADS];
static int stickBindingsLoaded;
static void stick_bindings_load(void) {
    if (stickBindingsLoaded) return;
    stickBindingsLoaded=1;
    for(int s=0;s<RUNTIME_MAX_GAMEPADS;s++) { atomic_store(&stickSources[s][0],1);atomic_store(&stickSources[s][1],2);atomic_store(&gyroSources[s],0); }
    atomic_store(&mouseStickTarget,2);
    char path[4096];if(!setting_path("stick-bindings.plist",path,sizeof path))return;
    NSDictionary *d=[NSDictionary dictionaryWithContentsOfFile:@(path)];
    if(![d isKindOfClass:NSDictionary.class]||![d[@"version"] isEqual:@1])return;
    NSArray *ports=d[@"ports"];NSNumber *mouse=d[@"mouse"];
    if(![ports isKindOfClass:NSArray.class]||ports.count!=RUNTIME_MAX_GAMEPADS||
       ![mouse isKindOfClass:NSNumber.class]||mouse.doubleValue!=mouse.intValue||mouse.intValue<0||mouse.intValue>3)return;
    NSArray *gyro=d[@"gyro"];
    if(gyro) {
        if(![gyro isKindOfClass:NSArray.class]||gyro.count!=RUNTIME_MAX_GAMEPADS)return;
        for(int i=0;i<RUNTIME_MAX_GAMEPADS;i++) {
            id value=gyro[i];
            if(![value isKindOfClass:NSNumber.class]||[value doubleValue]!=[value intValue]||[value intValue]<0||[value intValue]>3||([value intValue]==2&&i!=0))return;
        }
    }
    for(id pair in ports) {
        if(![pair isKindOfClass:NSArray.class]||[pair count]!=2)return;
        for(id value in pair)if(![value isKindOfClass:NSNumber.class]||[value doubleValue]!=[value intValue]||[value intValue]<0||[value intValue]>2)return;
    }
    for(int s=0;s<RUNTIME_MAX_GAMEPADS;s++)for(int t=0;t<2;t++)atomic_store(&stickSources[s][t],[ports[s][t] intValue]);
    atomic_store(&mouseStickTarget,mouse.intValue);
    for(int i=0;i<RUNTIME_MAX_GAMEPADS;i++)atomic_store(&gyroSources[i],gyro?[gyro[i] intValue]:0);
}
static int stick_bindings_save(int slot,int target,int source,int mouse,int gyroSlot,int gyroSource) {
    NSMutableArray *ports=[NSMutableArray array];
    for(int s=0;s<RUNTIME_MAX_GAMEPADS;s++)[ports addObject:@[@(s==slot&&target==0?source:atomic_load(&stickSources[s][0])),
        @(s==slot&&target==1?source:atomic_load(&stickSources[s][1]))]];
    NSMutableArray *gyro=[NSMutableArray array];
    for(int s=0;s<RUNTIME_MAX_GAMEPADS;s++)[gyro addObject:@(s==gyroSlot?gyroSource:atomic_load(&gyroSources[s]))];
    char path[4096];
    if(!setting_path("stick-bindings.plist",path,sizeof path)||
       ![@{@"version":@1,@"ports":ports,@"mouse":@(mouse),@"gyro":gyro} writeToFile:@(path) atomically:YES])return 0;
    if(slot>=0)atomic_store(&stickSources[slot][target],source);
    atomic_store(&mouseStickTarget,mouse);
    if(gyroSlot>=0)atomic_store(&gyroSources[gyroSlot],gyroSource);
    runtime_input_clear();runtime_mouse_release();
    runtime_mouse_refresh_source();
    if(started)refresh_gamepads();
    return 1;
}
int runtime_input_stick_get(int slot,int target) {
    stick_bindings_load();return slot>=0&&slot<RUNTIME_MAX_GAMEPADS&&target>=0&&target<2?atomic_load(&stickSources[slot][target]):-1;
}
int runtime_input_stick_set(int slot,int target,int source) {
    stick_bindings_load();if(slot<0||slot>=RUNTIME_MAX_GAMEPADS||target<0||target>1||source<0||source>2)return 0;
    return stick_bindings_save(slot,target,source,atomic_load(&mouseStickTarget),-1,0);
}
int runtime_input_mouse_stick_get(void){stick_bindings_load();return atomic_load(&mouseStickTarget);}
int runtime_input_mouse_stick_set(int target){stick_bindings_load();return target>=0&&target<=3?stick_bindings_save(-1,0,0,target,-1,0):0;}
static void stick_bindings_apply(int slot,RuntimeGamepadState *state) {
    int32_t x[]={0,state->lx,state->rx},y[]={0,state->ly,state->ry};
    int left=atomic_load(&stickSources[slot][0]),right=atomic_load(&stickSources[slot][1]);
    state->lx=x[left];state->ly=y[left];state->rx=x[right];state->ry=y[right];
}

int runtime_input_gyro_get(int slot) {
    stick_bindings_load();return slot>=0&&slot<RUNTIME_MAX_GAMEPADS?atomic_load(&gyroSources[slot]):-1;
}
int runtime_input_gyro_set(int slot,int source) {
    stick_bindings_load();
    if(slot<0||slot>=INPUT_MOTION_PLAYERS||source<0||source>3||(source==2&&slot!=0))return 0;
    return stick_bindings_save(-1,0,0,atomic_load(&mouseStickTarget),slot,source);
}
int runtime_input_motion_allowed(int slot) {
    if(slot<0||slot>=RUNTIME_MAX_GAMEPADS||atomic_load(&inputSettingsSuspended))return 0;
    int source=atomic_load(&gyroSources[slot]);
    int mouse=slot==0&&runtime_input_source_allows(0,INPUT_DEVICE_MOUSE)&&runtime_mouse_enabled();
    const char *g=getenv("TOTK_MOUSE_GYRO");if(g&&!strcmp(g,"0"))mouse=0;
    if(source==3)return 0;
    if(source==2)return mouse;
    return (source==0&&mouse)||runtime_input_source_allows(slot,INPUT_DEVICE_PAD);
}
