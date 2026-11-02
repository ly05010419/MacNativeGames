/* The key bindings drawings of the settings panel (Mac; included by settings_panel.h). PR #1210: instead of a list,
   line drawings of a Mac keyboard with the mouse, and of a controller, with what each key or button does written on it.
   Keys and buttons in use are coloured by kind (moving and camera, combat, actions, menus and d-pad); key 1, which opens
   the panel, has its own colour; unused ones stay grey outlines. The controller drawing follows the controller type:
   Xbox and Switch put the left stick above the d-pad, PlayStation the d-pad above it and a touchpad between them; the
   button names follow it too (settings_pad_button). Each view reads the layout in force when it draws
   (runtime_keymap_get, runtime_controller_map_get), so it always shows what is in force. A click on a key, a mouse button,
   a controller button or its callout calls pick with that input (keyboard_map.h: key codes, 128-132 the mouse buttons) or
   physical button (controller_map.h) and where it is drawn, and the panel shows its menu of actions there. Below each
   drawing, the actions no key or button drives. */
#pragma once

enum { DIAGRAM_NONE, DIAGRAM_MOVE, DIAGRAM_COMBAT, DIAGRAM_ACTION, DIAGRAM_MENU, DIAGRAM_SYSTEM };
static NSColor *diagram_color(int kind) {
 switch(kind){
  case DIAGRAM_MOVE:return [NSColor colorWithCalibratedRed:0.38 green:0.65 blue:1.0 alpha:1];
  case DIAGRAM_COMBAT:return [NSColor colorWithCalibratedRed:1.0 green:0.62 blue:0.28 alpha:1];
  case DIAGRAM_ACTION:return [NSColor colorWithCalibratedRed:0.42 green:0.86 blue:0.56 alpha:1];
  case DIAGRAM_MENU:return [NSColor colorWithCalibratedRed:0.76 green:0.58 blue:1.0 alpha:1];
  case DIAGRAM_SYSTEM:return [NSColor colorWithCalibratedRed:0.95 green:0.85 blue:0.40 alpha:1];
 }
 return [NSColor colorWithCalibratedWhite:0.62 alpha:1];
}
static int diagram_kind(int action) {
 if(action<0)return DIAGRAM_NONE;
 if(action>=KEYMAP_MOVE_UP)return DIAGRAM_MOVE;
 if(action==KEYMAP_Y||action==KEYMAP_ZR||action==KEYMAP_ZL)return DIAGRAM_COMBAT;
 if(action==KEYMAP_PLUS||action==KEYMAP_MINUS||(action>=KEYMAP_DLEFT&&action<=KEYMAP_DDOWN))return DIAGRAM_MENU;
 return DIAGRAM_ACTION;
}
/* The few words written on a key (a line break where two do not fit). */
static NSString *diagram_action_short(int action) {
 switch(action){
#ifdef GAME_XC3
  /* Xenoblade Chronicles 3: the Switch button's name (settings_actions), with A and B's one meaning. */
  case KEYMAP_A:return settings_text("A 确认","A\nConfirm");
  case KEYMAP_B:return settings_text("B 返回","B\nBack");
  case KEYMAP_X:return @"X";
  case KEYMAP_Y:return @"Y";
  case KEYMAP_ZR:return @"ZR";
  case KEYMAP_ZL:return @"ZL";
  case KEYMAP_L:return @"L";
  case KEYMAP_R:return @"R";
  case KEYMAP_LSTICK:return @"L3";
  case KEYMAP_RSTICK:return @"R3";
  case KEYMAP_PLUS:return settings_text("+ 菜单","+\nMenu");
  case KEYMAP_MINUS:return @"−";
#endif
#ifndef GAME_XC3
  case KEYMAP_A:return settings_text("确认","Confirm");
  case KEYMAP_B:return settings_text("冲刺\n取消","Sprint\nCancel");
  case KEYMAP_X:return settings_text("跳跃","Jump");
  case KEYMAP_Y:return settings_text("攻击","Attack");
  case KEYMAP_ZR:return settings_text("弓","Bow");
  case KEYMAP_ZL:return settings_text("锁定","Lock on");
#ifdef GAME_BOTW
  case KEYMAP_L:return settings_text("希卡石","Rune");
#else
  case KEYMAP_L:return settings_text("能力","Ability");
#endif
  case KEYMAP_R:return settings_text("投掷","Throw");
  case KEYMAP_LSTICK:return settings_text("蹲下","Crouch");
  case KEYMAP_RSTICK:return settings_text("主视角","Look");
  case KEYMAP_PLUS:return settings_text("菜单","Menu");
  case KEYMAP_MINUS:return settings_text("地图","Map");
#endif
  case KEYMAP_DLEFT:return settings_text("十字 ←","D-pad ←");
  case KEYMAP_DUP:return settings_text("十字 ↑","D-pad ↑");
  case KEYMAP_DRIGHT:return settings_text("十字 →","D-pad →");
  case KEYMAP_DDOWN:return settings_text("十字 ↓","D-pad ↓");
  case KEYMAP_MOVE_UP:return settings_text("前进","Forward");
  case KEYMAP_MOVE_DOWN:return settings_text("后退","Back");
  case KEYMAP_MOVE_LEFT:return settings_text("左移","Left");
  case KEYMAP_MOVE_RIGHT:return settings_text("右移","Right");
  case KEYMAP_CAMERA_UP:return settings_text("视角 ↑","Cam ↑");
  case KEYMAP_CAMERA_DOWN:return settings_text("视角 ↓","Cam ↓");
  case KEYMAP_CAMERA_LEFT:return settings_text("视角 ←","Cam ←");
  case KEYMAP_CAMERA_RIGHT:return settings_text("视角 →","Cam →");
 }
 return @"";
}
/* The action's full name, as the list and the menus write it (settings_actions). */
static NSString *diagram_action_name(int action) {
 int count;const SettingsAction *all=settings_actions(&count);
 for(int i=0;i<count;i++)if(all[i].action==action)return @(TOTK_UI(all[i].zh,all[i].en));
 return @"—";
}
/* The actions nothing drives, for the line under a drawing: keys and mouse (pad 0) or controller (pad 1). */
static NSString *diagram_missing(int pad) {
 KeyMap keys;uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];
 if(pad)runtime_controller_map_get(map,modes);else runtime_keymap_get(&keys);
 NSMutableArray<NSString *> *names=[NSMutableArray array];
 int count;const SettingsAction *all=settings_actions(&count);
 for(int i=0;i<count;i++){
  int a=all[i].action;if(a<0||(pad&&a>=KEYMAP_SWITCH_BUTTONS))continue;
  BOOL used=NO;
  if(pad){for(int p=0;p<CONTROLLER_BUTTONS;p++)used|=controller_map_target(map[p])==a;}
  else used=keys.slot[a][0]!=KEYMAP_NONE||keys.slot[a][1]!=KEYMAP_NONE;
  if(!used)[names addObject:@(TOTK_UI(all[i].zh,all[i].en))];
 }
 return names.count?[(pad?settings_text("没有按钮：","No button: "):settings_text("没有按键：","No key: "))
  stringByAppendingString:[names componentsJoinedByString:@", "]]:nil;
}
static void diagram_text(NSString *s, NSRect r, CGFloat size, NSFontWeight weight, NSColor *color, NSTextAlignment align) {
 NSMutableParagraphStyle *p=[NSMutableParagraphStyle new];p.alignment=align;p.lineBreakMode=NSLineBreakByWordWrapping;p.maximumLineHeight=size+2;
 NSDictionary *at=@{NSFontAttributeName:[NSFont systemFontOfSize:size weight:weight],NSForegroundColorAttributeName:color,NSParagraphStyleAttributeName:p};
 NSRect b=[s boundingRectWithSize:NSMakeSize(r.size.width,CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin attributes:at];
 r.origin.y+=fmax(0,(r.size.height-b.size.height)/2);r.size.height=b.size.height+1;
 [s drawWithRect:r options:NSStringDrawingUsesLineFragmentOrigin attributes:at];
}
/* A shape filled and outlined in its action's kind; selected (the one being changed) gets a white outline. */
static void diagram_shape(NSBezierPath *p, int action, BOOL selected, CGFloat fill) {
 [[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:1] setFill];[p fill];
 if(action>=0){[[diagram_color(diagram_kind(action)) colorWithAlphaComponent:fill] setFill];[p fill];[diagram_color(diagram_kind(action)) setStroke];}
 else [[NSColor colorWithCalibratedWhite:1 alpha:0.5] setStroke];
 p.lineWidth=1.3;[p stroke];
 if(selected){[[NSColor whiteColor] setStroke];p.lineWidth=2.6;[p stroke];}
}
static void diagram_legend(CGFloat x, CGFloat y) {
 NSString *names[]={settings_text("移动和视角","Moving & camera"),settings_text("战斗","Combat"),settings_text("动作","Actions"),
  settings_text("菜单和十字键","Menus & d-pad"),settings_text("设置面板","Settings panel"),settings_text("未使用","Not used")};
 int kinds[]={DIAGRAM_MOVE,DIAGRAM_COMBAT,DIAGRAM_ACTION,DIAGRAM_MENU,DIAGRAM_SYSTEM,DIAGRAM_NONE};
 NSFont *font=[NSFont systemFontOfSize:10.5 weight:NSFontWeightMedium];
 for(int i=0;i<6;i++){
  NSBezierPath *dot=[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x,y+3,14,10) xRadius:3 yRadius:3];NSColor *c=diagram_color(kinds[i]);
  if(kinds[i]!=DIAGRAM_NONE){[[c colorWithAlphaComponent:0.25] setFill];[dot fill];[c setStroke];}else [[NSColor colorWithCalibratedWhite:1 alpha:0.3] setStroke];
  [dot stroke];
  CGFloat w=[names[i] sizeWithAttributes:@{NSFontAttributeName:font}].width;
  diagram_text(names[i],NSMakeRect(x+19,y,w+4,16),10.5,NSFontWeightMedium,[NSColor colorWithCalibratedWhite:0.75 alpha:1],NSTextAlignmentLeft);
  x+=19+w+22;
 }
}
static void diagram_missing_line(int pad, NSRect r) {
 NSString *missing=diagram_missing(pad);
 if(missing)diagram_text(missing,r,10.5,NSFontWeightMedium,[NSColor colorWithCalibratedRed:1 green:0.75 blue:0.45 alpha:1],NSTextAlignmentLeft);
}

