/* Breath of the Wild 1.9.0's effects page (PR #1602; included once by settings_panel.h, in place of the other versions' list,
   after its row helpers). The rows stand in Tears of the Kingdom 1.4.3's order, so both games' pages read the same; a row
   whose code is not there for this game shows its name greyed, "Not in this version".
   The effect switches are totk_fx.h's (dm_gfx_fx_*), saved as on 1.4.3: graphics-effects.txt holds the ids of the
   effects that are off, one word each ("none" when all are on). On this game dm_gfx_fx_available opens only the switches
   that act here: ambient occlusion, the AO patch's own switch (totk_ssao.h, c1[3].w bit 0 in the shader 0eeb23f2), while the AO
   shaders are the patched ones. The sun's shadows have their row (settings_botw_shadow_row), as on 1.4.3. */
int dm_gfx_fx_count(void); const char *dm_gfx_fx_id(int i); const char *dm_gfx_fx_name(int i, int english);   /* totk_fx.h */
int dm_gfx_fx_available(int i); int dm_gfx_fx_on(int i); void dm_gfx_fx_set(int i, int on);

static void settings_effects_save(void) {
 char line[512]="";size_t n=0;
 for(int i=0;i<dm_gfx_fx_count();i++)if(!dm_gfx_fx_on(i))n+=(size_t)snprintf(line+n,sizeof line-n,"%s%s",n?" ":"",dm_gfx_fx_id(i));
 runtime_setting_save("graphics-effects.txt",n?line:"none","effects");
}
/* Before the guest starts (settings_restore_begin): an id this game cannot switch off is left as it is (dm_gfx_fx_set
   does nothing for it). */
static void settings_effects_restore(void) {
 char line[512];
 if(runtime_setting_word("graphics-effects.txt",line,sizeof line))
  for(char *w=strtok(line," \t\r\n");w;w=strtok(NULL," \t\r\n"))
   for(int i=0;i<dm_gfx_fx_count();i++)if(!strcmp(w,dm_gfx_fx_id(i)))dm_gfx_fx_set(i,0);
}
/* The page's "Reset to defaults": every switch on and its file removed. */
static void settings_botw_effects_reset(void) {
 for(int i=0;i<dm_gfx_fx_count();i++)if(!dm_gfx_fx_on(i))dm_gfx_fx_set(i,1);
 runtime_setting_remove("graphics-effects.txt");
}
static NSArray<TOTKSettingRow *> *settings_effect_rows(void) {
 NSString *none=settings_text("这个版本没有","Not in this version");
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray arrayWithObjects:settings_botw_fov_row(),
  settings_fixed(settings_text("模型细节距离（LOD）","Model detail distance (LOD)"),none),settings_fixed(settings_text("渲染距离","Render distance"),none),
  settings_botw_shadow_row(),settings_fixed(settings_text("FXAA 抗锯齿","FXAA anti-aliasing"),none),
  settings_fixed(settings_text("景深","Depth of field"),none),settings_fixed(settings_text("锐化","Sharpening"),none),nil];
 for(int i=0;i<dm_gfx_fx_count();i++){
  if(!strcmp(dm_gfx_fx_id(i),"shadow"))continue;   /* in the shadow resolution row */
  NSString *t=@(TOTK_UI(dm_gfx_fx_name(i,0),dm_gfx_fx_name(i,1)));
  if(!dm_gfx_fx_available(i)){[rows addObject:settings_fixed(t,none)];continue;}
  [rows addObject:settings_row(t,nil,nil,^int{return dm_gfx_fx_on(i);},^(int v){dm_gfx_fx_set(i,v);settings_effects_save();})];
 }
 return rows;
}
