#include <TargetConditionals.h>
#if TARGET_OS_OSX && defined(MNG_SWITCH2_CONTROLLER)
// CoreBluetooth transport for Switch 2 controllers, shared by the Mac and iOS front ends (pad_switch2_apple.h says what it does; switch2_proto.h is the protocol).
// Everything runs on the main queue, where the front ends' own pad callbacks run.
#import "pad_switch2_apple.h"
#import <CoreBluetooth/CoreBluetooth.h>
#if TARGET_OS_OSX
#import <IOBluetooth/IOBluetooth.h>   // IOBluetoothHostController: this Mac's own Bluetooth address, which a bonded controller wakes toward
#endif
#define LOG(...) do { flockfile(stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } while (0)
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dm_switch2_front g_front;
static s2_pad *g_pads;
static int g_verbose;
static uint8_t g_host[6];           // this host's Bluetooth address, little endian, as a bonded controller's wake advertisement carries it
static int g_host_known;

s2_pad *dm_switch2_pads(void) { return g_pads; }

@interface S2Command : NSObject
@property (nonatomic) uint8_t cmd, sub; @property (nonatomic, copy) NSData *payload; @property (nonatomic, copy) void (^done)(NSData *);
@end
@implementation S2Command
@end

@interface S2Session : NSObject <CBPeripheralDelegate>
@property (nonatomic, strong) CBPeripheral *peripheral;
@property (nonatomic) uint16_t pid;
@property (nonatomic) int use;      // S2_ADV_PAIR, S2_ADV_TAKE or S2_ADV_WAKE: why it was connected
@property (nonatomic) s2_pad *pad;
@property (nonatomic) BOOL live;
@end

static NSMutableDictionary<NSUUID *, S2Session *> *g_sessions;
static CBCentralManager *g_central;
static void s2_session_closed(S2Session *s);

static const char *s2_kind(uint16_t pid) { return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)"; }
static s2_dev *s2_dev_of(s2_pad *p, uint16_t pid) { return pid == S2_PID_PRO ? &p->st.pro : pid == S2_PID_JOYCON_L ? &p->st.left : &p->st.right; }
static int s2_pad_empty(const s2_pad *p) { return !p->st.pro.have && !p->st.left.have && !p->st.right.have; }
static int s2_player(s2_pad *p) { return p && g_front.player ? g_front.player(p) : -1; }
static s2_pad *s2_pad_for(uint16_t pid) {   // a Joy-Con joins a pad that has the other one alone; anything else starts a pad
    if (pid != S2_PID_PRO)
        for (s2_pad *p = g_pads; p; p = p->next)
            if (!p->st.pro.have && ((pid == S2_PID_JOYCON_L && p->st.right.have && !p->st.left.have) || (pid == S2_PID_JOYCON_R && p->st.left.have && !p->st.right.have))) return p;
    s2_pad *p = calloc(1, sizeof *p); if (!p) return NULL;
    p->next = g_pads; g_pads = p;
    return p;
}

