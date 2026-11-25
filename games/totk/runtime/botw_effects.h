/* Breath of the Wild 1.9.0's effects page (PR #1602; included once by settings_panel.h, in place of the other versions' list,
   after its row helpers). The rows stand in Tears of the Kingdom 1.4.3's order, so both games' pages read the same; a row
   whose code is not there for this game shows its name greyed, "Not in this version".
   The effect switches are totk_fx.h's (dm_gfx_fx_*), saved as on 1.4.3: graphics-effects.txt holds the ids of the
   effects that are off, one word each ("none" when all are on). On this game dm_gfx_fx_available opens only the switches
   that act here: ambient occlusion, the AO patch's own switch (totk_ssao.h, c1[3].w bit 0 in the shader 0eeb23f2), while the AO
   shaders are the patched ones, the grass blades (their own fragment / vertex shader pair in totk_fx.h), and the sun's
   shadows, which are an Off choice in their row (settings_botw_shadow_row), as on 1.4.3.
   Anti-aliasing and sharpening are 1.4.3's rows and files (graphics-aa-method.txt, graphics-fxaa-quality.txt,
   graphics-smaa-quality.txt; an older graphics-fxaa.txt "off" still reads as Off; graphics-sharpen.txt) acting around
   this game's FXAA / upscale draw (gfx/nvn/botw_finish.h); while MetalFX replaces that draw the anti-aliasing rows say
   so instead. */
int dm_gfx_fx_count(void); const char *dm_gfx_fx_id(int i); const char *dm_gfx_fx_name(int i, int english);   /* totk_fx.h */
int dm_gfx_fx_available(int i); int dm_gfx_fx_on(int i); void dm_gfx_fx_set(int i, int on);
void dm_gfx_fxaa_set(int on); int dm_gfx_fxaa_on(void); void dm_gfx_sharpen_set(int pct); int dm_gfx_sharpen(void);   /* gfx/nvn/totk_finish.h */
int dm_gfx_botw_fxaa_replaced(void);   /* gfx/nvn/botw_finish.h */

static void settings_aa_save(void) {   /* as 1.4.3's (settings_game.h) */
 settings_save_int("graphics-fxaa-quality.txt",dm_gfx_fxaa_quality(),"FXAA quality");
 settings_save_int("graphics-smaa-quality.txt",dm_gfx_smaa_quality(),"SMAA quality");
 settings_save_int("graphics-aa-method.txt",dm_gfx_aa_method(),"anti-aliasing method");
}
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
 char word[16];double pct;
 /* As 1.4.3 reads them: the older FXAA switch first, then the method, which decides when its file is there. */
 if(runtime_setting_word("graphics-fxaa.txt",word,sizeof word)&&!strncmp(word,"off",3))dm_gfx_fxaa_set(0);
 int method,sq;
 if(settings_read_int("graphics-aa-method.txt",&method)&&method>=0&&method<=2){
  if(settings_read_int("graphics-fxaa-quality.txt",&sq)&&sq>=0&&sq<=1)dm_gfx_fxaa_quality_set(sq);
  if(settings_read_int("graphics-smaa-quality.txt",&sq)&&sq>=0&&sq<=3)dm_gfx_smaa_quality_set(sq);
  dm_gfx_aa_method_set(method);
 }
 if(settings_read_number("graphics-sharpen.txt",0,100,&pct))dm_gfx_sharpen_set((int)pct);   /* as 1.4.3 reads it */
}
/* The page's "Reset to defaults": every switch on, the game's FXAA (1.4.3's defaults: FXAA Original, SMAA High), no
   sharpening, and their files removed. */
static void settings_botw_effects_reset(void) {
 for(int i=0;i<dm_gfx_fx_count();i++)if(!dm_gfx_fx_on(i))dm_gfx_fx_set(i,1);
 dm_gfx_fxaa_quality_set(0);dm_gfx_aa_method_set(1);dm_gfx_smaa_quality_set(2);dm_gfx_sharpen_set(0);
 for(const char *const *f=(const char *const[]){"graphics-effects.txt","graphics-fxaa.txt","graphics-aa-method.txt","graphics-fxaa-quality.txt",
  "graphics-smaa-quality.txt","graphics-sharpen.txt",NULL};*f;f++)runtime_setting_remove(*f);
}
/* The sun shadows' row (settings_botw_shadow_row in settings_panel.h names it): off, the game's sizes or 2048 per cascade.
   The sizes are the next launch's (botw_shadow_res.h); off is the effect switch "shadow" and acts at once (P9, 2026-11-05:
   on the Great Plateau the shadows of Link, rocks and cliffs go, with no other evident fault in that scene; AO and other conditions not checked). Without that switch, the sizes only. */