/* ---- Keyboard and mouse ---- */
typedef struct { NSRect rect; int code; const char *label; } DiagramKey;
enum { DIAGRAM_KEYS_MAX = 96 };
#define DIAGRAM_U 44.0     /* one key's width */
#define DIAGRAM_X0 8.0
#define DIAGRAM_Y0 30.0
/* The keys of a US Mac keyboard where they are drawn (rows of key widths; the arrows at half height). */
static int diagram_keyboard(DiagramKey out[DIAGRAM_KEYS_MAX]) {
 typedef struct { const char *label; int code; double w; } K;
 static const K fn[]={{"esc",53,1.5},{"F1",122,1.0833},{"F2",120,1.0833},{"F3",99,1.0833},{"F4",118,1.0833},{"F5",96,1.0833},{"F6",97,1.0833},
  {"F7",98,1.0833},{"F8",100,1.0833},{"F9",101,1.0833},{"F10",109,1.0833},{"F11",103,1.0833},{"F12",111,1.0833}};
 static const K r1[]={{"`",50,1},{"1",18,1},{"2",19,1},{"3",20,1},{"4",21,1},{"5",23,1},{"6",22,1},{"7",26,1},{"8",28,1},{"9",25,1},{"0",29,1},{"-",27,1},{"=",24,1},{"delete",51,1.5}};
 static const K r2[]={{"tab",48,1.5},{"Q",12,1},{"W",13,1},{"E",14,1},{"R",15,1},{"T",17,1},{"Y",16,1},{"U",32,1},{"I",34,1},{"O",31,1},{"P",35,1},{"[",33,1},{"]",30,1},{"\\",42,1}};
 static const K r3[]={{"caps lock",57,1.75},{"A",0,1},{"S",1,1},{"D",2,1},{"F",3,1},{"G",5,1},{"H",4,1},{"J",38,1},{"K",40,1},{"L",37,1},{";",41,1},{"'",39,1},{"return",36,1.75}};
 static const K r4[]={{"shift",56,2.25},{"Z",6,1},{"X",7,1},{"C",8,1},{"V",9,1},{"B",11,1},{"N",45,1},{"M",46,1},{",",43,1},{".",47,1},{"/",44,1},{"shift",60,2.25}};
 static const K r5[]={{"fn",63,1},{"control",59,1},{"option",58,1},{"command",55,1.25},{"space",49,5},{"command",54,1.25},{"option",61,1}};
 const struct { const K *keys; int n; double h; } rows[]={{fn,13,0.72},{r1,14,1},{r2,14,1},{r3,13,1},{r4,12,1},{r5,7,1}};
 int n=0;CGFloat y=DIAGRAM_Y0;
 for(int i=0;i<6;i++){
  CGFloat x=DIAGRAM_X0,h=rows[i].h*DIAGRAM_U;
  for(int k=0;k<rows[i].n;k++){CGFloat w=rows[i].keys[k].w*DIAGRAM_U;out[n++]=(DiagramKey){NSMakeRect(x,y,w,h),rows[i].keys[k].code,rows[i].keys[k].label};x+=w;}
  if(i==5){CGFloat u=DIAGRAM_U;
   out[n++]=(DiagramKey){NSMakeRect(x,y+h/2,u,h/2),123,"←"};out[n++]=(DiagramKey){NSMakeRect(x+u,y,u,h/2),126,"↑"};
   out[n++]=(DiagramKey){NSMakeRect(x+u,y+h/2,u,h/2),125,"↓"};out[n++]=(DiagramKey){NSMakeRect(x+2*u,y+h/2,u,h/2),124,"→"};}
  y+=h;
 }
 return n;
}
#define DIAGRAM_HALF_H (0.6*DIAGRAM_U)   /* the arrows are drawn at half height */
#define DIAGRAM_HALF_NAME_W 13.0
/* A half-height key's action on one line (without a closing arrow that repeats the key's own: ↑ 视角, not ↑ 视角 ↑), or nil
   when the key drives nothing; and where it is written. */
