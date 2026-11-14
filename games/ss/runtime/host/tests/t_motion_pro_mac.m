// Exercise the real IOKit callbacks with fake devices and per-device report buffers; no hardware or manager is opened.
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../dm_input.h"

@interface TestPro : NSObject
@property BOOL synthetic;
@property uint8_t *buffer;
@property IOHIDReportCallback callback;
@property void *context;
@end
@implementation TestPro
@end
static CFTypeRef test_property(IOHIDDeviceRef device, CFStringRef key) {
    TestPro *d = (__bridge id)device;
    return CFEqual(key, CFSTR(kIOHIDGCSyntheticDeviceKey)) && d.synthetic ? kCFBooleanTrue : NULL;
}
static void test_callback(IOHIDDeviceRef device, uint8_t *buffer, CFIndex len, IOHIDReportCallback callback, void *ctx) {
    TestPro *d = (__bridge id)device; assert(len >= 49);
    d.buffer = buffer; d.callback = callback; d.context = ctx;
}
static int sent, player, cleared = -1;
static uint64_t tick = 1000000000, sample_time;
uint64_t now_ns(void) { return tick; }
void dm_motion_set_hid(int p, const float a[3], const float w[3], uint64_t ns) { sent++; player = p; sample_time = ns; assert(a[2] == -1 && w[2] == 0); }
void dm_motion_clear_hid(int p) { cleared = p; }
#define LOG(...) ((void)0)
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceRegisterInputReportCallback test_callback
#include "../motion_pro_mac.h"
#undef IOHIDDeviceGetProperty
#undef IOHIDDeviceRegisterInputReportCallback

static IOHIDDeviceRef hid(TestPro *d) { return (__bridge IOHIDDeviceRef)d; }
static void found(TestPro *d) { pro_found(NULL, kIOReturnSuccess, NULL, hid(d)); }
static void removed(TestPro *d) { pro_removed(NULL, kIOReturnSuccess, NULL, hid(d)); }
static void changed(void) { dm_motion_pro_players(g_pro_controllers, g_pro_slot); }
static void report(TestPro *d) {
    assert(d.callback && d.buffer); memset(d.buffer, 0, 49); d.buffer[0] = 0x30; d.buffer[1] = (uint8_t)(tick / 15000000);
    for (int k = 0; k < 3; k++) d.buffer[18 + 12 * k] = 0x10;   // +4096 z specific force
    d.callback(d.context, kIOReturnSuccess, hid(d), kIOHIDReportTypeInput, 0x30, d.buffer, 49);
    tick += 15000000;
}
int main(void) { @autoreleasepool {
    g_pro_changed = changed;
    TestPro *a = [TestPro new], *b = [TestPro new], *synthetic = [TestPro new]; synthetic.synthetic = YES;
    found(synthetic); assert(!g_pro_devices && !synthetic.callback);
    pro_found(NULL, kIOReturnError, NULL, hid(a)); assert(!g_pro_devices);
    dm_motion_pro_players(1, 2); found(a); found(a);
    assert(g_pro_devices && !g_pro_devices->next && g_pro_player == 2);
    report(a); assert(sent == 3 && player == 2 && sample_time == tick - 15000000);
    a.callback(a.context, kIOReturnError, hid(a), kIOHIDReportTypeInput, 0x30, a.buffer, 49); assert(sent == 3);
    dm_motion_pro_active(0); assert(cleared == 2); report(a); assert(sent == 3);
    dm_motion_pro_active(1); report(a); assert(sent == 6);
    dm_motion_pro_players(2, 0); assert(g_pro_player == -1 && cleared == 2); report(a); assert(sent == 6);
    dm_motion_pro_players(1, 2); found(b);
    assert(a.buffer != b.buffer && a.context != b.context && g_pro_player == -1);
    report(a); report(b); assert(sent == 6);
    removed(b); assert(!b.callback && g_pro_player == 2);
    report(a); assert(sent == 9 && player == 2);
    dm_motion_pro_players(1, -1); report(a); assert(sent == 9 && cleared == 2);   // no free slot
    dm_motion_pro_players(1, 4); report(a); assert(sent == 12 && player == 4);
    removed(a); assert(!a.callback && !g_pro_devices && !g_pro_bound && cleared == 4);
    dm_motion_pro_players(1, 0); found(b); report(b); assert(sent == 15 && player == 0);   // reconnect / merged mode
    removed(b); removed(synthetic); assert(!g_pro_devices);
    puts("t_motion_pro_mac: passed");
} }
