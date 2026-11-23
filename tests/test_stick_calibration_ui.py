"""Exercise the actual native calibration view's single-button state machine."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'

PRELUDE = r'''
#import <Cocoa/Cocoa.h>
#include <assert.h>
#include <math.h>
#include "runtime.h"
#define TOTK_UI(c,e) c
static NSString *settings_text(const char*c,const char*e){(void)e;return @(c);}
static RuntimeGamepadCalibration saved={.left={.scaleX=1, .scaleY=1},.right={.scaleX=1, .scaleY=1}};
static int saves, connection=1;
static float triggerLeft,triggerRight,renderedTriggers[2];
static NSPoint renderedStickDots[2];
static uint64_t now=1000000000;
static RuntimeGamepadCalibrationSample sampleFixture={.valid=1,.generation=1};
void runtime_gamepad_calibration_sample(RuntimeGamepadCalibrationSample *out){*out=sampleFixture;out->lt=triggerLeft;out->rt=triggerRight;}
void runtime_gamepad_calibration_get(RuntimeGamepadCalibration*c){*c=saved;}
void runtime_gamepad_calibration_set(const RuntimeGamepadCalibration*c){saved=*c;saves++;}
static float dz[2]={.12f,.12f};
float runtime_gamepad_stick_deadzone_get(int r){return dz[!!r];}
void runtime_gamepad_stick_deadzone_set(int r,float v){dz[!!r]=v;}
void runtime_gamepad_stick_raw(float*a,float*b,float*c,float*d,int*e){*a=*b=*c=*d=0;*e=connection;}
void runtime_gamepad_trigger_raw(float*a,float*b){*a=triggerLeft;*b=triggerRight;}
void runtime_gamepad_stick_preview(int r,float x,float y,float*a,float*b){(void)r;*a=x;*b=y;}
uint64_t runtime_now_ns(void){return now;}
'''
MAIN = r'''
static void render(TOTKStickCalibrationView*v){
NSBitmapImageRep *rep=[v bitmapImageRepForCachingDisplayInRect:v.bounds];
[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
}
int main(){@autoreleasepool{
[NSApplication sharedApplication];
NSWindow*w=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,800,360) styleMask:0 backing:NSBackingStoreBuffered defer:NO];
TOTKStickCalibrationView*v=[[TOTKStickCalibrationView alloc]initWithFrame:NSMakeRect(0,0,800,360)];
[w.contentView addSubview:v];[v.timer invalidate];
/* Already connected on first construction, without one controller input. */
assert(v.calibrateButton.enabled);
connection=0;[v tick];assert(!v.calibrateButton.enabled);
connection=1;[v tick];assert(v.calibrateButton.enabled);
NSUInteger buttons=0;for(NSView*child in v.subviews)if([child isKindOfClass:NSButton.class])buttons++;
assert(buttons==1);assert(!settings_calibration_active());
assert([[v valueText:CAL_LEFT] length]==0&&[[v valueText:CAL_RIGHT] length]==0);
/* The larger circles and reordered controls fit the compact game panel too. */
for(int h=280;h<=400;h+=40){
[v setFrameSize:NSMakeSize(710,h)];[v relayout];
CGFloat r=[v wellRadius];NSPoint left=[v wellCenter:CAL_LEFT],right=[v wellCenter:CAL_RIGHT];
assert(r>=76);assert(left.x+r+24<right.x-r-20);
assert(left.y-r>=16&&left.y+r+31<=h-80);
assert(NSMaxY(v.calibrateButton.frame)<=NSMinY(v.deadzone.frame));
assert(NSMaxY(v.deadzone.frame)<=h&&NSMaxY(v.deadzoneLabel.frame)<=h);
}
[v setFrameSize:NSMakeSize(800,360)];

/* Full per-axis diagonals must fit the circular preview, without changing the samples. */
const float directions[][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{-1,-1},{1,-1},{.25f,.5f},{0,0}};
calRun[0].phase=calRun[1].phase=CAL_RECORD;
for(unsigned i=0;i<sizeof(directions)/sizeof(directions[0]);i++){
    float x=directions[i][0],y=directions[i][1];
    [v setValue:@(x) forKey:@"rawLX"];[v setValue:@(y) forKey:@"rawLY"];
    [v setValue:@(-x) forKey:@"rawRX"];[v setValue:@(-y) forKey:@"rawRY"];
    render(v);
    for(int stick=CAL_LEFT;stick<=CAL_RIGHT;stick++){
        NSPoint c=[v wellCenter:stick],p=renderedStickDots[stick];
        CGFloat dx=p.x-c.x,dy=c.y-p.y,r=[v wellRadius],len=hypot(dx,dy);
        CGFloat sign=stick==CAL_LEFT?1:-1;
        assert(len+6.5<=r&&len>=fmin(1,hypot(x,y))*(r-8)-1e-6);
        assert(fabs(dx*y-dy*x)<1e-5&&sign*(dx*x+dy*y)>=0);
        if(x==0&&y==0)assert(len==0);
    }
}
[v setValue:@1 forKey:@"rawLX"];[v setValue:@1 forKey:@"rawLY"];
[v setValue:@(-1) forKey:@"rawRX"];[v setValue:@(-1) forKey:@"rawRY"];
render(v);
assert([[v valueForKey:@"rawLX"] floatValue]==1&&[[v valueForKey:@"rawRX"] floatValue]==-1);
calRun[0].phase=calRun[1].phase=CAL_IDLE;render(v);
for(int stick=CAL_LEFT;stick<=CAL_RIGHT;stick++)assert(NSEqualPoints(renderedStickDots[stick],[v wellCenter:stick]));
[v setValue:@0 forKey:@"rawLX"];[v setValue:@0 forKey:@"rawLY"];
[v setValue:@0 forKey:@"rawRX"];[v setValue:@0 forKey:@"rawRY"];

/* Product contract: triggers display only during calibration, not merely when settings is open. */
triggerLeft=.25f;triggerRight=.75f;[v tick];render(v);
assert(renderedTriggers[0]==0&&renderedTriggers[1]==0);
[v calibrate:v.calibrateButton];[v tick];render(v);
assert(renderedTriggers[0]==.25f&&renderedTriggers[1]==.75f);
calRun[0].phase=CAL_READY;calRun[1].phase=CAL_RECORD;render(v);
assert(renderedTriggers[0]==.25f&&renderedTriggers[1]==.75f);
[v calibrate:v.calibrateButton];render(v);assert(renderedTriggers[0]==0&&renderedTriggers[1]==0);
[v calibrate:v.calibrateButton];connection=0;[v tick];render(v);
assert(renderedTriggers[0]==0&&renderedTriggers[1]==0&&!settings_calibration_active());
connection=1;triggerLeft=triggerRight=0;[v tick];
/* Slider changes are independent, and the rings use the matching value. */
v.deadzone.floatValue=23;[v changeDeadzone:v.deadzone];assert(fabsf(dz[0]-.23f)<1e-6&&fabsf(dz[1]-.12f)<1e-6);
v.rightDeadzone.floatValue=7;[v changeDeadzone:v.rightDeadzone];assert(fabsf(dz[0]-.23f)<1e-6&&fabsf(dz[1]-.07f)<1e-6);
[v calibrate:v.calibrateButton];assert(calRun[0].phase==CAL_AWAIT_CENTER&&calRun[1].phase==CAL_AWAIT_CENTER);
assert([v.calibrateButton.title isEqualToString:@"取消"]);
[v calibrate:v.calibrateButton];assert(!settings_calibration_active()&&saves==0);
/* Held-off-centre input cannot be mistaken for a neutral centre. */
[v calibrate:v.calibrateButton];[v setValue:@.8 forKey:@"rawLX"];
[v stepStick:0];now+=1000000000;[v stepStick:0];assert(calRun[0].phase==CAL_AWAIT_CENTER);
[v setValue:@0 forKey:@"rawLX"];[v stepStick:0];[v stepStick:1];
now+=100000000;[v stepStick:0];[v stepStick:1];
now+=600000000;[v stepStick:0];[v stepStick:1];
assert(calRun[0].phase==CAL_RECORD&&calRun[1].phase==CAL_RECORD);
/* A small circular wobble must not light the outer ring or finish. */
for(int i=0;i<CAL_SECTORS;i++){
float a=(i+.5)*2*M_PI/CAL_SECTORS;
[v setValue:@(.3*cos(a)) forKey:@"rawLX"];[v setValue:@(.3*sin(a)) forKey:@"rawLY"];
now+=100000000;[v stepStick:0];[v stepStick:0];}
for(int i=0;i<CAL_SECTORS;i++)assert(calRun[0].coverage[i]==0);
/* Four cardinal extremes are not a complete sweep. */
for(int i=0;i<4;i++){
float a=i*M_PI/2;
[v setValue:@(.8*cos(a)) forKey:@"rawLX"];[v setValue:@(.8*sin(a)) forKey:@"rawLY"];
[v stepStick:0];[v stepStick:0];}
[v finishIfComplete];assert(saves==0&&calRun[0].phase==CAL_RECORD);
/* Complete left alone remains pending, so Cancel discards everything. */
for(int i=0;i<CAL_SECTORS;i++){
float a=(i+.5)*2*M_PI/CAL_SECTORS;
[v setValue:@(.8*cos(a)) forKey:@"rawLX"];[v setValue:@(.8*sin(a)) forKey:@"rawLY"];
now+=100000000;[v stepStick:0];[v stepStick:0];}
assert(calRun[0].phase==CAL_READY);[v finishIfComplete];assert(saves==0);
[v calibrate:v.calibrateButton];assert(saves==0&&saved.left.scaleX==1&&!settings_calibration_active());
/* Actual full sweeps auto-complete and save exactly once, without another click. */
[v calibrate:v.calibrateButton];
for(int j=0;j<2;j++)calRun[j]=(StickCalibrationRun){.phase=CAL_RECORD,.recordSince=now/1e9};
for(int i=0;i<CAL_SECTORS;i++){
float a=(i+.5)*2*M_PI/CAL_SECTORS;
[v setValue:@(.8*cos(a)) forKey:@"rawLX"];[v setValue:@(.8*sin(a)) forKey:@"rawLY"];
[v setValue:@(.7*cos(a)) forKey:@"rawRX"];[v setValue:@(.7*sin(a)) forKey:@"rawRY"];
now+=100000000;[v stepStick:0];[v stepStick:1];[v stepStick:0];[v stepStick:1];[v finishIfComplete];}
assert(saves==0&&settings_calibration_active());
/* Coverage alone cannot commit while the user is still moving. Continue recording READY travel. */
[v setValue:@.9 forKey:@"rawLX"];[v stepStick:0];assert(calRun[0].posX==.9f);
[v setValue:@0 forKey:@"rawLX"];[v setValue:@0 forKey:@"rawLY"];
[v setValue:@0 forKey:@"rawRX"];[v setValue:@0 forKey:@"rawRY"];
[v setValue:@(NAN) forKey:@"rawLX"];[v finishIfComplete];assert(saves==0);
[v setValue:@0 forKey:@"rawLX"];[v finishIfComplete];
assert(saves==1&&!settings_calibration_active());[v finishIfComplete];assert(saves==1);
assert(saved.stableHID && saved.left.scaleNegX>saved.left.scaleX);
assert(saved.left.scaleX>1.11&&saved.left.scaleX<1.12&&saved.right.scaleX>1.42);
assert([v.calibrateButton.title isEqualToString:@"校准"]);
assert([[v valueForKey:@"calibrationStatus"] containsString:@"完成"]);
triggerLeft=.4f;triggerRight=.9f;[v tick];render(v);
assert(renderedTriggers[0]==0&&renderedTriggers[1]==0); /* auto-complete returns to idle */
triggerLeft=triggerRight=0;
/* Invalid focus-cleared samples must neither record centre nor finish a completed sweep. */
for(int phase=CAL_AWAIT_CENTER;phase<=CAL_READY;phase+=CAL_READY-CAL_AWAIT_CENTER){
 calRun[0]=(StickCalibrationRun){.phase=phase,.settleSince=1,.posX=.9,.negX=-.8};calRun[1]=calRun[0];
 sampleFixture.valid=0;sampleFixture.generation++;now+=1000000000;[v tick];
 assert(saves==1&&calRun[0].phase==CAL_AWAIT_CENTER&&calRun[0].settleSince==0&&calRun[0].posX==0);
 now+=1000000000;[v tick];assert(calRun[0].phase==CAL_AWAIT_CENTER&&calRun[0].settleSince==0);
 sampleFixture.valid=1;sampleFixture.generation++;[v tick];assert(calRun[0].settleSince==0);
}
/* Domain/device transitions reset centre, dwell, travel and sector coverage, even if already READY. */
for(int phase=CAL_RECORD;phase<=CAL_READY;phase++){
 calRun[0]=(StickCalibrationRun){.phase=phase,.centerX=.1,.settleSince=1,.recordSince=1,.posX=.9,.negX=-.8};calRun[1]=calRun[0];
 calRun[0].coverage[0]=2;sampleFixture.generation+=2;[v tick];
 assert(saves==1&&calRun[0].phase==CAL_AWAIT_CENTER&&calRun[0].centerX==0&&calRun[0].coverage[0]==0&&calRun[0].posX==0);
}
calRun[0].phase=calRun[1].phase=CAL_IDLE;
/* Hiding, disconnect and detaching cancel without a write. */
[v calibrate:v.calibrateButton];w.contentView.hidden=YES;[v tick];assert(!settings_calibration_active()&&saves==1);
w.contentView.hidden=NO;[v calibrate:v.calibrateButton];connection=0;[v tick];assert(!settings_calibration_active()&&saves==1);
connection=1;[v tick];[v calibrate:v.calibrateButton];[v removeFromSuperview];assert(!settings_calibration_active()&&saves==1);
puts("single-button calibration: pass");
}}
'''


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS/AppKit')
class StickCalibrationUITests(unittest.TestCase):
    def test_single_button(self):
        source = (RUNTIME / 'settings_panel.h').read_text()
        view = source[source.index('enum { CAL_LEFT'):source.index('\n#else\nstatic int settings_calibration_active')]
        # Observe the value the actual production draw method uses, without changing its expression.
        trigger_expression = 'float v=calRun[stick].phase!=CAL_IDLE?(right?rawRT:rawLT):0;'
        self.assertEqual(view.count(trigger_expression), 1)
        view = view.replace(trigger_expression, trigger_expression + 'renderedTriggers[stick]=v;')
        vector_comment = '/* the vector line from centre to the dot */'
        self.assertEqual(view.count(vector_comment), 1)
        view = view.replace(vector_comment, 'renderedStickDots[stick]=p;\n' + vector_comment)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'probe.m').write_text(PRELUDE + view + MAIN)
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', f'-I{RUNTIME}', str(path / 'probe.m'),
                            '-framework', 'Cocoa', '-o', str(path / 'probe')], check=True, timeout=120)
            run = subprocess.run([str(path / 'probe')], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('single-button calibration: pass', run.stdout)

    def test_open_clears_before_refresh(self):
        source = (RUNTIME / 'settings_panel.h').read_text()
        start = source.index('static void settings_show(int open) {')
        body = source[start:source.index('static void settings_user_added', start)]
        self.assertLess(body.index('runtime_input_clear();'), body.index('runtime_gamepad_text_capture(open);'))
