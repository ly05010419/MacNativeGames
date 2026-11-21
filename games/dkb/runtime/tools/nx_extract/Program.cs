// NSP extraction: nx_extract <prod.keys> <base NSP> <update NSP or -> <output dir> [base RomFS dir to compare against] [--manifest] [--romfs-image]
// --romfs-image streams the merged RomFS directly to <output dir>/RomFS.img (the packing script's default); only ExeFS is expanded. Without it, legacy folder extraction remains available.
// With a base RomFS dir in legacy mode, files whose content matches it become APFS clones (no new space).
// --manifest: the RomFS is not written, only a list <output dir>/romfs_manifest.tsv (path, size, first 32 bytes), for surveying a new game (a large game's RomFS is well over 10 GB),
//   and its NRO modules under <output dir>/romfs_nro/ (small: their imports decide the graphics API and the modules, Portal 2's 21 NROs are 60 MB).
// A damaged NSP (a file whose data fails the NCA's hash check, e.g. a copy cut short or a broken download) stops the extraction with exit code 3 after listing every damaged file;
//   --allow-damaged writes those files as read instead and only lists them (Super Mario Galaxy's base NSP with 147 MB zeroed: its two prologue WebM movies). A RomFS image marks them in its index, and the host names them in its log.
// A sparse base program (Minecraft Dungeons II: its Program NCA leaves out data that its update carries) can be read only with its update: without one, exit code 4.
// Add-on content: nx_extract <prod.keys> --aoc <DLC NSP> <output dir> writes the RomFS of the DLC's data NCA to <output dir> (the packing script puts it at aoc/<index>/, the host mounts it through DM_AOC_DIR).
// Title ID of a package: nx_extract <prod.keys> --title <NSP, NSZ, XCI or XCZ> prints "title <16 hex digits>", the lowest title ID of the package's meta NCAs (the base game's, before its update's or a DLC's),
//   read from the meta NCA's header with the keys' header_key. For a package without a ticket and without a [title ID] in its name (the NSP script's title_of); exit code 5 when it has no meta NCA.
// Keys of a package: nx_extract <prod.keys> --keys <NSP, NSZ, XCI or XCZ>... checks that the keys open every NCA (an NCZ by its header, which nsz keeps as it was) without extracting anything.
//   Keys older than a game (a prod.keys dumped from a console on older firmware) lack the newer master key and every key derived from it; the newest key missing is named with the firmware
//   it came with, exit code 6. Extraction, --aoc and --contents run the same check before reading the NCAs it covers (LibHac would stop on the first such NCA with an unhandled MissingKeyException).
// Title keys come from the common tickets (.tik) inside the NSPs, so only prod.keys is needed; key values are never printed.
// The base game may also be a cartridge dump (.xci, Monster Hunter Rise): its secure partition holds the NCAs, which carry their keys in the key area (no ticket).
//   When the cartridge also carries an update (1G+1U), only the base game's NCAs (those its Application content meta lists) are read; the update comes from the update NSP.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Ncm;
using LibHac.Spl;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using LibHac.Tools.Ncm;
using Path = System.IO.Path;

static class P {
    [DllImport("libc", SetLastError = true)] static extern int clonefile(string src, string dst, uint flags);

    static int Main(string[] a) {
        try { return Run(a); }
        catch (KeysMissing e) { Console.WriteLine($"[RED] {e.Message}"); return 6; }
    }

