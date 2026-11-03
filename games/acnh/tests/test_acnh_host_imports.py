"""SDK functions Animal Crossing 3.0.1 imports that would otherwise go to Switch services this host does not run (reported in play, PR #1305:
 the mailbox crash, sm: service "pctl" not implemented). Each must be named by a host override table, so the
loader binds the host function (runtime/loader.c relocate: runtime_override by name)."""
import re
import unittest
from pathlib import Path

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"

# The list made on 2026-10-29 from the imports of main and subsdk0 that no override named, reachable in single-player play.
REQUIRED = {
    "system_applets.c": [
        "_ZN2nn4pctl32CheckFreeCommunicationPermissionEb",   # the mailbox
        "_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE",
        "_ZN2nn2ec30ShowShopApplicationInformationEv",
        "_ZN2nn5album10InitializeEv", "_ZN2nn5album8FinalizeEv",
        "_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE",
        "_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc",
        "_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE",
        "_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE"],
    "fs_host.c": [
        "_ZN2nn2fs6CommitEPKcRKNS0_12CommitOptionE", "_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_",
        "_ZN2nn2fs20ExtendDeviceSaveDataEll", "_ZN2nn2fs22GetSaveDataRestoreFlagEPbPKc",
        "_ZN2nn2fs15RenameDirectoryEPKcS2_", "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"],
    "system_services.c": [
        "_ZN2nn4nifm19SetLocalNetworkModeEb", "_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_",
        "_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", "_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE",
        "_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "_ZN2nn3ssl7Context9ImportCrlEPmPKcj",
        "_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", "_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_"],
    "input.c": ["_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_"],
    # Already answered before 3.0.1 (1.2.1 keeps these handlers): system_applets.c must not name them again.
    "applet_host.c": ["_ZN2nn4pctl28IsFreeCommunicationAvailableEv", "_ZN2nn3err9ShowErrorENS_6ResultE"],
    "account.c": [
        "_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi",
        "_ZN2nn7account20PromoteQualificationERKNS0_3UidE",
        "_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE"],
}


class HostImportTests(unittest.TestCase):
    def test_geometry_stages_are_skipped_not_fatal(self):
        # The name entry keyboard's candidate list (Swkbd/shader/*Ocean*) draws with a geometry shader (reported in play, PR #1305).
        mtl = (RUNTIME.parent / "third_party/d2r_gfx/gfx/nvn/nvn_mtl.m").read_text()
        self.assertNotIn('MDIE("TOTK shader %s: unsupported NVN stage %u"', mtl)
        self.assertIn("o->unsupported|=1<<st", mtl)
        self.assertIn("(o->unsupported & 1 << st)) continue;", mtl)

    def test_overrides_name_the_imports(self):
        for file, names in REQUIRED.items():
            table = (RUNTIME / file).read_text()
            for name in names:
                with self.subTest(name=name):
                    self.assertRegex(table, r'\{"' + re.escape(name) + r'",\s*[a-z_]+\}')

    def test_each_name_has_one_table(self):
        # system_applets.c is consulted before the other tables (services.c): a name it repeated would replace the
        # handler 1.2.1 was verified with.
        names = {}
        for path in sorted(RUNTIME.glob("*.[cm]")):
            for name in re.findall(r'\{"(_ZN[^"]+)",\s*[a-z_]+\}', path.read_text()):
                names.setdefault(name, set()).add(path.name)
        for name in re.findall(r'\{"(_ZN[^"]+)"', (RUNTIME / "system_applets.c").read_text()):
            with self.subTest(name=name):
                self.assertEqual(names[name], {"system_applets.c"})

    def test_the_applets_table_is_consulted(self):
        services = (RUNTIME / "services.c").read_text()
        self.assertIn("runtime_applets_override(name)", services)


if __name__ == "__main__":
    unittest.main()