static TOTKSettingRow *settings_botw190_shadow_row(NSString *t) {
 if(!runtime_shadow_res_available())return settings_fixed(t,settings_text("这个版本没有","Not in this version"));
 if(runtime_shadow_res_locked())return settings_fixed(t,settings_text("由 TOTK_SHADOW_RES 设置","Set by TOTK_SHADOW_RES"));
 t=[t stringByAppendingString:settings_text("（下次启动生效）"," (from the next launch)")];
 /* Off (the effect switch "shadow", totk_fx.h: every shadow-map comparison reads lit) acts at once and is saved in
    graphics-effects.txt; the resolution keeps its own file and its next-launch rule. -1 = off in this row. */
 int fx=-1;for(int i=0;i<dm_gfx_fx_count();i++)if(!strcmp(dm_gfx_fx_id(i),"shadow"))fx=i;
 if(fx<0||!dm_gfx_fx_available(fx))
  return settings_row(t,@[settings_text("游戏原样","As the game has it"),settings_text("2048（每级）","2048 per cascade")],@[@0,@2048],
   ^int{return runtime_shadow_res_saved();},^(int v){runtime_shadow_res_save(v);});
 return settings_row(t,@[settings_text("关（无阴影，立即生效）","Off (no shadows, at once)"),settings_text("游戏原样","As the game has it"),settings_text("2048（每级）","2048 per cascade")],@[@-1,@0,@2048],
  ^int{return dm_gfx_fx_on(fx)?runtime_shadow_res_saved():-1;},
  ^(int v){dm_gfx_fx_set(fx,v>=0);if(v>=0)runtime_shadow_res_save(v);settings_effects_save();});
}
static NSArray<TOTKSettingRow *> *settings_effect_rows(void) {
 NSString *none=settings_text("这个版本没有","Not in this version");
 NSMutableArray<TOTKSettingRow *> *rows=[NSMutableArray arrayWithObjects:settings_botw_fov_row(),
  settings_fixed(settings_text("模型细节距离（LOD）","Model detail distance (LOD)"),none),settings_fixed(settings_text("渲染距离","Render distance"),none),
  settings_botw_shadow_row(),nil];
 /* 1.4.3's anti-aliasing rows: the quality row follows the method (settings_aa_rebuild). */
 NSString *method=settings_text("抗锯齿方式","Anti-aliasing method"),*quality=settings_text("抗锯齿质量","Anti-aliasing quality");
 if(dm_gfx_botw_fxaa_replaced()){
  NSString *mfx=settings_text("MetalFX 取代了它","MetalFX replaces it");
  [rows addObject:settings_fixed(method,mfx)];
  if(dm_gfx_aa_method())[rows addObject:settings_fixed(quality,mfx)];
 }else{
  [rows addObject:settings_row(method,@[settings_text("关","Off"),@"FXAA",@"SMAA"],@[@0,@1,@2],
   ^int{return dm_gfx_aa_method();},^(int v){dm_gfx_aa_method_set(v);settings_aa_save();})];
  if(dm_gfx_aa_method()==1)
   [rows addObject:settings_row(quality,@[settings_text("原版","Original"),@"Quality"],@[@0,@1],
    ^int{return dm_gfx_fxaa_quality();},^(int v){dm_gfx_fxaa_quality_set(v);settings_aa_save();})];
  else if(dm_gfx_aa_method()==2)
   [rows addObject:settings_row(quality,@[@"Low",@"Medium",@"High",@"Ultra"],@[@0,@1,@2,@3],
    ^int{return dm_gfx_smaa_quality();},^(int v){dm_gfx_smaa_quality_set(v);settings_aa_save();})];
 }
 [rows addObject:settings_fixed(settings_text("景深","Depth of field"),none)];
 [rows addObject:settings_row(settings_text("锐化","Sharpening"),@[settings_text("关（游戏原样）","Off (as the game has it)"),@"25%",@"50%",@"75%",@"100%"],@[@0,@25,@50,@75,@100],
  ^int{return dm_gfx_sharpen();},^(int v){dm_gfx_sharpen_set(v);settings_save_int("graphics-sharpen.txt",v,"sharpening");})];
 for(int i=0;i<dm_gfx_fx_count();i++){
  if(!strcmp(dm_gfx_fx_id(i),"shadow"))continue;   /* in the shadow resolution row */
  NSString *t=@(TOTK_UI(dm_gfx_fx_name(i,0),dm_gfx_fx_name(i,1)));
  if(!dm_gfx_fx_available(i)){[rows addObject:settings_fixed(t,none)];continue;}
  [rows addObject:settings_row(t,nil,nil,^int{return dm_gfx_fx_on(i);},^(int v){dm_gfx_fx_set(i,v);settings_effects_save();})];
 }
 return rows;
}
