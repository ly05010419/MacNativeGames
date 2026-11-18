// Public NFC hooks must stop an older SDK from opening nfc:user, while preserving
// the existing NFP no-tag path. Exercise the real forced registrations, no guest SDK.
#include "../hle_sdk16.c"

static int checks, fails, lookups;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
void *guest_sym(const char *name) { (void)name; lookups++; return NULL; }
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 3; }
int dm_main_imports(const char *prefix) { (void)prefix; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
// The included SDK source also registers vsync hooks; NFC must never enter them.
uint64_t hle_system_event_binding(void *ev) { (void)ev; abort(); }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; abort(); }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static void *forced(const hle_force_ent *e, const char *name) {
    // ASan places redzones between section globals; inspect the emitted registration
    // objects individually instead of treating the Mach-O section as a packed array.
    CHECK(!strcmp(e->name, name) && e->fn, "forced registration has the exact public symbol and a function");
    CHECK(e->why && e->why[0], "forced hook has a reason");
    return e->fn;
}

int main(void) {
    void (*init)(void) = forced(&hle_force_h_nfc_Init, "_ZN2nn3nfc10InitializeEv");
    void (*fini)(void) = forced(&hle_force_h_nfc_Fini, "_ZN2nn3nfc8FinalizeEv");
    int (*state)(void) = forced(&hle_force_h_nfc_State, "_ZN2nn3nfc8GetStateEv");
    unsigned (*enabled)(void) = forced(&hle_force_h_nfc_Enabled, "_ZN2nn3nfc12IsNfcEnabledEv");
    void (*nfp_init)(void) = forced(&hle_force_h_nfp_Init, "_ZN2nn3nfp10InitializeEv");
    void (*nfp_fini)(void) = forced(&hle_force_h_nfp_Fini, "_ZN2nn3nfp8FinalizeEv");
    Result (*list)(NfpDeviceHandle *, int *, int) = forced(&hle_force_h_nfp_List, "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii");
    Result (*start)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Start, "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE");
    Result (*stop)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_Stop, "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE");
    int (*device_state)(const NfpDeviceHandle *) = forced(&hle_force_h_nfp_State, "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE");
    Result (*tag_info)(void *, const NfpDeviceHandle *) = forced(&hle_force_h_nfp_TagInfo, "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE");
    if (fails) return 1;
    CHECK(state() == 0, "NFC begins uninitialized");
    CHECK(enabled() == 0, "NFC has no physical reader before initialization");
    nfp_init();
    struct { uint64_t before; NfpDeviceHandle handle; uint64_t after; } device;
    memset(&device, 0xa5, sizeof device);
    int count = -1;
    CHECK(list(&device.handle, &count, 1) == RES_OK && count == 1, "existing NFP device remains available");
    CHECK(device.before == UINT64_C(0xa5a5a5a5a5a5a5a5) && device.after == UINT64_C(0xa5a5a5a5a5a5a5a5), "device enumeration respects output bounds");
    CHECK(start(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_SEARCH, "existing NFP detection can search without a tag");
    // Repeated entry/dismissal follows the caller's state check, init, disabled branch,
    // and conditional finalization. A missing forced export fails before this sequence.
    for (int pass = 0; pass < 3; pass++) {
        CHECK(state() == 0, "NFC entry starts finalized");
        if (state() != 1) init();
        CHECK(state() == 1, "NFC initialization is observable");
        CHECK(enabled() == 0, "initialization cannot fabricate physical NFC support");
        CHECK(device_state(&device.handle) == NFP_STATE_SEARCH, "NFC initialization preserves NFP detection");
        unsigned char output[0x80]; memset(output, 0x5a, sizeof output);
        CHECK(tag_info(output + 16, &device.handle) == (115u | 97u << 9), "tag access still returns tag-not-found");
        int unchanged = 1; for (unsigned i = 0; i < sizeof output; i++) if (output[i] != 0x5a) unchanged = 0;
        CHECK(unchanged, "no-tag result neither fabricates tag data nor overwrites guards");
        if (state()) fini();
        CHECK(state() == 0 && enabled() == 0, "dismissal finalizes NFC and keeps it disabled");
        CHECK(device_state(&device.handle) == NFP_STATE_SEARCH, "NFC finalization preserves NFP detection");
    }
    CHECK(stop(&device.handle) == RES_OK && device_state(&device.handle) == NFP_STATE_INIT, "NFP detection stops normally");
    nfp_fini();
    CHECK(state() == 0, "NFP finalization leaves NFC finalized");
    CHECK(lookups == 0, "NFC and NFP hooks do not call an original SDK service path");
    printf("NFC availability: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