@implementation S2Session {
    CBCharacteristic *_wr, *_rsp, *_inp, *_vib;
    NSTimer *_rumbleTimer;
    uint8_t _rumbleSequence;
    unsigned _silentPackets;
    s2_rest _rest;
    NSMutableArray<S2Command *> *_queue;
    BOOL _pending; uint8_t _pendingCmd; NSUInteger _seq; void (^_cb)(NSData *);
    s2_cal _cal[2];
    NSTimer *_tick;
    uint64_t _reports;
    BOOL _handshaken;
}
- (instancetype)initWith:(CBPeripheral *)p pid:(uint16_t)pid use:(int)use {
    if ((self = [super init])) { _peripheral = p; _pid = pid; _use = use; p.delegate = self; _queue = [NSMutableArray new]; s2_cal_default(&_cal[0]); s2_cal_default(&_cal[1]); }
    return self;
}
- (void)begin {
    [_peripheral discoverServices:nil];
    __weak S2Session *w = self;   // a controller that connects but never finishes the handshake is dropped
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ S2Session *s = w; if (s && !s->_handshaken) [s fail:@"handshake timed out"]; });
}
- (void)fail:(NSString *)why {
    LOG("[sw2] %s: %s", s2_kind(_pid), why.UTF8String);
    [g_central cancelPeripheralConnection:_peripheral];   // didDisconnect cleans up; a session that never connected is dropped there too
    if (!_live) s2_session_closed(self);
}
- (void)teardown {
    [_tick invalidate]; _tick = nil;
    [_rumbleTimer invalidate]; _rumbleTimer = nil;
    if (_pad && _pad == g_pads) runtime_feedback_switch2_motion(NULL, NULL);
    if (_pad) {
        s2_pad *p = _pad; _pad = NULL;
        s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
        if (s2_pad_empty(p)) {
            s2_pad **link = &g_pads; while (*link && *link != p) link = &(*link)->next;
            if (*link) *link = p->next;
            if (g_front.detach) g_front.detach(p);
            free(p);
        } else if (g_front.read) g_front.read(p);
    }
}
// ---- commands: one in flight (the controller has one reply channel) ----
- (void)send:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n done:(void (^)(NSData *))done {
    S2Command *c = [S2Command new]; c.cmd = cmd; c.sub = sub; c.payload = [NSData dataWithBytes:pl length:n]; c.done = done;
    [_queue addObject:c]; [self pump];
}
- (void)pump {
    if (_pending || !_queue.count || !_wr) return;
    S2Command *c = _queue.firstObject; [_queue removeObjectAtIndex:0];
    uint8_t f[8 + 255]; size_t n = s2_command(f, sizeof f, c.cmd, c.sub, c.payload.bytes, c.payload.length);
    if (!n) { if (c.done) c.done(nil); [self pump]; return; }
    _pending = YES; _pendingCmd = c.cmd; _cb = c.done; NSUInteger mine = ++_seq; uint8_t id = c.cmd;
    __weak S2Session *w = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        S2Session *s = w; if (!s || !s->_pending || s->_seq != mine) return;
        s->_pending = NO; void (^d)(NSData *) = s->_cb; s->_cb = nil;
        LOG("[sw2] %s: command 0x%02x timed out", s2_kind(s->_pid), id);
        if (d) d(nil);
        [s pump];
    });
    [_peripheral writeValue:[NSData dataWithBytes:f length:n] forCharacteristic:_wr type:CBCharacteristicWriteWithoutResponse];
}
- (void)readMemory:(uint8_t)len at:(uint32_t)addr done:(void (^)(NSData *))done {
    uint8_t p[8]; s2_memory_payload(p, len, addr);
    [self send:S2_CMD_MEMORY sub:S2_SUB_MEMORY_READ payload:p len:8 done:^(NSData *r) {
        done(r.length >= S2_REPLY_DATA + len && ((const uint8_t *)r.bytes)[8] == len ? [r subdataWithRange:NSMakeRange(S2_REPLY_DATA, len)] : nil);
    }];
}
- (void)readCal:(int)slot done:(void (^)(void))done {   // the user's calibration when set, else the factory's
    __weak S2Session *w = self;
    uint32_t user = slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, factory = slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1;
    [self readMemory:0x0B at:user done:^(NSData *u) {
        S2Session *s = w; if (!s) return;
        if (u && s2_cal_parse(u.bytes, u.length, &s->_cal[slot])) { done(); return; }
        [s readMemory:0x0B at:factory done:^(NSData *f) { S2Session *s2 = w; if (!s2) return; if (f) s2_cal_parse(f.bytes, f.length, &s2->_cal[slot]); done(); }];
    }];
}
- (void)setLEDs:(void (^)(void))done {
    int p = s2_player(_pad); uint8_t pl[4] = {s2_led_pattern[p >= 0 && p < 8 ? p : 0], 0, 0, 0};
    [self send:S2_CMD_LEDS sub:S2_SUB_LEDS_SET payload:pl len:4 done:^(NSData *r) { (void)r; if (done) done(); }];
}
- (void)handshake {
    __weak S2Session *w = self;
    [self readMemory:0x40 at:S2_ADDR_INFO done:^(NSData *info) {
        S2Session *s = w; if (!s) return;
        uint16_t vid, pid; char serial[15];
        if (!info || !s2_info_parse(info.bytes, info.length, &vid, &pid, serial) || vid != S2_VENDOR_ID) { [s fail:@"the controller did not answer the identity read"]; return; }
        if (pid != s->_pid) LOG("[sw2] advertised 0x%04x but identifies as 0x%04x", s->_pid, pid);
        if (pid != S2_PID_PRO) { [s fail:@"the controller is not a Switch 2 Pro Controller"]; return; }
        void (^calibrate0)(void) = ^{ S2Session *a = w; if (!a) return;
            [a readCal:0 done:^{ S2Session *b = w; if (!b) return;
                void (^next)(void) = ^{ S2Session *c = w; if (!c) return; [c finish]; };
                if (b->_pid == S2_PID_PRO) [b readCal:1 done:next]; else next();
            }];
        };
        void (^calibrate)(void) = calibrate0;
        if (s->_use == S2_ADV_PAIR) [s bond:calibrate]; else calibrate();
    }];
}
- (void)bond:(void (^)(void))done {
    __weak S2Session *w = self;
    uint8_t host[S2_PAIR_HOST_LEN], k1[16], k2[16], key1[S2_PAIR_KEY_LEN], key2[S2_PAIR_KEY_LEN], last = 0;
    arc4random_buf(k1, sizeof k1); arc4random_buf(k2, sizeof k2);
    s2_pair_host_payload(host, g_host); s2_pair_key_payload(key1, k1); s2_pair_key_payload(key2, k2);
    NSData *z = [NSData dataWithBytes:&last length:1], *h = [NSData dataWithBytes:host length:sizeof host], *a = [NSData dataWithBytes:key1 length:sizeof key1], *b = [NSData dataWithBytes:key2 length:sizeof key2];
    void (^step)(NSData *, const char *, void (^)(void)) = ^(NSData *r, const char *what, void (^next)(void)) {
        S2Session *s = w; if (!s) return;
        if (!r) { LOG("[sw2] %s: pairing stopped at %s (no answer): it plays now, but needs Sync again next time", s2_kind(s->_pid), what); done(); return; }
        next();
    };
    [self send:S2_CMD_PAIR sub:S2_SUB_PAIR_HOST payload:h.bytes len:h.length done:^(NSData *r1) { step(r1, "the host address", ^{
        S2Session *s = w; if (!s) return;
        [s send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY1 payload:a.bytes len:a.length done:^(NSData *r2) { step(r2, "the first key", ^{
            S2Session *t = w; if (!t) return;
            [t send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY2 payload:b.bytes len:b.length done:^(NSData *r3) { step(r3, "the second key", ^{
                S2Session *u = w; if (!u) return;
                [u send:S2_CMD_PAIR sub:S2_SUB_PAIR_DONE payload:z.bytes len:z.length done:^(NSData *r4) { step(r4, "the last step", ^{
                    S2Session *v = w; if (!v) return;
                    LOG("[sw2] %s paired with this Mac: from now on a press of any of its buttons connects it, no Sync needed", s2_kind(v->_pid));
                    done();
                }); }];
            }); }];
        }); }];
    }); }];
}
- (void)finish {
    __weak S2Session *w = self;
    uint8_t pl[4] = {S2_FEATURES, 0, 0, 0};
    [self send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_SET payload:pl len:4 done:^(NSData *r1) {
        S2Session *s = w; if (!s) return; (void)r1;
        uint8_t on[4] = {S2_FEATURES, 0, 0, 0};
        [s send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_ENABLE payload:on len:4 done:^(NSData *r2) {
            S2Session *t = w; if (!t) return;
            if (!r2) { [t fail:@"enabling the features was refused"]; return; }
            [t attachPad];
        }];
    }];
}
- (void)attachPad {
    s2_pad *p = s2_pad_for(_pid);
    if (!p) { [self fail:@"out of memory"]; return; }
    BOOL fresh = s2_pad_empty(p);
    s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
    d->have = 1; d->pid = _pid; d->cal[0] = _cal[0]; d->cal[1] = _cal[1];
    _pad = p; _live = YES; _handshaken = YES;
    if (fresh) {
        if (g_front.attach) g_front.attach(p);
        if (s2_player(p) < 0) { LOG("[sw2] %s: no free player, not used", s2_kind(_pid)); [self fail:@"all players are taken"]; return; }
    } else if (g_front.read) g_front.read(p);
    LOG("[sw2] %s connected: player %d%s", s2_kind(_pid), s2_player(p) + 1, fresh ? "" : " (joined the Joy-Con already there)");
    [self setLEDs:nil];
    [_peripheral setNotifyValue:YES forCharacteristic:_inp];
    __weak S2Session *w = self;   // the link drops after about ten seconds without a write from the host, so the LEDs are written once a second
    {
        _rumbleTimer = [NSTimer timerWithTimeInterval:0.015 repeats:YES block:^(NSTimer *timer) {
            S2Session *s = w;
            if (s) [s rumbleTick]; else [timer invalidate];
        }];
        [NSRunLoop.mainRunLoop addTimer:_rumbleTimer forMode:NSRunLoopCommonModes];
    }
    _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        S2Session *s = w; if (!s) { [t invalidate]; return; }
        int pl = s2_player(s->_pad); uint8_t l[4] = {s2_led_pattern[pl >= 0 && pl < 8 ? pl : 0], 0, 0, 0}; uint8_t f[12];
        size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, l, 4);
        if (s->_wr && n) [s->_peripheral writeValue:[NSData dataWithBytes:f length:n] forCharacteristic:s->_wr type:CBCharacteristicWriteWithoutResponse];
    }];
}
// Two grips, each with three consecutive 5 ms samples. MNG exposes its held
// vibration value for each grip; repeat it until the game's next value arrives.
- (void)rumbleTick {
    if (!_live || !_pad || !_vib || !_peripheral.canSendWriteWithoutResponse) return;
    float motors[2][4] = {{0}};
    if (g_front.input_active() && runtime_rumble_enabled()) runtime_feedback_switch2_rumble(motors);
    s2_haptic samples[2][3];
    int active = 0;
    for (unsigned i = 0; i < 2; i++) {
        float *m = motors[i];
        s2_rumble_cap(&m[0], &m[2]);
        s2_haptic value = s2_haptic_from(m[0], m[1], m[2], m[3]);
        for (unsigned sample = 0; sample < 3; sample++) samples[i][sample] = value;
        active |= s2_haptic_on(&value);
    }
    if (active) _silentPackets = 3;
    else if (!_silentPackets) return;
    else _silentPackets--;
    uint8_t packet[33];
    size_t length = s2_rumble_packet3(packet, samples[0], samples[1], _rumbleSequence++);
    [_peripheral writeValue:[NSData dataWithBytes:packet length:length]
           forCharacteristic:_vib type:CBCharacteristicWriteWithoutResponse];
}
- (void)report:(NSData *)d {
    s2_report r; if (!_pad || !s2_report_parse(d.bytes, d.length, &r)) return;
    s2_dev_of(_pad, _pid)->r = r;
    _pad->received_report = 1;
    if (g_verbose && _reports++ % 33 == 0)
        LOG("[sw2] %s: buttons 0x%08x sticks %u,%u %u,%u", s2_kind(_pid), r.buttons, r.lx, r.ly, r.rx, r.ry);
    if (g_front.read) g_front.read(_pad);
    if (_pad == g_pads) {
        if (g_front.input_active() && runtime_gyro_enabled()) {
            float acc[3], gyro[3];
            s2_rest_update(&_rest, r.raw);
            s2_motion(r.raw, s2_map_pro, _rest.have ? _rest.bias : NULL, S2_GYRO_RAD, acc, gyro);
            runtime_feedback_switch2_motion(acc, gyro);
        } else runtime_feedback_switch2_motion(NULL, NULL);
    }
}
// ---- CBPeripheralDelegate ----
- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"service discovery: %@", e.localizedDescription]]; return; }
    for (CBService *s in p.services) [p discoverCharacteristics:nil forService:s];
}
- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"characteristic discovery: %@", e.localizedDescription]]; return; }
    CBUUID *wr = [CBUUID UUIDWithString:@S2_UUID_CMD_WR], *rs = [CBUUID UUIDWithString:@S2_UUID_CMD_RSP], *in = [CBUUID UUIDWithString:@S2_UUID_INPUT];
    CBUUID *vibration = [CBUUID UUIDWithString:@S2_UUID_VIB_PRO];
    for (CBCharacteristic *c in s.characteristics) { if ([c.UUID isEqual:wr]) _wr = c; else if ([c.UUID isEqual:rs]) _rsp = c; else if ([c.UUID isEqual:in]) _inp = c;
        else if ([c.UUID isEqual:vibration] && (c.properties & CBCharacteristicPropertyWriteWithoutResponse)) _vib = c;
    }
    if (_wr && _rsp && _inp && !_handshaken && !_live) [p setNotifyValue:YES forCharacteristic:_rsp];
}
- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"notifications: %@", e.localizedDescription]]; return; }
    if (c == _rsp && !_live) [self handshake];
}
- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)e {
    if (e || !c.value.length) return;
    if (c == _inp) { [self report:c.value]; return; }
    if (c == _rsp && _pending && s2_reply_is(c.value.bytes, c.value.length, _pendingCmd)) {
        _pending = NO; void (^d)(NSData *) = _cb; _cb = nil;
        if (d) d(c.value);
        [self pump];
    }
}
- (void)peripheral:(CBPeripheral *)p didModifyServices:(NSArray<CBService *> *)invalidated { (void)invalidated; [self fail:@"the controller reset the link"]; }
@end

