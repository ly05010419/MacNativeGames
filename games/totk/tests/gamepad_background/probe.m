/* Real controller routing and saved policy, with the HID state supplied locally; no game files or physical pad needed. */
#include "../../runtime/gamepad.m"
#include "../dsu/motion_stubs.h"
#import <objc/runtime.h>

/* Isolate this process from any real controller the user is currently holding.
   Only the framework enumeration is replaced; all runtime routing stays real. */
static NSArray *testControllers;
static NSArray *test_controllers(id cls, SEL selector) { (void)cls; (void)selector; return testControllers; }
@interface TestConnectedController : NSObject
@end
@implementation TestConnectedController
-(id)extendedGamepad { return @YES; } /* presence only: never passed to refresh_gamepads */
-(NSString *)productCategory { return @"Xbox One"; }
-(NSString *)vendorName { return @"Test controller"; }
@end

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
static uint64_t testNow=1000000000;
uint64_t runtime_now_ns(void) { return testNow; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
void runtime_input_mouse(int button, int down) { (void)button; (void)down; }
int runtime_gyro_enabled(void) { return 0; }
void runtime_motion_mouse(int on) { (void)on; }
void runtime_motion_feed_mouse(const double a[3], const double g[3], uint64_t n) { (void)a; (void)g; (void)n; }
uint64_t runtime_keyboard_buttons(void) { return 0; }

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void write_word(const char *path, const char *word) { FILE *f = fopen(path, "w"); fputs(word, f); fclose(f); }

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    char controls[1024], saved[1024], word[32] = {0};
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/controller-background.txt", argv[1]);
    setenv("TOTK_CONTROLS", controls, 1);
    testControllers = @[];
    Method controllersMethod = class_getClassMethod(GCController.class, @selector(controllers));
    CHECK(controllersMethod != NULL);
    IMP originalControllers = method_setImplementation(controllersMethod, (IMP)test_controllers);
    load_controller_map();
    GCController.shouldMonitorBackgroundEvents = YES;
    load_gamepad_background();
    CHECK(!runtime_gamepad_background_enabled() && !GCController.shouldMonitorBackgroundEvents);
    CHECK(access(saved, F_OK) != 0);   /* loading the default does not create a preference */

    /* Presence is enumerated, not dependent on receiving the first input event. */
    int connected = -1;
    float rawX = 99, rawY = 99;
    runtime_gamepad_clear();
    runtime_gamepad_stick_raw(&rawX, &rawY, NULL, NULL, &connected);
    CHECK(!connected && rawX == 0 && rawY == 0);
    hidPad = (HIDPadState){.present = 1}; /* hot-plug, no report/refresh yet */
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(connected);
    runtime_gamepad_clear();
    runtime_gamepad_stick_raw(&rawX, &rawY, NULL, NULL, &connected);
    CHECK(connected && rawX == 0 && rawY == 0); /* opening a panel clears input, not hardware */
    hidPad.present = 0; /* hot-unplug, again without a refresh */
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(!connected);
    testControllers = @[[TestConnectedController new]];
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(connected);
    runtime_gamepad_clear();
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(connected);
    testControllers = @[];
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(!connected);

    RuntimeGamepadState out;
    float lt = -1, rt = -1;
    /* HID fallback: digital ZL/ZR are raw 0/1 triggers, independent of calibration. */
    hidPad = (HIDPadState){.present = 1};
    runtime_gamepad_clear();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0); /* no first report */
    hidPad.buttons = 1ULL << 8; refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 1 && rt == 0);
    runtime_gamepad_read(&out); CHECK(out.buttons == controllerMap[8]);
    hidPad.buttons = 1ULL << 9; refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 1);
    runtime_gamepad_read(&out); CHECK(out.buttons == controllerMap[9]);
    hidPad.buttons = (1ULL << 8) | (1ULL << 9);
    runtime_gamepad_clear(); runtime_gamepad_text_capture(1); /* real panel-open order */
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 1 && rt == 1);
    runtime_gamepad_read(&out); CHECK(!out.buttons); /* UI owns input, raw diagnostics still live */
    runtime_gamepad_text_read(&out); CHECK(out.buttons == ((1ULL << 8) | (1ULL << 9)));
    runtime_gamepad_text_capture(0);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 1 && rt == 1);
    runtime_gamepad_read(&out); CHECK(!out.buttons); /* held triggers wait for release */
    hidPad.buttons = 0; refresh_gamepads(); runtime_gamepad_read(&out);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    CHECK(runtime_gamepad_game_input());
    hidPad.buttons = (1ULL << 8) | (1ULL << 9); refresh_gamepads();
    runtime_gamepad_read(&out); CHECK(out.buttons == (controllerMap[8] | controllerMap[9]));
    hidPad.present = 0; refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    runtime_gamepad_read(&out); CHECK(!out.buttons && !out.connected);

    /* Apple's writable snapshot exercises actual analog GC elements, no hardware. */
    GCController *snapshot = [GCController controllerWithExtendedGamepad];
    CHECK(snapshot.isSnapshot && snapshot.extendedGamepad != nil);
    testControllers = @[snapshot];
    runtime_gamepad_clear();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    [snapshot.extendedGamepad.leftTrigger setValue:.25f];
    [snapshot.extendedGamepad.rightTrigger setValue:.75f];
    refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(fabsf(lt-.25f)<1e-6f && fabsf(rt-.75f)<1e-6f);
    runtime_gamepad_read(&out);
    CHECK(out.buttons == ((snapshot.extendedGamepad.leftTrigger.isPressed ? controllerMap[8] : 0) |
                          (snapshot.extendedGamepad.rightTrigger.isPressed ? controllerMap[9] : 0)));
    runtime_gamepad_focus(0); refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    runtime_gamepad_read(&out); CHECK(!out.buttons); /* default-off background policy also clears trigger diagnostics */
    runtime_gamepad_focus(1);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(fabsf(lt-.25f)<1e-6f && fabsf(rt-.75f)<1e-6f);
    runtime_gamepad_clear(); runtime_gamepad_text_capture(1);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(fabsf(lt-.25f)<1e-6f && fabsf(rt-.75f)<1e-6f);
    runtime_gamepad_read(&out); CHECK(!out.buttons);
    runtime_gamepad_text_capture(0);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(fabsf(lt-.25f)<1e-6f && fabsf(rt-.75f)<1e-6f);
    [snapshot.extendedGamepad.leftTrigger setValue:0];
    [snapshot.extendedGamepad.rightTrigger setValue:0];
    refresh_gamepads(); runtime_gamepad_read(&out);
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    CHECK(runtime_gamepad_game_input());
    [snapshot.extendedGamepad.leftTrigger setValue:.5f]; refresh_gamepads();
    testControllers = @[]; refresh_gamepads();
    runtime_gamepad_trigger_raw(&lt, &rt); CHECK(lt == 0 && rt == 0);
    runtime_gamepad_read(&out); runtime_gamepad_read(&out); CHECK(!out.buttons && !out.connected);
    runtime_gamepad_clear();

    hidPad = (HIDPadState){.present = 1, .buttons = 1, .lx = 12345, .ry = -23456};
    refresh_gamepads();
    runtime_gamepad_read(&out);
    CHECK(out.buttons && out.lx == 12345 && out.ry == -23456);
    runtime_gamepad_focus(0); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !out.ry);
    runtime_gamepad_stick_raw(NULL, NULL, NULL, NULL, &connected); CHECK(connected);
    runtime_gamepad_read(&out); CHECK(!out.buttons && !out.lx && !out.ry); /* detection cannot re-enable background input */
    hidPad.buttons = 2; refresh_gamepads(); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx);   /* the HID fallback cannot bypass the default-off policy */

    runtime_gamepad_background_set(1);
    CHECK(runtime_gamepad_background_enabled() && GCController.shouldMonitorBackgroundEvents);
    CHECK(runtime_setting_word("controller-background.txt", word, sizeof word) && !strcmp(word, "on\n"));
    runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 12345 && out.ry == -23456);
    runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 12345);   /* held through background frames */
    runtime_gamepad_focus(1); runtime_gamepad_focus(0); runtime_gamepad_read(&out);
    CHECK(out.buttons && out.lx == 12345);   /* opting in preserves input across focus loss */
    hidPad.buttons = 0; hidPad.lx = hidPad.ry = 0; refresh_gamepads(); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !out.ry);   /* releases are delivered in the background */

    hidPad.buttons = 1; hidPad.lx = 20000; refresh_gamepads();
    runtime_gamepad_background_set(0); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !GCController.shouldMonitorBackgroundEvents);
    CHECK(runtime_setting_word("controller-background.txt", word, sizeof word) && !strcmp(word, "off\n"));
    refresh_gamepads(); runtime_gamepad_read(&out); CHECK(!out.buttons && !out.lx);
    runtime_gamepad_focus(1); runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 20000);

    /* A stationary HID stick must immediately use an edited/persisted deadzone. */
    hidX = .2f; hidY = hidZ = hidRz = 0; hidOrder = 0; memset(hidButtons,0,sizeof hidButtons);
    runtime_gamepad_deadzone_set(.3f); runtime_gamepad_read(&out);
    CHECK(out.lx == 0 && runtime_gamepad_deadzone_get() == .3f);
    runtime_gamepad_deadzone_set(.1f); runtime_gamepad_read(&out);
    CHECK(out.lx > 3600 && out.lx < 3700);
    stickDeadzone[0] = stickDeadzone[1] = .12f; load_stick_deadzone(); CHECK(stickDeadzone[0] == .1f && stickDeadzone[1] == .1f);
    runtime_gamepad_deadzone_set(NAN); CHECK(stickDeadzone[0] == .1f && stickDeadzone[1] == .1f);
    runtime_gamepad_deadzone_set(2); CHECK(stickDeadzone[0] == .5f && stickDeadzone[1] == .5f);

    hidSampleAxes=15;RuntimeGamepadCalibrationSample ownerSample;
    runtime_gamepad_calibration_sample(&ownerSample);CHECK(ownerSample.valid);
    /* Real calibration -> cached HID -> game state, with independent L/R data. */
    runtime_gamepad_deadzone_set(.1f);
    RuntimeGamepadCalibration custom={.left={.centerX=.1f,.centerY=-.2f,.scaleX=2,.scaleY=1.5f},.right={.centerX=-.1f,.centerY=.1f,.scaleX=1.2f,.scaleY=1.1f}};
    hidX=.35f;hidY=.2f;hidZ=.3f;hidRz=-.1f;
    runtime_gamepad_calibration_set(&custom); runtime_gamepad_read(&out);
    CHECK(out.lx>=14562 && out.lx<=14564 && out.ly==0);
    CHECK(out.rx>=13833 && out.rx<=13835 && out.ry==0);
    int rightBefore=out.rx;
    custom.left.centerX=.35f;
    runtime_gamepad_calibration_set(&custom);runtime_gamepad_read(&out);
    CHECK(out.lx==0 && out.rx==rightBefore); /* changing left never changes right */
    calibration=(RuntimeGamepadCalibration){.left={.scaleX=1, .scaleY=1},.right={.scaleX=1, .scaleY=1}};
    load_calibration(); /* restart-equivalent loading, not the UI's in-memory copy */
    RuntimeGamepadCalibration restored;runtime_gamepad_calibration_get(&restored);
    CHECK(fabsf(restored.left.centerX-.35f)<1e-6f && restored.left.scaleX==2);
    CHECK(fabsf(restored.right.centerX+.1f)<1e-6f && fabsf(restored.right.scaleX-1.2f)<1e-6f);
    CHECK(!calibration_hid_matches()); /* archived file is never bound to whichever pad happens to be connected */
    runtime_gamepad_calibration_sample(&ownerSample);runtime_gamepad_calibration_set(&restored);
    runtime_gamepad_read(&out);CHECK(out.lx==0 && out.rx==rightBefore);

    /* Old eight-value profiles load with legacy coordinates. New files retain asymmetric gains. */
    setting_write("controller-calibration.txt","0.1235 0 2 1 0 0 1 1","legacy test");
    load_calibration(); CHECK(!calibration.stableHID && calibration.left.scaleNegX==2);
    CHECK(fabsf(stick_calibrate(0,.1235f))<1e-6f && !calibration_hid_matches());
    runtime_gamepad_calibration_get(&restored);runtime_gamepad_calibration_sample(&ownerSample);
    runtime_gamepad_calibration_set(&restored); /* explicit calibration of the sampled device activates it */
    /* At 0% deadzone quantization/ADC residuals must not trap UI capture; buttons still block release. */
    hidX=.123456f;hidY=hidZ=hidRz=0;runtime_gamepad_deadzone_set(0);
    runtime_gamepad_text_capture(1); runtime_gamepad_text_capture(0);
    runtime_gamepad_read(&out);CHECK(textCapture==0);
    hidX=.3f; hid_pad_compute();refresh_gamepads();runtime_gamepad_read(&out);CHECK(out.lx>1000);
    runtime_gamepad_deadzone_set(.5f); hidX=.3f;hid_pad_compute();refresh_gamepads();
    runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);CHECK(textCapture==2); /* large game DZ is not neutral */
    hidX=.123456f;hidButtons[1]=1;hid_pad_compute();refresh_gamepads();runtime_gamepad_read(&out);CHECK(textCapture==2);
    hidButtons[1]=0;hid_pad_compute();refresh_gamepads();runtime_gamepad_read(&out);CHECK(textCapture==0);
    custom=(RuntimeGamepadCalibration){.left={.centerX=.15f,.centerY=-.1f,.scaleX=1/.65f,.scaleY=1/.8f,
        .scaleNegX=1/.95f,.scaleNegY=1/.6f},.right={.scaleX=1,.scaleY=1},.stableHID=1};
    runtime_gamepad_calibration_sample(&ownerSample);runtime_gamepad_calibration_set(&custom);load_calibration();
    CHECK(!calibration_hid_matches());
    runtime_gamepad_calibration_sample(&ownerSample);runtime_gamepad_calibration_set(&custom);
    CHECK(calibration.stableHID && fabsf(calibration.left.scaleNegX-1/.95f)<1e-6f);
    CHECK(stick_calibrate(0,.8f)>.9999f && stick_calibrate(0,-.8f)<-.9999f);
    CHECK(stick_calibrate(1,.7f)>.9999f && stick_calibrate(1,-.7f)<-.9999f);
    /* Stable Switch Pro samples/output cannot change as the adaptive range learns or after reconnect. */
    hidOrder=GAMEPAD_HID_SWITCH_PRO;hidX=.15f;hidY=.1f;hidZ=hidRz=0;
    runtime_gamepad_deadzone_set(0);hid_pad_compute(); CHECK(hidPad.lx==0 && hidPad.ly==0);
    CHECK(fabsf(hidRawLx-.15f)<1e-6f && fabsf(hidRawLy+.1f)<1e-6f);
    hidX=.8f;hid_pad_compute();int calibratedEdge=hidPad.lx;CHECK(calibratedEdge>32760);
    memset(hidReach,0,sizeof hidReach);hid_pad_compute();CHECK(hidPad.lx==calibratedEdge);
    hid_pad_reset();hidOrder=GAMEPAD_HID_SWITCH_PRO;hidX=.15f;hidY=.1f;hidZ=hidRz=0;
    hid_pad_compute();CHECK(!calibration_hid_matches() && hidPad.lx!=0); /* reconnect never inherits an unverified identity */
    hidSampleAxes=15;runtime_gamepad_calibration_sample(&ownerSample);runtime_gamepad_calibration_set(&custom);
    CHECK(hidPad.lx==0 && hidPad.ly==0); /* explicit re-calibration restores this connection */
    runtime_gamepad_calibration_reset(); CHECK(!calibration.stableHID);
    hidX=.65f;hidY=hidZ=hidRz=0;hid_pad_compute();
    CHECK(hidPad.lx>32760 && fabsf(hidRawLx-.65f)<1e-6f); /* uncalibrated adaptive reach retained */

    /* Calibration has its own source-coherent samples; focus/cache clears cannot fake neutral. */
    RuntimeGamepadCalibrationSample sample;testControllers=@[snapshot];hidPad.present=0;
    [snapshot.extendedGamepad.leftThumbstick.xAxis setValue:.2f];[snapshot.extendedGamepad.leftThumbstick.yAxis setValue:.1f];
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && fabsf(sample.lx-.2f)<1e-6f);
    uint64_t generation=sample.generation;
    runtime_gamepad_background_set(0);runtime_gamepad_focus(0);
    runtime_gamepad_calibration_sample(&sample);CHECK(!sample.valid && sample.generation>generation);
    runtime_gamepad_focus(1);runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && fabsf(sample.lx-.2f)<1e-6f);
    generation=sample.generation;
    runtime_gamepad_focus(0);runtime_gamepad_focus(1); /* interruption wholly between UI ticks stays latched */
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation>generation);
    runtime_gamepad_background_set(1);generation=sample.generation;runtime_gamepad_focus(0);
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation==generation);
    runtime_gamepad_focus(1);runtime_gamepad_background_set(0);
    /* Foreign controller reports must never supply selected controller coordinates. */
    GCController *other=[GCController controllerWithExtendedGamepad];
    runtime_gamepad_switch_left((__bridge void*)other,3800,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(fabsf(sample.lx-.2f)<1e-6f);
    generation=sample.generation;
    runtime_gamepad_switch_left((__bridge void*)snapshot,3200,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.generation==generation && fabsf(sample.lx-.2f)<1e-6f); /* working native remains authoritative */
    [snapshot.extendedGamepad.leftThumbstick.xAxis setValue:0];[snapshot.extendedGamepad.leftThumbstick.yAxis setValue:0];
    runtime_gamepad_switch_left((__bridge void*)snapshot,2100,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation==generation && sample.lx==0); /* normal native centre ignores small raw bias */
    testNow+=300000000;
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation==generation && sample.lx==0); /* stale auxiliary data cannot invalidate normal native */
    runtime_gamepad_switch_left((__bridge void*)snapshot,3200,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(!sample.valid); /* delayed fallback cannot calibrate stuck zero */
    testNow+=250000000;runtime_gamepad_switch_left((__bridge void*)snapshot,3200,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation>generation && sample.lx>.5f);
    generation=sample.generation;
    runtime_gamepad_switch_left((__bridge void*)snapshot,2100,2048); /* final centre: same domain, not native zero */
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.generation==generation && sample.lx>0 && sample.lx<.03f);
    rawLeft.valid=0;refresh_gamepads();runtime_gamepad_switch_left((__bridge void*)snapshot,2100,2048);
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.generation>generation); /* change and return between ticks */
    testControllers=@[other];runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid);
    testControllers=@[];hid_pad_reset();hidPad.present=1;
    runtime_gamepad_calibration_sample(&sample);CHECK(!sample.valid); /* attached synthetic zero is not primed */
    hidSampleAxes=15;hidX=.1f;hidY=hidZ=hidRz=0;hid_pad_compute();
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid);generation=sample.generation;
    runtime_gamepad_calibration_sample(&sample);CHECK(sample.valid && sample.generation==generation); /* change-only stationary pad */

    /* Two same-model devices must not share centre compensation, or lock capture at neutral. */
    hidPad.present=0;rawLeft=(SwitchLeftFallback){0};rawRight=(SwitchLeftFallback){0};
    [snapshot.extendedGamepad.leftTrigger setValue:0];[snapshot.extendedGamepad.rightTrigger setValue:0];
    [snapshot.extendedGamepad.leftThumbstick.xAxis setValue:.2f];
    [snapshot.extendedGamepad.leftThumbstick.yAxis setValue:0];
    testControllers=@[snapshot,other];runtime_gamepad_calibration_sample(&ownerSample);
    custom=(RuntimeGamepadCalibration){.left={.centerX=.2f,.scaleX=1,.scaleY=1},.right={.scaleX=1,.scaleY=1}};
    runtime_gamepad_calibration_set(&custom);runtime_gamepad_deadzone_set(0);runtime_gamepad_read(&out);
    CHECK(out.lx==0 && !captureStickActive); /* other controller at raw zero must not become -.2 */
    testControllers=@[other,snapshot];refresh_gamepads();runtime_gamepad_read(&out);CHECK(out.lx==0); /* array order isn't identity */
    [other.extendedGamepad.leftThumbstick.xAxis setValue:.5f];refresh_gamepads();runtime_gamepad_read(&out);CHECK(out.lx>16000);
    [other.extendedGamepad.leftThumbstick.xAxis setValue:0];runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);CHECK(textCapture==0);
    detach_gamepad(snapshot);CHECK(!calibration_source_matches(snapshot,0,0)); /* even object reuse/re-attachment cannot inherit */
    testControllers=@[snapshot];[snapshot.extendedGamepad.leftThumbstick.xAxis setValue:.10f];
    runtime_gamepad_deadzone_set(.12f);runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);CHECK(textCapture==0);
    runtime_gamepad_deadzone_set(0);runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);CHECK(textCapture==0); /* configured DZ independent */
    [snapshot.extendedGamepad.leftThumbstick.xAxis setValue:.2f];runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);CHECK(textCapture==2);
    /* Raw fallback expires at read-time with no intervening refresh. Other device/right/buttons remain held. */
    [snapshot.extendedGamepad.leftThumbstick.xAxis setValue:0];
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);testNow+=250000000;
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);runtime_gamepad_text_capture(0);
    runtime_gamepad_read(&out);CHECK(textCapture==2 && captureRawLeftActive);
    testNow+=250000001;runtime_gamepad_read(&out);CHECK(textCapture==0);
    testControllers=@[snapshot,other];[other.extendedGamepad.rightThumbstick.xAxis setValue:.5f];
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);testNow+=250000000;runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);
    runtime_gamepad_text_capture(0);testNow+=250000001;runtime_gamepad_read(&out);CHECK(textCapture==2); /* unrelated right axis still held */
    [other.extendedGamepad.rightThumbstick.xAxis setValue:0];[other.extendedGamepad.buttonA setValue:1];
    refresh_gamepads();runtime_gamepad_read(&out);CHECK(textCapture==2); /* unrelated button still held */
    [other.extendedGamepad.buttonA setValue:0];refresh_gamepads();runtime_gamepad_read(&out);CHECK(textCapture==0);
    testControllers=@[snapshot];
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);testNow+=250000000;runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);
    runtime_gamepad_switch_right((__bridge void*)snapshot,4000,2048);testNow+=200000000;
    runtime_gamepad_switch_right((__bridge void*)snapshot,4000,2048);runtime_gamepad_text_capture(0);
    testNow+=60000000;runtime_gamepad_read(&out);CHECK(textCapture==2); /* left expired, right fallback still fresh */
    testNow+=200000000;runtime_gamepad_read(&out);CHECK(textCapture==0); /* both expired without refreshing */
    /* A calibration measured in raw HID coordinates must not offset the synthetic native zero,
       either in gameplay or after the settings panel releases capture. */
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);testNow+=250000000;
    runtime_gamepad_switch_left((__bridge void*)snapshot,4000,2048);
    runtime_gamepad_calibration_sample(&ownerSample);CHECK(ownerSample.valid);
    custom=(RuntimeGamepadCalibration){.left={.centerX=.2f,.scaleX=1,.scaleY=1},.right={.scaleX=1,.scaleY=1}};
    runtime_gamepad_calibration_set(&custom);
    CHECK(calibration_source_matches(snapshot,0,1) && !calibration_source_matches(snapshot,0,0));
    runtime_gamepad_switch_left((__bridge void*)snapshot,2457,2048);
    runtime_gamepad_text_capture(1);runtime_gamepad_text_capture(0);runtime_gamepad_read(&out);
    CHECK(textCapture==0 && gamepadNativeLx==0 && !captureStickActive);
    testNow+=250000001;runtime_gamepad_read(&out);CHECK(out.lx==0 && out.ly==0);
    testControllers=@[];

    /* Restart-equivalent preference loading; only a valid opt-in enables background events. */
    write_word(saved, "on\n"); load_gamepad_background();
    CHECK(runtime_gamepad_background_enabled() && GCController.shouldMonitorBackgroundEvents);
    write_word(saved, "off\n"); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    write_word(saved, "on garbage\n"); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    unlink(saved); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    unsetenv("TOTK_CONTROLS"); runtime_gamepad_background_set(1);
    CHECK(runtime_gamepad_background_enabled() && access(saved, F_OK) != 0);
    method_setImplementation(controllersMethod, originalControllers);
    printf("{\"status\":\"pass\",\"probe\":\"gamepad-background\",\"checks\":%u}\n", checks);
    return 0;
}}