static NSString *diagram_half_action(const KeyMap *m, DiagramKey k) {
 if(k.code==KEYMAP_SETTINGS_KEY)return settings_text("设置","Settings");
 int a=keymap_action_of(m,k.code,NULL);
 if(a<0)return nil;
 NSString *t=[diagram_action_short(a) stringByReplacingOccurrencesOfString:@"\n" withString:@" "],*own=@(k.label);
 if(t.length>own.length&&[t hasSuffix:own])t=[[t substringToIndex:t.length-own.length] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
 return t;
}
static NSRect diagram_half_action_rect(NSRect r) { return NSMakeRect(r.origin.x+4+DIAGRAM_HALF_NAME_W,r.origin.y+2,r.size.width-8-DIAGRAM_HALF_NAME_W,r.size.height-4); }
#define DIAGRAM_KEYBOARD_H (5.72*DIAGRAM_U)
#define DIAGRAM_MOUSE_X (DIAGRAM_X0+14.5*DIAGRAM_U+40)
enum { DIAGRAM_MOUSE_W = 92, DIAGRAM_MOUSE_H = 150, DIAGRAM_MOUSE_SPLIT = 58 };

@interface TOTKKeyboardDiagram:NSView
@property int selected;   /* the input whose menu is open (white outline), -1 none */
@property(copy) void (^pick)(int input, NSRect at);
-(int)inputAt:(NSPoint)point rect:(NSRect *)rect;
-(NSRect)rectOfInput:(int)input;
@end
@implementation TOTKKeyboardDiagram
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame]))self.selected=-1;return self;}
-(BOOL)isFlipped{return YES;}
/* The mouse's parts, where a click selects them: 0 left, 1 right, 2 the wheel, 3 and 4 the side buttons; then the callout lines. */
-(NSRect)mousePart:(int)i{
 CGFloat x=DIAGRAM_MOUSE_X,y=DIAGRAM_Y0,w=DIAGRAM_MOUSE_W;
 switch(i){
  case 0:return NSMakeRect(x,y+4,w/2-7,DIAGRAM_MOUSE_SPLIT);
  case 1:return NSMakeRect(x+w/2+7,y+4,w/2-7,DIAGRAM_MOUSE_SPLIT);
  case 2:return NSMakeRect(x+w/2-7,y+14,14,34);
  case 3:return NSMakeRect(x-6,y+72,14,24);
  case 4:return NSMakeRect(x-6,y+96,14,24);
 }
 return NSZeroRect;
}
-(NSRect)calloutRow:(int)i{return NSMakeRect(DIAGRAM_MOUSE_X+DIAGRAM_MOUSE_W+26,DIAGRAM_Y0+6+i*28,150,26);}
-(int)inputAt:(NSPoint)p rect:(NSRect *)rect{
 DiagramKey keys[DIAGRAM_KEYS_MAX];int n=diagram_keyboard(keys);
 for(int i=0;i<n;i++)if(NSPointInRect(p,keys[i].rect)){if(rect)*rect=keys[i].rect;return keymap_bindable(keys[i].code)?keys[i].code:-1;}
 int order[]={2,3,4,0,1};   /* the wheel and the side buttons before the halves they sit in */
 for(int k=0;k<5;k++){int i=order[k];if(NSPointInRect(p,[self mousePart:i])||NSPointInRect(p,[self calloutRow:(int[]){0,2,1,3,4}[i]])){
  if(rect)*rect=[self mousePart:i];return KEYMAP_MOUSE_FIRST+i;}}
 return -1;
}
-(NSRect)rectOfInput:(int)input{
 if(input>=KEYMAP_MOUSE_FIRST)return [self mousePart:input-KEYMAP_MOUSE_FIRST];
 DiagramKey keys[DIAGRAM_KEYS_MAX];int n=diagram_keyboard(keys);
 for(int i=0;i<n;i++)if(keys[i].code==input)return keys[i].rect;
 return NSZeroRect;
}
-(void)mouseDown:(NSEvent *)e{
 NSRect r;int input=[self inputAt:[self convertPoint:e.locationInWindow fromView:nil] rect:&r];
 if(input>=0&&self.pick)self.pick(input,r);
}
-(void)drawKey:(DiagramKey)k map:(const KeyMap *)m halfSize:(CGFloat)halfSize{
 int a=keymap_action_of(m,k.code,NULL),settings=k.code==KEYMAP_SETTINGS_KEY,bindable=keymap_bindable(k.code),kind=settings?DIAGRAM_SYSTEM:diagram_kind(a);
 NSRect r=k.rect;NSBezierPath *path=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r,2,2) xRadius:5 yRadius:5];NSColor *c=diagram_color(kind);
 if(kind!=DIAGRAM_NONE){[[c colorWithAlphaComponent:0.20] setFill];[path fill];[[c colorWithAlphaComponent:0.95] setStroke];path.lineWidth=1.4;}
 else{[[NSColor colorWithCalibratedWhite:1 alpha:bindable?0.30:0.14] setStroke];path.lineWidth=1;}
 [path stroke];
 if(k.code==self.selected){[[NSColor whiteColor] setStroke];path.lineWidth=2.6;[path stroke];}
 NSColor *labelColor=[NSColor colorWithCalibratedWhite:1 alpha:kind!=DIAGRAM_NONE?0.95:bindable?0.55:0.28];
 NSString *name=@(k.label);CGFloat size=9.5;   /* long names in narrow keys (control, command, option) a little smaller */
 while(size>7&&[name sizeWithAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:size weight:NSFontWeightSemibold]}].width>r.size.width-10)size-=0.5;
 if(kind==DIAGRAM_NONE){diagram_text(name,NSInsetRect(r,3,3),size,NSFontWeightMedium,labelColor,NSTextAlignmentCenter);return;}
 if(r.size.height<DIAGRAM_HALF_H){   /* the half-height arrows: the key and its action side by side on one line */
  NSRect at=diagram_half_action_rect(r);
  diagram_text(name,NSMakeRect(r.origin.x+4,r.origin.y+2,DIAGRAM_HALF_NAME_W,r.size.height-4),size,NSFontWeightSemibold,labelColor,NSTextAlignmentLeft);
  diagram_text(diagram_half_action(m,k),at,halfSize,NSFontWeightBold,c,NSTextAlignmentCenter);
  return;
 }
 diagram_text(name,NSMakeRect(r.origin.x+5,r.origin.y+3,r.size.width-10,12),size,NSFontWeightSemibold,labelColor,NSTextAlignmentLeft);
 diagram_text(settings?settings_text("设置","Settings"):diagram_action_short(a),NSMakeRect(r.origin.x+3,r.origin.y+14,r.size.width-6,r.size.height-17),
  8.6,NSFontWeightBold,c,NSTextAlignmentCenter);
}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;KeyMap m;runtime_keymap_get(&m);
 NSColor *heading=[NSColor colorWithCalibratedWhite:0.85 alpha:1];
 NSBezierPath *body=[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(DIAGRAM_X0-6,DIAGRAM_Y0-6,14.5*DIAGRAM_U+12,DIAGRAM_KEYBOARD_H+12) xRadius:12 yRadius:12];
 [[NSColor colorWithCalibratedWhite:1 alpha:0.03] setFill];[body fill];[[NSColor colorWithCalibratedWhite:1 alpha:0.22] setStroke];body.lineWidth=1.2;[body stroke];
 DiagramKey keys[DIAGRAM_KEYS_MAX];int n=diagram_keyboard(keys);
 CGFloat half=8.6;   /* one size for the half-height arrows' actions: the largest at which all of them fit */
 for(int i=0;i<n;i++){NSString *t=keys[i].rect.size.height<DIAGRAM_HALF_H?diagram_half_action(&m,keys[i]):nil;CGFloat w=diagram_half_action_rect(keys[i].rect).size.width;
  while(t&&half>6.5&&[t sizeWithAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:half weight:NSFontWeightBold]}].width>w)half-=0.5;}
 for(int i=0;i<n;i++)[self drawKey:keys[i] map:&m halfSize:half];
 diagram_text(settings_text("键盘","Keyboard"),NSMakeRect(DIAGRAM_X0,4,300,18),12,NSFontWeightSemibold,heading,NSTextAlignmentLeft);
 [self drawMouse:&m];
 CGFloat y=DIAGRAM_Y0+DIAGRAM_KEYBOARD_H+20;
 diagram_legend(DIAGRAM_X0,y);
 diagram_missing_line(0,NSMakeRect(DIAGRAM_X0,y+22,self.bounds.size.width-2*DIAGRAM_X0,16));
}
-(void)drawMouse:(const KeyMap *)m{
 CGFloat x=DIAGRAM_MOUSE_X,y=DIAGRAM_Y0,w=DIAGRAM_MOUSE_W,cx=x+w/2;
 diagram_text(runtime_mouse_enabled()?settings_text("鼠标","Mouse"):settings_text("鼠标（鼠标操作未开）","Mouse (mouse mode is off)"),
  NSMakeRect(x,4,260,18),12,NSFontWeightSemibold,[NSColor colorWithCalibratedWhite:0.85 alpha:1],NSTextAlignmentLeft);
 int act[5];for(int i=0;i<5;i++)act[i]=keymap_action_of(m,KEYMAP_MOUSE_FIRST+i,NULL);
 NSBezierPath *body=[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x,y+4,w,DIAGRAM_MOUSE_H) xRadius:w/2 yRadius:w/2];
 [NSGraphicsContext saveGraphicsState];[body addClip];
 for(int i=0;i<2;i++)if(act[i]>=0){[[diagram_color(diagram_kind(act[i])) colorWithAlphaComponent:0.22] setFill];NSRectFill(NSMakeRect(i?cx:x,y,w/2,DIAGRAM_MOUSE_SPLIT+4));}
 for(int i=0;i<2;i++)if(self.selected==KEYMAP_MOUSE_FIRST+i){[[NSColor colorWithCalibratedWhite:1 alpha:0.25] setFill];NSRectFill(NSMakeRect(i?cx:x,y,w/2,DIAGRAM_MOUSE_SPLIT+4));}
 [NSGraphicsContext restoreGraphicsState];
 [[NSColor colorWithCalibratedWhite:1 alpha:0.55] setStroke];body.lineWidth=1.4;[body stroke];
 NSBezierPath *lines=[NSBezierPath bezierPath];
 [lines moveToPoint:NSMakePoint(cx,y+4)];[lines lineToPoint:NSMakePoint(cx,y+4+DIAGRAM_MOUSE_SPLIT)];
 [lines moveToPoint:NSMakePoint(x+1,y+4+DIAGRAM_MOUSE_SPLIT)];
 [lines curveToPoint:NSMakePoint(x+w-1,y+4+DIAGRAM_MOUSE_SPLIT) controlPoint1:NSMakePoint(x+25,y+DIAGRAM_MOUSE_SPLIT+12) controlPoint2:NSMakePoint(x+w-25,y+DIAGRAM_MOUSE_SPLIT+12)];
 lines.lineWidth=1.2;[lines stroke];
 diagram_shape([NSBezierPath bezierPathWithRoundedRect:NSMakeRect(cx-6,y+18,12,26) xRadius:6 yRadius:6],act[2],self.selected==KEYMAP_MOUSE_FIRST+2,0.5);
 for(int i=0;i<2;i++)diagram_shape([NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x-4,y+74+i*24,8,20) xRadius:3 yRadius:3],act[3+i],self.selected==KEYMAP_MOUSE_FIRST+3+i,0.5);
 /* callouts: left click, wheel click, right click, the side buttons, moving the mouse */
 struct { NSPoint at; int part; } call[]={{{x+20,y+30},0},{{cx,y+46},2},{{x+w-20,y+30},1},{{x,y+84},3},{{x,y+108},4},{{cx,y+130},-1}};
 for(int i=0;i<6;i++){
  NSRect row=[self calloutRow:i];int a=call[i].part<0?KEYMAP_CAMERA_UP:act[call[i].part];
  NSColor *c=a>=0?diagram_color(diagram_kind(a)):[NSColor colorWithCalibratedWhite:0.6 alpha:1];
  NSBezierPath *leader=[NSBezierPath bezierPath];[leader moveToPoint:call[i].at];
  [leader lineToPoint:NSMakePoint(x+w+8,row.origin.y+12)];[leader lineToPoint:NSMakePoint(row.origin.x-4,row.origin.y+12)];
  [[c colorWithAlphaComponent:0.6] setStroke];leader.lineWidth=1;[leader stroke];
  [c setFill];[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(call[i].at.x-2.5,call[i].at.y-2.5,5,5)] fill];
  NSString *name=call[i].part<0?settings_text("移动鼠标","Move the mouse"):settings_input_text(KEYMAP_MOUSE_FIRST+call[i].part);
  diagram_text(name,NSMakeRect(row.origin.x,row.origin.y,row.size.width,12),9.5,NSFontWeightMedium,[NSColor colorWithCalibratedWhite:0.7 alpha:1],NSTextAlignmentLeft);
  NSString *what=call[i].part<0?settings_text("视角","Camera"):a>=0?diagram_action_name(a):@"—";
  diagram_text(what,NSMakeRect(row.origin.x,row.origin.y+12,row.size.width,14),11,NSFontWeightSemibold,c,NSTextAlignmentLeft);
 }
}
@end