static void s2_session_closed(S2Session *s) { [s teardown]; [g_sessions removeObjectForKey:s.peripheral.identifier]; }

@interface S2Central : NSObject <CBCentralManagerDelegate>
@end
@implementation S2Central {
    NSMutableDictionary<NSUUID *, NSDate *> *_tried;   // a controller whose connection failed is not tried again at every advertisement
    NSMutableSet<NSUUID *> *_told;                       // a controller in pairing mode that cannot be paired is logged once
}
- (void)scan:(CBCentralManager *)c {
    [c scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];   // a controller's wake is advertised for a short while: every advertisement counts
    LOG("[sw2] Switch 2 controllers: press a button on a paired one; to pair one, hold its Sync button until the player lights sweep");
}
- (void)centralManagerDidUpdateState:(CBCentralManager *)c {
    if (c.state == CBManagerStatePoweredOn) {
#if TARGET_OS_OSX
        static BOOL asked;
        if (!g_host_known && !asked) {   // this Mac's address, from the Bluetooth host controller once the app may use Bluetooth (off the main queue: the first call can take seconds); the scan starts after it
            asked = YES; __weak CBCentralManager *wc = c;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                const char *addr = IOBluetoothHostController.defaultController.addressAsString.UTF8String;   // "aa-bb-cc-dd-ee-ff"
                unsigned a[6]; BOOL ok = addr && sscanf(addr, "%x%*c%x%*c%x%*c%x%*c%x%*c%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6 && (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]);
                uint8_t le[6]; for (int i = 0; i < 6; i++) le[i] = ok ? (uint8_t)a[5 - i] : 0;
                NSData *leData = [NSData dataWithBytes:le length:6];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (ok) { memcpy(g_host, leData.bytes, 6); g_host_known = 1; }
                    else LOG("[sw2] this Mac's Bluetooth address could not be read: controllers are used without pairing (DM_SW2_HOST=aa:bb:cc:dd:ee:ff names it)");
                    CBCentralManager *cm = wc; if (cm && cm.state == CBManagerStatePoweredOn) [self scan:cm];
                });
            });
            return;
        }
