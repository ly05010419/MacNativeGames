"""Exercise applet state through the same symbol map used by the guest SDK."""
from pathlib import Path
import subprocess
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]


class AppletStateTests(unittest.TestCase):
    def test_save_backup_query_is_disabled_and_unknown_commands_fail(self):
        source = r'''
#include "runtime.h"
#include "ipc.h"
#include <stdlib.h>
#include <string.h>
static IpcHandler handler; static IpcOpen open_session; static void (*close_session)(void *);
void runtime_ipc_register(const char *name, IpcHandler h, IpcOpen o, void (*c)(void *)) {
    if (strcmp(name,"olsc:u")) abort(); handler=h; open_session=o; close_session=c;
}
void runtime_account_uid(uint64_t out[2]) { out[0]=1; out[1]=2; }
_Noreturn void runtime_fail(const char *message) { (void)message; abort(); }
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) { (void)s; (void)rq; _Exit(77); }
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t size) {
    if (off+size>rq->data_size) abort(); return rq->data+off;
}
void ipc_out(IpcResponse *rs, const void *data, uint32_t size) {
    if (rs->data_size+size>sizeof rs->data) abort();
    memcpy(rs->data+rs->data_size,data,size); rs->data_size+=size;
}
int main(int argc, char **argv) {
    (void)argv; if (!handler || !open_session || !close_session) return 1;
    IpcSession session={.name="olsc:u",.state=open_session()};
    IpcRequest rq={0}; IpcResponse rs={0}; handler(&session,&rq,&rs);
    uint64_t uid[2]={1,2}; rq.cmd=13; rq.data=(void *)uid; rq.data_size=sizeof uid;
    handler(&session,&rq,&rs);
    if (rs.result || rs.data_size!=1 || rs.data[0]!=0) return 2;
    uid[0]=3; memset(&rs,0,sizeof rs); handler(&session,&rq,&rs);
    if (!rs.result || rs.data_size) return 3;
    if (argc>1) { rq.cmd=99; handler(&session,&rq,&rs); return 4; }
    close_session(session.state); return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            probe, binary = root / "olsc.c", root / "olsc"
            probe.write_text(source)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(GAME / "runtime"), str(probe), str(GAME / "runtime/olsc_host.c"),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)
            unknown = subprocess.run([str(binary), "unknown"], capture_output=True)
            self.assertEqual(unknown.returncode, 77)

    def test_account_enumeration_preserves_uid_and_buffer_bounds(self):
        source = r'''
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
_Noreturn void runtime_fail(const char *message) { (void)message; abort(); }
int main(int argc, char **argv) {
    if (argc != 2 || setenv("TOTK_PROFILE_DIR", argv[1], 1)) return 1;
    typedef unsigned (*Count)(int *);
    typedef unsigned (*List)(int *, uint64_t *, int);
    Count total = (Count)runtime_account_override("_ZN2nn7account12GetUserCountEPi");
    List list = (List)runtime_account_override("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi");
    uint64_t uid[2], buffer[4] = {0,0,0x1234,0x5678}; int count = -1;
    runtime_account_uid(uid);
    if (!total || !list || total(&count) || count != 1) return 2;
    if (list(&count, buffer, 2) || count != 1 || memcmp(uid, buffer, sizeof uid)) return 3;
    if (buffer[2] != 0x1234 || buffer[3] != 0x5678) return 4;
    if (list(&count, NULL, 0) || count != 0) return 5;
    if (!list(&count, NULL, 1) || !list(&count, buffer, -1) || !total(NULL)) return 6;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            probe, binary = root / "account.c", root / "account"
            probe.write_text(source)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(GAME / "runtime"), str(probe), str(GAME / "runtime/account.c"),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary), str(root / "profile")], check=True, capture_output=True)

    def test_inactivity_query_tracks_both_setters(self):
        source = r'''
#include "runtime.h"
#include <stdlib.h>
uint64_t runtime_now_ns(void) { return 1; }
uint64_t runtime_desired_language(void) { return 0; }
int runtime_core_away(void) { return 0; }
void runtime_core_back(int *away) { (void)away; }
void hle_bind_system_event(void *event, int clear) { (void)event; (void)clear; }
_Noreturn void runtime_fail(const char *message) { (void)message; abort(); }
int main(void) {
    typedef unsigned char (*Get)(void);
    typedef void (*Set)(unsigned char);
    Get get = (Get)runtime_applet_override("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv");
    Set safe = (Set)runtime_applet_override("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb");
    Set unsafe = (Set)runtime_applet_override("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb");
    Get communication = (Get)runtime_applet_override("_ZN2nn4pctl28IsFreeCommunicationAvailableEv");
    if (!communication || !communication()) return 6;
    if (!get || !safe || !unsafe || get()) return 1;
    safe(1); if (!get()) return 2;
    unsafe(0); if (get()) return 3;
    unsafe(1); if (!get()) return 4;
    safe(0); return get() ? 5 : 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            probe, binary = root / "applet.c", root / "applet"
            probe.write_text(source)
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(GAME / "runtime"), str(probe), str(GAME / "runtime/applet_host.c"),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