/* ---- Controller ---- */
typedef struct { int physical; NSPoint at; int stick; } DiagramCall;   /* stick 1 left, 2 right: no button, not changeable */
enum { DIAGRAM_CALLS = 10 };
typedef struct {
 CGFloat cx, y0; NSRect lt, rt, lb, rb, touchpad; NSPoint ls, rs, dp, face, view, menu;
 DiagramCall calls[2][DIAGRAM_CALLS]; int count[2];
} DiagramPad;
static DiagramPad diagram_pad(NSRect bounds, int family) {
 DiagramPad d={0};BOOL ps=family==GAMEPAD_TYPE_PLAYSTATION;
 CGFloat cx=d.cx=floor(bounds.size.width/2),y0=d.y0=58;
 d.lt=NSMakeRect(cx-150,y0-46,62,34);d.rt=NSMakeRect(cx+88,y0-46,62,34);d.lb=NSMakeRect(cx-168,y0-16,96,22);d.rb=NSMakeRect(cx+72,y0-16,96,22);
 d.ls=ps?NSMakePoint(cx-58,y0+122):NSMakePoint(cx-100,y0+58);d.dp=ps?NSMakePoint(cx-100,y0+58):NSMakePoint(cx-58,y0+122);
 d.rs=NSMakePoint(cx+58,y0+122);d.face=NSMakePoint(cx+100,y0+58);
 d.view=NSMakePoint(ps?cx-50:cx-30,y0+(ps?30:52));d.menu=NSMakePoint(ps?cx+50:cx+30,y0+(ps?30:52));
 d.touchpad=ps?NSMakeRect(cx-34,y0+12,68,40):NSZeroRect;
 DiagramCall stick[]={{-1,{d.ls.x-24,d.ls.y},1},{4,{d.ls.x-8,d.ls.y+8},0}};
 DiagramCall pad[]={{13,{d.dp.x,d.dp.y-15},0},{12,{d.dp.x-15,d.dp.y},0},{14,{d.dp.x+15,d.dp.y},0},{15,{d.dp.x,d.dp.y+15},0}};
 DiagramCall left[]={{8,{d.lt.origin.x+12,NSMidY(d.lt)},0},{6,{d.lb.origin.x+10,NSMidY(d.lb)},0},{11,d.view,0}};
 int n=0;for(int i=0;i<3;i++)d.calls[0][n++]=left[i];
 /* top to bottom as the controls sit: the stick above the d-pad (Xbox, Switch) or below it (PlayStation) */
 for(int i=0;i<6;i++)d.calls[0][n++]=ps?(i<4?pad[i]:stick[i-4]):(i<2?stick[i]:pad[i-2]);
 d.count[0]=n;
 DiagramCall right[]={{9,{NSMaxX(d.rt)-12,NSMidY(d.rt)},0},{7,{NSMaxX(d.rb)-10,NSMidY(d.rb)},0},{10,d.menu,0},
  {3,{d.face.x,d.face.y-20},0},{1,{d.face.x+20,d.face.y},0},{2,{d.face.x-20,d.face.y},0},{0,{d.face.x,d.face.y+20},0},
  {-1,{d.rs.x+24,d.rs.y},2},{5,{d.rs.x+8,d.rs.y+8},0},{16,{NSMaxX(d.touchpad)-6,NSMidY(d.touchpad)},0}};
 d.count[1]=ps?10:9;for(int i=0;i<d.count[1];i++)d.calls[1][i]=right[i];
 return d;
}
static NSRect diagram_call_row(const DiagramPad *d, int side, int i) {
 return NSMakeRect(side?d->cx+236:d->cx-236-170,10+i*30,170,28);
}