#endif
        [self scan:c];
    } else if (c.state == CBManagerStateUnauthorized) LOG("[sw2] Bluetooth is not allowed for this app (System Settings > Privacy & Security > Bluetooth)");
    else if (c.state == CBManagerStatePoweredOff) LOG("[sw2] Bluetooth is off");
}
- (void)centralManager:(CBCentralManager *)c didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary<NSString *, id> *)ad RSSI:(NSNumber *)rssi {
    NSData *d = ad[CBAdvertisementDataManufacturerDataKey];
    if (d.length < 18 || s2_u16(d.bytes) != S2_COMPANY_ID || g_sessions[p.identifier]) return;
    s2_advert a; if (!s2_advert_parse((const uint8_t *)d.bytes + 2, d.length - 2, &a) || a.pid != S2_PID_PRO) return;
    int use = s2_advert_use(&a, g_host_known ? g_host : NULL);
    if (!_tried) { _tried = [NSMutableDictionary new]; _told = [NSMutableSet new]; }
    if (use == S2_ADV_IGNORE) {   // waking toward another host (a Switch 2 nearby): not touched, logged once
        if (![_told containsObject:p.identifier]) { [_told addObject:p.identifier]; LOG("[sw2] a %s woke for another host: left alone (hold its Sync button to pair it with this Mac)", s2_kind(a.pid)); }
        return;
    }
    if (_tried[p.identifier] && -[_tried[p.identifier] timeIntervalSinceNow] < 3) return;
    _tried[p.identifier] = [NSDate date];
    if (use == S2_ADV_TAKE && ![_told containsObject:p.identifier]) {
        [_told addObject:p.identifier];
        LOG("[sw2] this host's Bluetooth address is not known: the %s is used without pairing, and needs Sync again next time", s2_kind(a.pid));
    }
    S2Session *s = [[S2Session alloc] initWith:p pid:a.pid use:use];
    g_sessions[p.identifier] = s;
    LOG("[sw2] %s %s (rssi %d), connecting", use == S2_ADV_WAKE ? "woke:" : "pairing:", s2_kind(a.pid), rssi.intValue);
    [c connectPeripheral:p options:nil];
    NSUUID *id = p.identifier; __weak CBCentralManager *wc = c;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        S2Session *t = g_sessions[id];
        if (t && t.peripheral.state != CBPeripheralStateConnected) { LOG("[sw2] connecting timed out"); [wc cancelPeripheralConnection:t.peripheral]; s2_session_closed(t); }
    });
}
- (void)centralManager:(CBCentralManager *)c didConnectPeripheral:(CBPeripheral *)p { [g_sessions[p.identifier] begin]; }
- (void)centralManager:(CBCentralManager *)c didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
    LOG("[sw2] connect failed: %s", e.localizedDescription.UTF8String ?: "unknown");
    S2Session *s = g_sessions[p.identifier]; if (s) s2_session_closed(s);
}
- (void)centralManager:(CBCentralManager *)c didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
    S2Session *s = g_sessions[p.identifier]; if (!s) return;
    LOG("[sw2] %s disconnected%s%s", s2_kind(s.pid), e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    s2_session_closed(s);
}
@end