    static int Run(string[] a) {
        bool manifest = Array.IndexOf(a, "--manifest") >= 0; a = Array.FindAll(a, s => s != "--manifest");
        bool image = Array.IndexOf(a, "--romfs-image") >= 0; a = Array.FindAll(a, s => s != "--romfs-image");
        g_allow = Array.IndexOf(a, "--allow-damaged") >= 0; a = Array.FindAll(a, s => s != "--allow-damaged");
        // --app <title ID>: the application to read from a package that holds several (Super Mario Galaxy + Super Mario Galaxy 2 on one cartridge dump); build_ipa_from_nsp.py
        // always names the one it packs. Without it every Application content meta's NCAs are kept and the lowest Program NCA is the game
        ulong? app = null; int ai = Array.IndexOf(a, "--app");
        if (ai >= 0 && ai + 1 < a.Length) { app = Convert.ToUInt64(a[ai + 1], 16); var rest = new List<string>(a); rest.RemoveRange(ai, 2); a = rest.ToArray(); }
        if (a.Length == 3 && a[1] == "--title") return Title(ExternalKeyReader.ReadKeyFile(a[0]), a[2]);
        if (a.Length == 3 && a[1] == "--contents") return Contents(ExternalKeyReader.ReadKeyFile(a[0]), a[2]);
        if (a.Length >= 3 && a[1] == "--keys") return Keys(ExternalKeyReader.ReadKeyFile(a[0]), a[2..]);
        if (a.Length < 4) { Console.Error.WriteLine("usage: nx_extract <prod.keys> <base.nsp> <update.nsp|-> <out dir> [base RomFS dir] [--app <title ID>] [--romfs-image|--manifest] [--allow-damaged]\n       nx_extract <prod.keys> --aoc <dlc.nsp> <out dir or image> [<DLC title ID>] [--romfs-image]\n       nx_extract <prod.keys> --title <nsp|nsz|xci|xcz>\n       nx_extract <prod.keys> --contents <nsp|nsz|xci|xcz>\n       nx_extract <prod.keys> --keys <nsp|nsz|xci|xcz>..."); return 2; }
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[0]);
        if (a[1] == "--aoc") return Aoc(ks, a[2], a[3], image, a.Length > 4 ? Convert.ToUInt64(a[4], 16) : null);
        // each by its own content meta: a merged package (base game + update + DLC in one NSP, a cartridge dump with its update) is given as both the base game and the update,
        // and the base game's Program NCA and the update's have the same title ID, so without the filter the update would be layered on itself or on the wrong base
        var basePkg = OpenNsp(ks, a[1], ContentMetaType.Application, app); var updPkg = a[2] == "-" ? null : OpenNsp(ks, a[2], ContentMetaType.Patch, app | 0x800);
        Fit(new[] { MissingKey(ks, basePkg, a[1]), updPkg == null ? null : MissingKey(ks, updPkg, a[2]) });
        Nca baseProg = Pick(basePkg, NcaContentType.Program) ?? throw new Exception("no Program NCA in the base NSP");
        Nca updProg = updPkg == null ? null : Pick(updPkg, NcaContentType.Program, baseProg.Header.TitleId) ?? throw new Exception("no Program NCA of the base game's program in the update NSP");
        Nca ctrl = Pick(updPkg ?? basePkg, NcaContentType.Control, baseProg.Header.TitleId);
        Console.WriteLine($"title {baseProg.Header.TitleId:x16}" + (updProg != null ? $", update {updProg.Header.TitleId:x16}" : ""));
        string outDir = a[3], cmpDir = a.Length > 4 ? a[4] : null;
        if (ctrl != null) {   // control data (NACP): display version and game name; a raw copy goes to <output dir>/control.nacp for naming the app
            byte[] nacp = Nacp(ctrl);
            Console.WriteLine($"display version {System.Text.Encoding.ASCII.GetString(nacp, 0x3060, 0x10).TrimEnd('\0')}");
            Console.WriteLine($"name {TitleName(nacp)}");
            Directory.CreateDirectory(outDir); File.WriteAllBytes(Path.Combine(outDir, "control.nacp"), nacp);
            var (icon, lang) = Icon(ctrl);   // the game icon for the app icon, saved as <output dir>/icon.jpg (build_ipa.sh picks it up)
            if (icon != null) { File.WriteAllBytes(Path.Combine(outDir, "icon.jpg"), icon); Console.WriteLine($"icon {lang} ({icon.Length} bytes)"); }
            else Console.WriteLine("no icon in the control data");
        }
        if (updProg == null && Sparse(baseProg)) {   // Horizon mounts a sparse NCA only through its patch; alone, the left-out data fails the hash check
            Console.WriteLine("[RED] the base game's program is sparse: it can be read only with its update NSP");
            return 4;
        }
        // Code section: the update's ExeFS is complete; opening it layered over the base gives the same result
        Copy(() => updProg != null ? baseProg.OpenFileSystemWithPatch(updProg, NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid), Path.Combine(outDir, "ExeFS"), null);
        string[] mods = Directory.GetFiles(Path.Combine(outDir, "ExeFS")); Array.Sort(mods, StringComparer.Ordinal);
        foreach (string p in mods) {   // build ID of every NSO module (main, sdk, subsdk*, rtld...)
            if (new FileInfo(p).Length < 0x50) continue;
            byte[] h = new byte[0x50]; using (var f = File.OpenRead(p)) f.ReadExactly(h);
            if (h[0] == 'N' && h[1] == 'S' && h[2] == 'O' && h[3] == '0') Console.WriteLine($"{Path.GetFileName(p)} build id {Convert.ToHexString(h, 0x40, 16).ToLowerInvariant()}");
        }
        // A program can embed all assets in its code modules (ANIMAL WELL 1.0.10).
        // Preserve the extraction layout without asking LibHac to open an absent section.
        if (!baseProg.SectionExists(1) && (updProg == null || !updProg.SectionExists(1))) {
            if (image && !manifest) WriteImage(null, Path.Combine(outDir, "RomFS.img"));
            else if (!manifest) Directory.CreateDirectory(Path.Combine(outDir, "RomFS"));
            if (manifest) File.WriteAllText(Path.Combine(outDir, "romfs_manifest.tsv"), "# path\tbytes\tfirst 32 bytes (hex)\n");
            Console.WriteLine("program has no data section: empty RomFS");
            return Damaged();
        }
        IFileSystem Romfs(IntegrityCheckLevel level) => new RomFsFileSystem(updProg != null ? baseProg.OpenStorageWithPatch(updProg, NcaSectionType.Data, level) : baseProg.OpenStorage(NcaSectionType.Data, level));
        Func<IFileSystem> romfs = () => Romfs(IntegrityCheckLevel.ErrorOnInvalid);
        // the same RomFS without hash checks, for reading damaged files as they are (--allow-damaged)
        g_lenient = () => Romfs(IntegrityCheckLevel.IgnoreOnInvalid);
        if (manifest) Manifest(romfs, Path.Combine(outDir, "romfs_manifest.tsv"));
        else if (image) WriteImage(romfs, Path.Combine(outDir, "RomFS.img"));
        else Copy(romfs, Path.Combine(outDir, "RomFS"), cmpDir);
        return Damaged();
    }

    static int Title(KeySet ks, string path) {
        ulong? id = null;
        foreach (Nca n in OpenNsp(ks, path)) if (n.Header.ContentType == NcaContentType.Meta && (id == null || n.Header.TitleId < id)) id = n.Header.TitleId;
        if (id == null) { Console.WriteLine($"[RED] no meta NCA in {Path.GetFileName(path)}"); return 5; }
        Console.WriteLine($"title {id:x16}");
        return 0;
    }

    // Every content meta of a package, one line each: "content <type> <title ID> <version>". A merged package (base game + update + DLC in one NSP, or a cartridge dump with its
    // update) lists several; build_ipa_from_nsp.py then takes the one file as the base game, the update and the DLC
    static int Contents(KeySet ks, string path) {
        int n = 0; var ncas = OpenNsp(ks, path);
        Fit(new[] { MissingKey(ks, ncas.FindAll(m => m.Header.ContentType == NcaContentType.Meta), path) });
        foreach (Cnmt c in Metas(ncas)) { Console.WriteLine($"content {c.Type} {c.TitleId:x16} {c.TitleVersion.Version}"); n++; }
        if (n == 0) { Console.WriteLine($"[RED] no content meta in {Path.GetFileName(path)}"); return 5; }
        return 0;
    }

    static int Keys(KeySet ks, string[] paths) {
        var found = new List<(int Rev, string Key, string File)?>();
        foreach (string p in paths) found.Add(MissingKey(ks, OpenNsp(ks, p, ncz: true), p));
        Fit(found);
        Console.WriteLine($"the keys open all {paths.Length} packages");
        return 0;
    }

    // The firmware that brought each master key revision (index = revision; Atmosphère's and hactool's tables), to say which keys a game needs
    static readonly string[] Firmware = { "1.0.0", "3.0.0", "3.0.1", "4.0.0", "5.0.0", "6.0.0", "6.2.0", "7.0.0", "8.1.0", "9.0.0", "9.1.0",
                                          "12.1.0", "13.0.0", "14.0.0", "15.0.0", "16.0.0", "17.0.0", "18.0.0", "19.0.0", "20.0.0", "21.0.0" };
    static readonly string[] KeyAreas = { "application", "ocean", "system" };
    sealed class KeysMissing : Exception { public KeysMissing(string m) : base(m) { } }

    // What the keys lack to open a package's NCAs: the newest key one of them needs (its master key revision and name), or (-1, why) for a rights-ID NCA with no common ticket;
    // null when every NCA opens. The test is LibHac's own (CanOpenSection), the one extraction meets
    static (int Rev, string Key, string File)? MissingKey(KeySet ks, List<Nca> ncas, string path) {
        (int Rev, string Key, string File)? worst = null;
        foreach (Nca n in ncas) {
            bool opens = true;
            for (int i = 0; i < 4; i++) if (n.SectionExists(i) && !n.CanOpenSection(i)) opens = false;
            if (opens) continue;
            int rev = Math.Max(n.Header.KeyGeneration - 1, 0);   // the master key revision, as LibHac takes it from the header's key generation (0 and 1 are both revision 0)
            if (n.Header.HasRightsId && !ks.ExternalKeySet.Contains(new RightsId(n.Header.RightsId))) {
                worst ??= (-1, $"no title key for rights ID {Convert.ToHexString(n.Header.RightsId).ToLowerInvariant()}: the package has no common ticket for it", Path.GetFileName(path));
                continue;
            }
            int area = n.Header.KeyAreaKeyIndex;
            string key = n.Header.HasRightsId ? $"titlekek_{rev:x2}" : $"key_area_key_{(area < KeyAreas.Length ? KeyAreas[area] : area.ToString())}_{rev:x2}";
            if (worst == null || rev > worst.Value.Rev) worst = (rev, key, Path.GetFileName(path));
        }
        return worst;
    }

    // Stops (KeysMissing, exit code 6) on the newest key any of the packages needs, naming the firmware it came with
    static void Fit(IEnumerable<(int Rev, string Key, string File)?> found) {
        (int Rev, string Key, string File)? worst = null; int n = 0;
        foreach (var f in found) if (f != null) { n++; if (worst == null || f.Value.Rev > worst.Value.Rev) worst = f; }
        if (worst == null) return;
        var (rev, key, file) = worst.Value;
        string more = n > 1 ? $" (and {n - 1} more files)" : "";
        if (rev < 0) throw new KeysMissing($"{file}{more}: {key}");
        string fw = rev < Firmware.Length ? $"firmware {Firmware[rev]}" : $"a firmware after {Firmware[^1]}";
        throw new KeysMissing($"{file}{more}: prod.keys has no {key} (master key {rev:x2}, which came with {fw}): the keys are older than the game; " +
                              $"dump prod.keys again from a console on {fw}{(rev < Firmware.Length ? " or later" : "")}");
    }

    static bool Sparse(Nca n) {
        for (int i = 0; i < 4; i++) if (n.Header.IsSectionEnabled(i) && n.Header.GetFsHeader(i).ExistsSparseLayer()) return true;
        return false;
    }

    // Files whose data failed the hash check: listed at the end; exit code 3 unless --allow-damaged
    static readonly List<string> g_damaged = new List<string>(); static bool g_allow; static Func<IFileSystem> g_lenient; static IFileSystem g_lenientFs;
    static bool HashError(HorizonResultException e) => e.ResultValue.Module == 2 && e.ResultValue.Description >= 4600 && e.ResultValue.Description < 4700;   // fs: integrity verification failures (2002-46xx)
    // Lists a file that failed the hash check and returns a new checked filesystem to read on with. LibHac's CachedStorage takes a block off its list before reading into it and
    // does not put it back when the read fails, so after a few failures its 4 blocks are gone and the next read throws "The LinkedList is empty", which is no hash error.
    static IFileSystem Failed(string path, Func<IFileSystem> open) { g_damaged.Add(path); return open(); }
    static int Damaged() {
        if (g_damaged.Count == 0) return 0;
        foreach (string p in g_damaged) Console.WriteLine($"[RED] hash check failed: {p}");
        if (g_allow) { Console.WriteLine($"[RED] the NSP is damaged: {g_damaged.Count} files written as read (--allow-damaged)"); return 0; }
        Console.WriteLine($"[RED] the NSP is damaged: " +
                          $"{g_damaged.Count} files fail the NCA's hash check (an incomplete copy or a broken download); copy or download it again, or pass --allow-damaged to write them as read");
        return 3;
    }

    // DLC NSP: one data NCA (content type PublicData / Data) + a meta NCA + the ticket; only the data NCA's RomFS is written
    // id: the DLC's title ID, for a merged package that holds several add-on contents (and the base game)
    static int Aoc(KeySet ks, string nsp, string outDir, bool image, ulong? id = null) {
        var pkg = OpenNsp(ks, nsp);   // every NCA: Pick takes the DLC's own data NCA by its title ID
        Fit(new[] { MissingKey(ks, pkg, nsp) });
        Nca data = Pick(pkg, NcaContentType.PublicData, id) ?? Pick(pkg, NcaContentType.Data, id) ?? throw new Exception(id == null ? "no data NCA in the DLC NSP" : $"no data NCA of add-on content {id:x16} in {Path.GetFileName(nsp)}");
        Console.WriteLine($"add-on content {data.Header.TitleId:x16}");
        Directory.CreateDirectory(image ? Path.GetDirectoryName(outDir) : outDir);
        Func<IFileSystem> fs = () => new RomFsFileSystem(data.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid));
        g_lenient = () => new RomFsFileSystem(data.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid));
        if (image) WriteImage(fs, outDir); else Copy(fs, outDir, null);
        return Damaged();
    }

    // The content metas (*.cnmt in the Meta NCAs) of a package's NCAs
    static IEnumerable<Cnmt> Metas(List<Nca> ncas) {
        foreach (Nca n in ncas) {
            if (n.Header.ContentType != NcaContentType.Meta) continue;
            IFileSystem mfs = n.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
            foreach (DirectoryEntryEx e in mfs.EnumerateEntries("*.cnmt", SearchOptions.Default)) {
                using var f = new UniqueRef<IFile>();
                mfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                yield return new Cnmt(f.Release().AsStream());
            }
        }
    }

    // Open an NSP (PFS0) or the secure partition of an XCI, import the common tickets, list all NCAs. With a content meta type, only the NCAs that the package's content metas of
    // that type list (when it has one), and with a title ID only that title's: a cartridge dump with its update on it (1G+1U) or a merged NSP holds the update's NCAs beside the
    // base game's, its Program NCA under the same title ID; a cartridge with two games holds two Application content metas. ncz: an NSZ's or XCZ's compressed NCAs too, by their
    // first 0x4000 bytes (the NCA header and section headers, which nsz keeps as they were): enough for the key check, not for reading
    static List<Nca> OpenNsp(KeySet ks, string path, ContentMetaType? only = null, ulong? id = null, bool ncz = false) {
        IFileSystem pfs; var st0 = new LocalStorage(path, FileAccess.Read);
        byte[] magic = new byte[4]; st0.Read(0x100, magic).ThrowIfFailure();
        if (System.Text.Encoding.ASCII.GetString(magic) == "HEAD") {   // XCI: the card header's magic at 0x100
            var xci = new Xci(ks, st0);
            if (!xci.HasPartition(XciPartitionType.Secure)) throw new Exception($"no secure partition in {path}");
            pfs = xci.OpenPartition(XciPartitionType.Secure);
        } else {
            var p = new PartitionFileSystem(); p.Initialize(st0).ThrowIfFailure(); pfs = p;
        }
        var ncas = new List<Nca>(); var ids = new List<string>();   // ids: content IDs (the file names' hex)
        foreach (DirectoryEntryEx e in pfs.EnumerateEntries("*", SearchOptions.Default)) {
            using var f = new UniqueRef<IFile>();
            pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            IStorage st = f.Release().AsStorage();
            if (e.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase)) {
                byte[] t = new byte[0x2c0]; st.Read(0, t).ThrowIfFailure();
                if (t[0x281] != 0) { Console.Error.WriteLine($"[RED] {e.Name} is not a common ticket; supply the title key separately"); continue; }
                ks.ExternalKeySet.Add(new RightsId(t.AsSpan(0x2a0, 0x10)), new AccessKey(t.AsSpan(0x180, 0x10))).ThrowIfFailure();
            } else if (e.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)) { ncas.Add(new Nca(ks, st)); ids.Add(e.Name[..e.Name.IndexOf('.')].ToLowerInvariant()); }
            else if (ncz && e.Name.EndsWith(".ncz", StringComparison.OrdinalIgnoreCase)) { ncas.Add(new Nca(ks, st.Slice(0, 0x4000))); ids.Add(e.Name[..e.Name.IndexOf('.')].ToLowerInvariant()); }
        }
        if (only == null) return ncas;
        Fit(new[] { MissingKey(ks, ncas.FindAll(n => n.Header.ContentType == NcaContentType.Meta), path) });   // the content metas are read first, with the same keys
        var listed = new HashSet<string>(); var titles = new List<ulong>();   // titles: every content meta of that type, to name them when none is the one asked for
        foreach (Cnmt cnmt in Metas(ncas)) {
            if (cnmt.Type != only) continue;
            titles.Add(cnmt.TitleId);
            if (id == null || cnmt.TitleId == id) foreach (CnmtContentEntry c in cnmt.ContentEntries) listed.Add(Convert.ToHexString(c.NcaId).ToLowerInvariant());
        }
        if (id != null && titles.Count > 0 && !titles.Contains(id.Value))
            throw new Exception($"{Path.GetFileName(path)} has no {only} content meta of title {id:x16}; it has {string.Join(", ", titles.ConvertAll(t => t.ToString("x16")))}");
        if (listed.Count == 0) return ncas;
        var kept = new List<Nca>(); int other = 0;   // the meta NCAs list the others, not themselves
        for (int i = 0; i < ncas.Count; i++) if (listed.Contains(ids[i])) kept.Add(ncas[i]); else if (ncas[i].Header.ContentType != NcaContentType.Meta) other++;
        if (!kept.Exists(n => n.Header.ContentType == NcaContentType.Program)) {   // file names that are not content IDs (renamed NCAs): nothing matches, so read every NCA as before
            Console.WriteLine($"[RED] no NCA of {Path.GetFileName(path)} matches its {only} content meta by name (renamed NCA files?); reading all {ncas.Count}");
            return ncas;
        }
        if (other > 0) Console.WriteLine($"leaving out {other} NCAs of {Path.GetFileName(path)} that its {only} content meta does not list (another title in the package: an update on the cartridge, or a merged NSP)");
        return kept;
    }
    // A multi-program application has a Program and a Control NCA per program index (ROMANCE OF THE THREE KINGDOMS 8 REMAKE: its update 1.1.3 adds a whole second program,
    // title ID …0001, beside the game's patch, …0000): with no title ID the lowest one (program index 0, the game), else the NCA of that title ID
    static Nca Pick(List<Nca> l, NcaContentType t, ulong? id = null) {
        Nca r = null;
        foreach (Nca n in l) if (n.Header.ContentType == t && (id == null ? r == null || n.Header.TitleId < r.Header.TitleId : n.Header.TitleId == id)) r = n;
        return r;
    }

    // Icon: the control data holds one 256x256 JPEG per language (/icon_<language>.dat). Use the English one (American, then British); if neither exists, take the first in NACP language order
    static readonly string[] IconLangs = { "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish", "Spanish", "Italian",
                                           "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese" };
    static (byte[], string) Icon(Nca ctrl) {
        IFileSystem fs = ctrl.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        foreach (string lang in IconLangs) {
            using var f = new UniqueRef<IFile>();
            if (fs.OpenFile(ref f.Ref, $"/icon_{lang}.dat".ToU8Span(), OpenMode.Read).IsFailure()) continue;
            f.Get.GetSize(out long n).ThrowIfFailure();
            byte[] v = new byte[n]; f.Get.Read(out _, 0, v, ReadOption.None).ThrowIfFailure();
            return (v, lang);
        }
        return (null, null);
    }
    static byte[] Nacp(Nca ctrl) {   // the whole control.nacp (0x4000 bytes)
        IFileSystem fs = ctrl.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        using var f = new UniqueRef<IFile>();
        fs.OpenFile(ref f.Ref, "/control.nacp".ToU8Span(), OpenMode.Read).ThrowIfFailure();
        f.Get.GetSize(out long n).ThrowIfFailure();
        byte[] v = new byte[n]; f.Get.Read(out _, 0, v, ReadOption.None).ThrowIfFailure();
        return v;
    }
    // Game name: the NACP starts with 16 language entries of 0x300 bytes each (name 0x200 + publisher 0x100); take the first non-empty one (0 = American English); DisplayVersion is at 0x3060
    //   NACPs from newer SDKs (such as Mario 1.2.1) compress these 0x3000 bytes: a u16 size + raw deflate, inflating to 32 languages × 0x300. LibHac 0.19 does not know this, so it is decoded here
    static string TitleName(byte[] nacp) {
        byte[] t = nacp; int n = nacp[0] | nacp[1] << 8;
        if (n > 0 && n <= 0x2ffe) try {
            using var z = new System.IO.Compression.DeflateStream(new MemoryStream(nacp, 2, n), System.IO.Compression.CompressionMode.Decompress);
            var m = new MemoryStream(); z.CopyTo(m);
            if (m.Length == 0x6000) t = m.ToArray();   // a wrong length means an uncompressed name that happened to inflate
        } catch (InvalidDataException) { }   // not compressed
        for (int i = 0; i < (t == nacp ? 16 : 32); i++) {
            int z = Array.IndexOf(t, (byte)0, i * 0x300, 0x200), len = (z < 0 ? i * 0x300 + 0x200 : z) - i * 0x300;
            if (len > 0) return System.Text.Encoding.UTF8.GetString(t, i * 0x300, len);
        }
        return "";
    }

    sealed class ImageEntry {
        public string Path; public byte[] Name; public bool Directory, Damaged; public long Size; public ulong A, B;
        public List<ImageEntry> Children = new List<ImageEntry>();
    }
    // Stream the merged, integrity-checked filesystem directly into DMROMFS1. No RomFS directories or per-resource host files are created.
    static void WriteImage(Func<IFileSystem> open, string dst) {
        IFileSystem fs = open?.Invoke();
        var root = new ImageEntry { Path = "/", Name = Array.Empty<byte>(), Directory = true };
        var byPath = new Dictionary<string, ImageEntry>(StringComparer.Ordinal) { ["/"] = root };
        if (fs != null) foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string path = e.FullPath.TrimEnd('/'); if (path.Length == 0) continue;
            byte[] name = System.Text.Encoding.UTF8.GetBytes(path.Substring(path.LastIndexOf('/') + 1));
            if (name.Length == 0 || name.Length > 0x300) throw new IOException($"invalid RomFS name length: {path}");
            byPath.Add(path, new ImageEntry { Path = path, Name = name, Directory = e.Type == DirectoryEntryType.Directory, Size = e.Size });
        }
        foreach (var e in byPath.Values) {
            if (e == root) continue;
            string parent = e.Path.Substring(0, e.Path.LastIndexOf('/')); if (parent.Length == 0) parent = "/";
            byPath[parent].Children.Add(e);
        }
        var entries = new List<ImageEntry> { root };
        for (int i = 0; i < entries.Count; i++) if (entries[i].Directory) {
            var e = entries[i]; e.Children.Sort((a, b) => a.Name.AsSpan().SequenceCompareTo(b.Name));
            e.A = (ulong)entries.Count; e.B = (ulong)e.Children.Count; entries.AddRange(e.Children);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(dst));
        string part = dst + ".part"; long files = 0, bytes = 0; int damaged = g_damaged.Count;
        try {
            using (var output = File.Create(part)) using (var writer = new BinaryWriter(output)) {
                writer.Write(new byte[32]); byte[] buffer = new byte[4 << 20];
                foreach (var e in entries) {
                    if (e.Directory) continue;
                    e.A = (ulong)output.Position; e.B = (ulong)e.Size; files++; bytes += e.Size;
                    using var f = new UniqueRef<IFile>();
                    fs.OpenFile(ref f.Ref, e.Path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                    try { WriteStream(f.Get, output, e.Size, buffer); }
                    catch (HorizonResultException x) when (HashError(x)) {
                        fs = Failed(e.Path, open);
                        if (!g_allow) continue;
                        e.Damaged = true;
                        g_lenientFs ??= g_lenient();
                        using var lf = new UniqueRef<IFile>();
                        g_lenientFs.OpenFile(ref lf.Ref, e.Path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                        output.Position = (long)e.A; output.SetLength(output.Position);
                        WriteStream(lf.Get, output, e.Size, buffer);
                    }
                }
                if (g_damaged.Count == damaged || g_allow) {
                    long index = output.Position; using var names = new MemoryStream();
                    foreach (var e in entries) {
                        writer.Write(e.A); writer.Write(e.B); writer.Write(checked((uint)names.Position));
                        writer.Write(checked((ushort)e.Name.Length)); writer.Write((byte)(e.Directory ? 0 : 1)); writer.Write((byte)(e.Damaged ? 1 : 0));
                        names.Write(e.Name);
                    }
                    names.Position = 0; names.CopyTo(output); long size = output.Position - index;
                    output.Position = 0; writer.Write(System.Text.Encoding.ASCII.GetBytes("DMROMFS1"));
                    writer.Write((ulong)index); writer.Write((ulong)size); writer.Write((ulong)entries.Count);
                }
            }
            if (g_damaged.Count != damaged && !g_allow) { File.Delete(part); return; }
            File.Move(part, dst, true);
            Console.WriteLine($"{dst}: {files} files {bytes / 1048576.0:F1} MB, image with {entries.Count} entries");
        } catch { File.Delete(part); throw; }
    }
    static void WriteStream(IFile file, Stream output, long size, byte[] buffer) {
        for (long offset = 0; offset < size;) {
            file.Read(out long got, offset, buffer.AsSpan(0, (int)Math.Min(buffer.Length, size - offset)), ReadOption.None).ThrowIfFailure();
            if (got <= 0) throw new IOException($"cannot read @{offset}");
            output.Write(buffer, 0, (int)got); offset += got;
        }
    }

    // Copy out file by file; files identical to the same path in the compare dir become APFS clones instead
    static void Copy(Func<IFileSystem> open, string dst, string cmpDir) {
        IFileSystem fs = open();
        long n = 0, cloned = 0, bytes = 0, written = 0; byte[] buf = new byte[4 << 20], cb = new byte[4 << 20];
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string rel = e.FullPath.TrimStart('/'), op = Path.Combine(dst, rel);
            if (e.Type == DirectoryEntryType.Directory) { Directory.CreateDirectory(op); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(op)); n++; bytes += e.Size;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            string cp = cmpDir == null ? null : Path.Combine(cmpDir, rel);
            try {
                if (cp != null && File.Exists(cp) && new FileInfo(cp).Length == e.Size && Same(f.Get, cp, e.Size, buf, cb)) {
                    if (File.Exists(op)) File.Delete(op);
                    if (clonefile(cp, op, 0) != 0) throw new IOException($"clonefile {cp} → {op} failed: errno {Marshal.GetLastWin32Error()}");
                    cloned++; continue;
                }
                Write(f.Get, op, e.Size, buf);
            }
            catch (HorizonResultException x) when (HashError(x)) {
                fs = Failed(e.FullPath, open);
                if (!g_allow || g_lenient == null) { File.Delete(op); continue; }
                g_lenientFs ??= g_lenient();
                using var lf = new UniqueRef<IFile>();
                g_lenientFs.OpenFile(ref lf.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                Write(lf.Get, op, e.Size, buf);
            }
            written += e.Size;
        }
        Console.WriteLine($"{dst}: {n} files {bytes / 1048576.0:F1} MB, {cloned} identical to the compare dir (cloned), {written / 1048576.0:F1} MB newly written");
    }
    static void Write(IFile f, string op, long size, byte[] buf) {
        using var o = File.Create(op);
        for (long off = 0; off < size;) {
            f.Read(out long got, off, buf.AsSpan(0, (int)Math.Min(buf.Length, size - off)), ReadOption.None).ThrowIfFailure();
            if (got <= 0) throw new IOException($"cannot read @{off}");
            o.Write(buf, 0, (int)got); off += got;
        }
    }
    // List the RomFS only (path, size, first 32 bytes) without writing game files, except the NRO modules (code the game loads with nn::ro, read like the ExeFS)
    static void Manifest(Func<IFileSystem> open, string path) {
        IFileSystem fs = open();
        long n = 0, bytes = 0; byte[] h = new byte[32];
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        using var o = new StreamWriter(path);
        o.WriteLine("# path\tbytes\tfirst 32 bytes (hex)");
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            if (e.Type == DirectoryEntryType.Directory) continue;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            long got = 0; int want = (int)Math.Min(h.Length, e.Size);
            try { if (want > 0) f.Get.Read(out got, 0, h.AsSpan(0, want), ReadOption.None).ThrowIfFailure(); }
            catch (HorizonResultException x) when (HashError(x)) { fs = Failed(e.FullPath, open); got = 0; }   // the survey goes on; the file is listed as damaged
            o.WriteLine($"{e.FullPath}\t{e.Size}\t{Convert.ToHexString(h, 0, (int)got).ToLowerInvariant()}");
            n++; bytes += e.Size;
            if (got == want && e.FullPath.EndsWith(".nro", StringComparison.OrdinalIgnoreCase)) {
                string op = Path.Combine(Path.GetDirectoryName(path), "romfs_nro", e.FullPath.TrimStart('/'));
                Directory.CreateDirectory(Path.GetDirectoryName(op));
                try { Write(f.Get, op, e.Size, new byte[4 << 20]); } catch (HorizonResultException x) when (HashError(x)) { fs = Failed(e.FullPath, open); File.Delete(op); }
            }
        }
        Console.WriteLine($"{path}: RomFS {n} files {bytes / 1048576.0:F1} MB (manifest only)");
    }
    static bool Same(IFile f, string path, long size, byte[] buf, byte[] cb) {
        using var c = File.OpenRead(path);
        for (long off = 0; off < size;) {
            int want = (int)Math.Min(buf.Length, size - off);
            f.Read(out long got, off, buf.AsSpan(0, want), ReadOption.None).ThrowIfFailure();
            c.ReadExactly(cb, 0, (int)got);
            if (!buf.AsSpan(0, (int)got).SequenceEqual(cb.AsSpan(0, (int)got))) return false;
            off += got;
        }
        return true;
    }
}