@interface TOTKControllerDiagram:NSView
@property int selected;   /* the physical button whose menu is open or that a controller has picked, -1 none */
@property(copy) void (^pick)(int physical, NSRect at);
-(int)physicalAt:(NSPoint)point rect:(NSRect *)rect;
-(NSArray<NSNumber *> *)order;   /* the buttons a controller moves through: the callouts top to bottom, left then right */
-(NSRect)rectOfPhysical:(int)physical;
@end
@implementation TOTKControllerDiagram
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame]))self.selected=-1;return self;}
-(BOOL)isFlipped{return YES;}
-(NSArray<NSNumber *> *)order{
 DiagramPad d=diagram_pad(self.bounds,settings_pad_family());NSMutableArray<NSNumber *> *o=[NSMutableArray array];
 for(int s=0;s<2;s++)for(int i=0;i<d.count[s];i++)if(d.calls[s][i].physical>=0)[o addObject:@(d.calls[s][i].physical)];
 return o;
}
/* Where each button is drawn (also what a click on it hits). */
-(NSRect)shapeOf:(int)physical pad:(const DiagramPad *)d{
 NSPoint dp=d->dp,f=d->face;
 switch(physical){
  case 0:return NSMakeRect(f.x-10,f.y+10,20,20);case 1:return NSMakeRect(f.x+10,f.y-10,20,20);
  case 2:return NSMakeRect(f.x-30,f.y-10,20,20);case 3:return NSMakeRect(f.x-10,f.y-30,20,20);
  case 4:return NSMakeRect(d->ls.x-15,d->ls.y-15,30,30);case 5:return NSMakeRect(d->rs.x-15,d->rs.y-15,30,30);
  case 6:return d->lb;case 7:return d->rb;case 8:return d->lt;case 9:return d->rt;
  case 10:return NSMakeRect(d->menu.x-9,d->menu.y-6,18,12);case 11:return NSMakeRect(d->view.x-9,d->view.y-6,18,12);
  case 12:return NSMakeRect(dp.x-23,dp.y-8,16,16);case 13:return NSMakeRect(dp.x-8,dp.y-23,16,16);
  case 14:return NSMakeRect(dp.x+7,dp.y-8,16,16);case 15:return NSMakeRect(dp.x-8,dp.y+7,16,16);
  case 16:return d->touchpad;
 }
 return NSZeroRect;
}
-(NSRect)rectOfPhysical:(int)physical{DiagramPad d=diagram_pad(self.bounds,settings_pad_family());return [self shapeOf:physical pad:&d];}
-(int)physicalAt:(NSPoint)p rect:(NSRect *)rect{
 DiagramPad d=diagram_pad(self.bounds,settings_pad_family());
 /* the small parts over the large ones: face buttons, d-pad and stick presses before the shoulders and the touchpad */
 static const int order[]={0,1,2,3,12,13,14,15,4,5,10,11,6,7,8,9,16};
 for(unsigned i=0;i<sizeof order/sizeof *order;i++){NSRect r=[self shapeOf:order[i] pad:&d];if(!NSIsEmptyRect(r)&&NSPointInRect(p,NSInsetRect(r,-3,-3))){if(rect)*rect=r;return order[i];}}
 for(int s=0;s<2;s++)for(int i=0;i<d.count[s];i++)if(d.calls[s][i].physical>=0&&NSPointInRect(p,diagram_call_row(&d,s,i))){
  if(rect)*rect=[self shapeOf:d.calls[s][i].physical pad:&d];return d.calls[s][i].physical;}
 return -1;
}
-(void)mouseDown:(NSEvent *)e{
 NSRect r;int physical=[self physicalAt:[self convertPoint:e.locationInWindow fromView:nil] rect:&r];
 if(physical>=0&&self.pick)self.pick(physical,r);
}
-(void)drawRect:(NSRect)dirty{
 (void)dirty;int family=settings_pad_family();BOOL ps=family==GAMEPAD_TYPE_PLAYSTATION;
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 DiagramPad d=diagram_pad(self.bounds,family);CGFloat cx=d.cx,y0=d.y0;
 NSColor *line=[NSColor colorWithCalibratedWhite:1 alpha:0.55];
 int sel=self.selected;
#define TARGET(p) controller_map_target(map[p])
 for(int p=6;p<=9;p++)diagram_shape([NSBezierPath bezierPathWithRoundedRect:[self shapeOf:p pad:&d] xRadius:p>=8?12:10 yRadius:p>=8?12:10],TARGET(p),sel==p,0.25);
 /* the body: one outline with two grips */
 NSBezierPath *b=[NSBezierPath bezierPath];
 [b moveToPoint:NSMakePoint(cx-120,y0)];[b lineToPoint:NSMakePoint(cx+120,y0)];
 [b curveToPoint:NSMakePoint(cx+178,y0+150) controlPoint1:NSMakePoint(cx+165,y0) controlPoint2:NSMakePoint(cx+180,y0+80)];
 [b curveToPoint:NSMakePoint(cx+118,y0+212) controlPoint1:NSMakePoint(cx+176,y0+205) controlPoint2:NSMakePoint(cx+140,y0+222)];
 [b curveToPoint:NSMakePoint(cx+62,y0+148) controlPoint1:NSMakePoint(cx+95,y0+200) controlPoint2:NSMakePoint(cx+82,y0+150)];
 [b lineToPoint:NSMakePoint(cx-62,y0+148)];
 [b curveToPoint:NSMakePoint(cx-118,y0+212) controlPoint1:NSMakePoint(cx-82,y0+150) controlPoint2:NSMakePoint(cx-95,y0+200)];
 [b curveToPoint:NSMakePoint(cx-178,y0+150) controlPoint1:NSMakePoint(cx-140,y0+222) controlPoint2:NSMakePoint(cx-176,y0+205)];
 [b curveToPoint:NSMakePoint(cx-120,y0) controlPoint1:NSMakePoint(cx-180,y0+80) controlPoint2:NSMakePoint(cx-165,y0)];
 [b closePath];
 [[NSColor colorWithCalibratedRed:0.10 green:0.16 blue:0.20 alpha:1] setFill];[b fill];[[NSColor colorWithCalibratedWhite:1 alpha:0.03] setFill];[b fill];
 [line setStroke];b.lineWidth=1.5;[b stroke];
 if(ps)diagram_shape([NSBezierPath bezierPathWithRoundedRect:d.touchpad xRadius:8 yRadius:8],TARGET(16),sel==16,0.18);
 NSPoint sticks[2]={d.ls,d.rs};
 for(int i=0;i<2;i++){
  NSBezierPath *ring=[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(sticks[i].x-24,sticks[i].y-24,48,48)];[diagram_color(DIAGRAM_MOVE) setStroke];ring.lineWidth=1.3;[ring stroke];
  diagram_shape([NSBezierPath bezierPathWithOvalInRect:[self shapeOf:4+i pad:&d]],TARGET(4+i),sel==4+i,0.25);
 }
 for(int p=12;p<=15;p++)diagram_shape([NSBezierPath bezierPathWithRoundedRect:[self shapeOf:p pad:&d] xRadius:3 yRadius:3],TARGET(p),sel==p,0.3);
 for(int p=0;p<4;p++){NSRect r=[self shapeOf:p pad:&d];diagram_shape([NSBezierPath bezierPathWithOvalInRect:r],TARGET(p),sel==p,0.3);
  diagram_text(settings_pad_button(p,family),r,10,NSFontWeightBold,NSColor.whiteColor,NSTextAlignmentCenter);}
 for(int p=10;p<=11;p++)diagram_shape([NSBezierPath bezierPathWithRoundedRect:[self shapeOf:p pad:&d] xRadius:6 yRadius:6],TARGET(p),sel==p,0.3);
 /* callouts */
 NSColor *dim=[NSColor colorWithCalibratedWhite:0.7 alpha:1];
 for(int s=0;s<2;s++)for(int i=0;i<d.count[s];i++){
  DiagramCall c=d.calls[s][i];NSRect row=diagram_call_row(&d,s,i);int a=c.stick?KEYMAP_MOVE_UP:TARGET(c.physical);
  BOOL panel=!c.stick&&c.physical>=0&&c.physical==runtime_gamepad_settings_button();   /* the settings panel's button */
  NSColor *col=panel?diagram_color(DIAGRAM_SYSTEM):a>=0?diagram_color(diagram_kind(a)):[NSColor colorWithCalibratedWhite:0.6 alpha:1];
  CGFloat edge=s?row.origin.x-8:NSMaxX(row)+8,elbow=s?edge-18:edge+18;
  NSBezierPath *leader=[NSBezierPath bezierPath];[leader moveToPoint:c.at];
  [leader lineToPoint:NSMakePoint(elbow,row.origin.y+13)];[leader lineToPoint:NSMakePoint(edge,row.origin.y+13)];
  [[col colorWithAlphaComponent:c.physical==sel&&sel>=0?1:0.45] setStroke];leader.lineWidth=c.physical==sel&&sel>=0?2:1;[leader stroke];
  [col setFill];[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.at.x-2.5,c.at.y-2.5,5,5)] fill];
  NSString *button=c.stick?(c.stick==1?settings_text("左摇杆","Left stick"):settings_text("右摇杆","Right stick")):
   c.physical==4||c.physical==5?[settings_pad_button(c.physical,family) stringByAppendingString:settings_text("（按下）"," (press)")]:settings_pad_button(c.physical,family);
  NSString *what=panel?settings_text("设置面板","Settings panel"):c.stick?(c.stick==1?settings_text("移动","Move"):settings_text("视角","Camera")):a>=0?diagram_action_name(a):@"—";
  NSTextAlignment al=s?NSTextAlignmentLeft:NSTextAlignmentRight;
  if(c.physical==sel&&sel>=0){NSBezierPath *hl=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(row,-4,-1) xRadius:5 yRadius:5];
   [[NSColor colorWithCalibratedWhite:1 alpha:0.10] setFill];[hl fill];[[NSColor whiteColor] setStroke];hl.lineWidth=1.5;[hl stroke];}
  diagram_text(button,NSMakeRect(row.origin.x,row.origin.y,row.size.width,12),9.5,NSFontWeightMedium,dim,al);
  diagram_text(what,NSMakeRect(row.origin.x,row.origin.y+12,row.size.width,14),11,NSFontWeightSemibold,col,al);
 }
#undef TARGET
 static const char *const families[GAMEPAD_TYPE_COUNT]={"","Xbox","PlayStation","Switch"};
 diagram_text([NSString stringWithFormat:@(TOTK_UI("手柄（%s） · 设置面板：%@","Controller (%s) · settings panel: %@")),families[family],settings_panel_pad_text(family)],
  NSMakeRect(cx-150,y0+226,300,16),11,NSFontWeightSemibold,dim,NSTextAlignmentCenter);
 diagram_missing_line(1,NSMakeRect(8,y0+252,self.bounds.size.width-16,16));
}
@end