static S2Central *g_delegate;
static void s2_central_start(void) { g_central = [[CBCentralManager alloc] initWithDelegate:g_delegate queue:nil options:nil]; }

int dm_switch2_start(const dm_switch2_front *front) {
    if (g_central) return 1;
    const char *e;
    if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"]) {
        LOG("[sw2] Switch 2 controllers are off: this app's Info.plist has no NSBluetoothAlwaysUsageDescription");
        return 0;
    }
    g_front = *front;
    e = getenv("DM_SW2_VERBOSE"); g_verbose = e && atoi(e);
    g_sessions = [NSMutableDictionary new];
    g_delegate = [S2Central new];
    unsigned v[6]; char sep; e = getenv("DM_SW2_HOST");
    if (e && sscanf(e, "%x%c%x%c%x%c%x%c%x%c%x", &v[0], &sep, &v[1], &sep, &v[2], &sep, &v[3], &sep, &v[4], &sep, &v[5]) == 11) { for (int i = 0; i < 6; i++) g_host[i] = (uint8_t)v[5 - i]; g_host_known = 1; }
    s2_central_start();   // the main queue: the pad callbacks run where the front end's do. On the Mac this Mac's address is read once Bluetooth is allowed (centralManagerDidUpdateState)
    return 1;
}

#endif
