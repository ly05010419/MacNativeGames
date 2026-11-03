// NSP extraction: nx_extract <prod.keys> <base NSP> <update NSP or -> <output dir> [base RomFS dir to compare against] [--manifest] [--romfs-image]
// --romfs-image streams the merged RomFS directly to <output dir>/RomFS.img (the packing script's default); only ExeFS is expanded. Without it, legacy folder extraction remains available.
// With a base RomFS dir in legacy mode, files whose content matches it become APFS clones (no new space).
// --manifest: the RomFS is not written, only a list <output dir>/romfs_manifest.tsv (path, size, first 32 bytes), for surveying a new game (a large game's RomFS is well over 10 GB).
// A damaged NSP (a file whose data fails the NCA's hash check, e.g. a copy cut short or a broken download) stops the extraction with exit code 3 after listing every damaged file;
//   --allow-damaged writes those files as read instead and only lists them (Super Mario Galaxy's base NSP with 147 MB zeroed: its two prologue WebM movies).
// Add-on content: nx_extract <prod.keys> --aoc <DLC NSP> <output dir> writes the RomFS of the DLC's data NCA to <output dir> (the packing script puts it at aoc/<index>/, the host mounts it through DM_AOC_DIR).
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
        bool manifest = Array.IndexOf(a, "--manifest") >= 0; a = Array.FindAll(a, s => s != "--manifest");
        bool image = Array.IndexOf(a, "--romfs-image") >= 0; a = Array.FindAll(a, s => s != "--romfs-image");
        g_allow = Array.IndexOf(a, "--allow-damaged") >= 0; a = Array.FindAll(a, s => s != "--allow-damaged");
        if (a.Length < 4) { Console.Error.WriteLine("usage: nx_extract <prod.keys> <base.nsp> <update.nsp|-> <out dir> [base RomFS dir] [--romfs-image|--manifest] [--allow-damaged]\n       nx_extract <prod.keys> --aoc <dlc.nsp> <out dir or image> [--romfs-image]"); return 2; }
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[0]);
        if (a[1] == "--aoc") return Aoc(ks, a[2], a[3], image);
        var basePkg = OpenNsp(ks, a[1], ContentMetaType.Application); var updPkg = a[2] == "-" ? null : OpenNsp(ks, a[2]);
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
        // Code section: the update's ExeFS is complete; opening it layered over the base gives the same result
        IFileSystem exefs = updProg != null ? baseProg.OpenFileSystemWithPatch(updProg, NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid);
        Copy(exefs, Path.Combine(outDir, "ExeFS"), null);
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
        IStorage romfs = updProg != null ? baseProg.OpenStorageWithPatch(updProg, NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        // the same RomFS without hash checks, for reading damaged files as they are (--allow-damaged)
        g_lenient = () => new RomFsFileSystem(updProg != null ? baseProg.OpenStorageWithPatch(updProg, NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid) : baseProg.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid));
        if (manifest) Manifest(new RomFsFileSystem(romfs), Path.Combine(outDir, "romfs_manifest.tsv"));
        else if (image) WriteImage(new RomFsFileSystem(romfs), Path.Combine(outDir, "RomFS.img"));
        else Copy(new RomFsFileSystem(romfs), Path.Combine(outDir, "RomFS"), cmpDir);
        return Damaged();
    }

    // Files whose data failed the hash check: listed at the end; exit code 3 unless --allow-damaged
    static readonly List<string> g_damaged = new List<string>(); static bool g_allow; static Func<IFileSystem> g_lenient; static IFileSystem g_lenientFs;
    static bool HashError(HorizonResultException e) => e.ResultValue.Module == 2 && e.ResultValue.Description >= 4600 && e.ResultValue.Description < 4700;   // fs: integrity verification failures (2002-46xx)
    static int Damaged() {
        if (g_damaged.Count == 0) return 0;
        foreach (string p in g_damaged) Console.WriteLine($"[RED] hash check failed: {p}");
        if (g_allow) { Console.WriteLine($"[RED] the NSP is damaged: {g_damaged.Count} files written as read (--allow-damaged)"); return 0; }
        Console.WriteLine($"[RED] the NSP is damaged: " +
                          $"{g_damaged.Count} files fail the NCA's hash check (an incomplete copy or a broken download); copy or download it again, or pass --allow-damaged to write them as read");
        return 3;
    }

    // DLC NSP: one data NCA (content type PublicData / Data) + a meta NCA + the ticket; only the data NCA's RomFS is written
    static int Aoc(KeySet ks, string nsp, string outDir, bool image) {
        var pkg = OpenNsp(ks, nsp);
        Nca data = Pick(pkg, NcaContentType.PublicData) ?? Pick(pkg, NcaContentType.Data) ?? throw new Exception("no data NCA in the DLC NSP");
        Console.WriteLine($"add-on content {data.Header.TitleId:x16}");
        Directory.CreateDirectory(image ? Path.GetDirectoryName(outDir) : outDir);
        var fs = new RomFsFileSystem(data.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid));
        g_lenient = () => new RomFsFileSystem(data.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid));
        if (image) WriteImage(fs, outDir); else Copy(fs, outDir, null);
        return Damaged();
    }

    // Open an NSP (PFS0) or the secure partition of an XCI, import the common tickets, list all NCAs. With a content meta type, only the NCAs that the package's content metas of
    // that type list (when it has one): a cartridge dump with its update on it (1G+1U) holds the update's NCAs beside the base game's, its Program NCA under the same title ID
    static List<Nca> OpenNsp(KeySet ks, string path, ContentMetaType? only = null) {
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
        }
        if (only == null) return ncas;
        var listed = new HashSet<string>();
        foreach (Nca n in ncas) {
            if (n.Header.ContentType != NcaContentType.Meta) continue;
            IFileSystem mfs = n.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
            foreach (DirectoryEntryEx e in mfs.EnumerateEntries("*.cnmt", SearchOptions.Default)) {
                using var f = new UniqueRef<IFile>();
                mfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
                var cnmt = new Cnmt(f.Release().AsStream());
                if (cnmt.Type == only) foreach (CnmtContentEntry c in cnmt.ContentEntries) listed.Add(Convert.ToHexString(c.NcaId).ToLowerInvariant());
            }
        }
        if (listed.Count == 0) return ncas;
        var kept = new List<Nca>(); int other = 0;   // the meta NCAs list the others, not themselves
        for (int i = 0; i < ncas.Count; i++) if (listed.Contains(ids[i])) kept.Add(ncas[i]); else if (ncas[i].Header.ContentType != NcaContentType.Meta) other++;
        if (!kept.Exists(n => n.Header.ContentType == NcaContentType.Program)) {   // file names that are not content IDs (renamed NCAs): nothing matches, so read every NCA as before
            Console.WriteLine($"[RED] no NCA of {Path.GetFileName(path)} matches its {only} content meta by name (renamed NCA files?); reading all {ncas.Count}");
            return ncas;
        }
        if (other > 0) Console.WriteLine($"leaving out {other} NCAs of {Path.GetFileName(path)} that its {only} content meta does not list (an update on the cartridge)");
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
        public string Path; public byte[] Name; public bool Directory; public long Size; public ulong A, B;
        public List<ImageEntry> Children = new List<ImageEntry>();
    }
    // Stream the merged, integrity-checked filesystem directly into DMROMFS1. No RomFS directories or per-resource host files are created.
    static void WriteImage(IFileSystem fs, string dst) {
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
                        g_damaged.Add(e.Path);
                        if (!g_allow) continue;
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
                        writer.Write(checked((ushort)e.Name.Length)); writer.Write((byte)(e.Directory ? 0 : 1)); writer.Write((byte)0);
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
    static void Copy(IFileSystem fs, string dst, string cmpDir) {
        long n = 0, cloned = 0, bytes = 0, written = 0; byte[] buf = new byte[4 << 20], cb = new byte[4 << 20];
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string rel = e.FullPath.TrimStart('/'), op = Path.Combine(dst, rel);
            if (e.Type == DirectoryEntryType.Directory) { Directory.CreateDirectory(op); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(op)); n++; bytes += e.Size;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            string cp = cmpDir == null ? null : Path.Combine(cmpDir, rel);
            if (cp != null && File.Exists(cp) && new FileInfo(cp).Length == e.Size && Same(f.Get, cp, e.Size, buf, cb)) {
                if (File.Exists(op)) File.Delete(op);
                if (clonefile(cp, op, 0) != 0) throw new IOException($"clonefile {cp} → {op} failed: errno {Marshal.GetLastWin32Error()}");
                cloned++; continue;
            }
            try { Write(f.Get, op, e.Size, buf); }
            catch (HorizonResultException x) when (HashError(x)) {
                g_damaged.Add(e.FullPath);
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
    // List the RomFS only (path, size, first 32 bytes) without writing game files
    static void Manifest(IFileSystem fs, string path) {
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
            catch (HorizonResultException x) when (HashError(x)) { g_damaged.Add(e.FullPath); got = 0; }   // the survey goes on; the file is listed as damaged
            o.WriteLine($"{e.FullPath}\t{e.Size}\t{Convert.ToHexString(h, 0, (int)got).ToLowerInvariant()}");
            n++; bytes += e.Size;
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
