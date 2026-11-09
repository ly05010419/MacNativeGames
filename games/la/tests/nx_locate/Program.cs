// nx_locate: for the integrity tests (tests/integrity_mutations.py), never for the build.
//   locate <prod.keys> <base> <update> <tolerated base meta sha256 | -> <RomFS path>: where the file's midpoint is in the merged section (V), which relocation entry
//     covers [V, V+64), and, when that entry reads the base game, the base section offset and NCA offset of the same bytes. JSON on stdout.
//   window <prod.keys> <base> <update> <tolerated | -> <V> <base section offset>: 64 bytes of the merged section at V and of the base section there (hex), read
//     without hash checks, so a changed byte in the base shows whether the merged RomFS reads it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;

static class P {
    const int Window = 64, NodeSize = 0x4000, EntrySize = 0x14;

    static int Main(string[] a) {
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[1]);
        var (b, u) = Select.Programs(ks, Open(ks, a[2]), Open(ks, a[3]), true, a[4] == "-" ? null : a[4]);
        if (a[0] == "window") {
            long wv = long.Parse(a[5]), wb = long.Parse(a[6]);
            Console.WriteLine($"merged {Hex(b.OpenRawStorageWithPatch(u, 1), wv)}");
            Console.WriteLine($"base {Hex(b.OpenRawStorage(1), wb)}");
            return 0;
        }
        IStorage image = b.OpenStorageWithPatch(u, NcaSectionType.Data, IntegrityCheckLevel.None), merged = b.OpenRawStorageWithPatch(u, 1);
        var rom = new RomFsFileSystem(image);
        if (!rom.FileTable.TryOpenFile(a[5], out RomFileInfo info)) { Console.Error.WriteLine($"no file {a[5]}"); return 2; }
        long r = rom.Header.DataOffset + info.Offset + info.Length / 2;
        long data = DataLevel(u, image, merged);
        long v = data + r;
        if (Hex(merged, v) != Hex(image, r)) { Console.Error.WriteLine("the data level offset does not map the image onto the section"); return 2; }
        var entries = Relocation(u);
        int k = entries.FindLastIndex(e => e.Virtual <= v);
        long end = k + 1 < entries.Count ? entries[k + 1].Virtual : long.MaxValue;
        var e0 = entries[k];
        bool fromBase = e0.Storage == 0 && v + Window <= end;
        long pb = e0.Physical + (v - e0.Virtual);
        Console.WriteLine("{" + $"\"file\":\"{a[5]}\",\"file_size\":{info.Length},\"image_offset\":{r},\"data_level\":{data},\"section_offset\":{v}," +
            $"\"entry\":{{\"virtual\":{e0.Virtual},\"physical\":{e0.Physical},\"storage\":{e0.Storage},\"end\":{(end == long.MaxValue ? -1 : end)}}},\"entries\":{entries.Count}," +
            $"\"from_base\":{(fromBase ? "true" : "false")},\"base_section_offset\":{pb},\"base_section_start\":{b.Header.GetSectionStartOffset(1)}," +
            $"\"base_encryption\":\"{b.GetFsHeader(1).EncryptionType}\",\"nca_offset\":{b.Header.GetSectionStartOffset(1) + pb},\"window\":\"{Hex(image, r)}\"" + "}");
        return 0;
    }

    // The data level: the IVFC level whose offset puts the RomFS image's first bytes in the merged section
    static long DataLevel(Nca u, IStorage image, IStorage merged) {
        var ivfc = u.GetFsHeader(1).GetIntegrityInfoIvfc();
        var hits = Enumerable.Range(0, ivfc.LevelCount).Select(i => ivfc.GetLevelOffset(i)).Where(o => Hex(merged, o) == Hex(image, 0)).ToList();
        if (hits.Count != 1) throw new Exception($"{hits.Count} IVFC levels match the RomFS image");
        return hits[0];
    }

    record Entry(long Virtual, long Physical, int Storage);
    // The update's relocation table (a bucket tree: an offset node, then entry sets of {virtual, physical, storage index}), checked for order and count
    static List<Entry> Relocation(Nca u) {
        var info = u.GetFsHeader(1).GetPatchInfo();
        byte[] head = info.RelocationTreeHeader.ToArray();
        if (head[0] != 'B' || head[1] != 'K' || head[2] != 'T' || head[3] != 'R') throw new Exception("no BKTR relocation header");
        int count = BitConverter.ToInt32(head, 8);
        byte[] t = new byte[info.RelocationTreeSize];
        u.OpenRawStorage(1).Read(info.RelocationTreeOffset, t).ThrowIfFailure();
        int sets = BitConverter.ToInt32(t, 4);
        var list = new List<Entry>();
        for (int s = 0; s < sets; s++) {
            int at = NodeSize * (1 + s), n = BitConverter.ToInt32(t, at + 4);
            for (int i = 0; i < n; i++) {
                int o = at + 0x10 + i * EntrySize;
                list.Add(new Entry(BitConverter.ToInt64(t, o), BitConverter.ToInt64(t, o + 8), BitConverter.ToInt32(t, o + 16)));
            }
        }
        if (list.Count != count || list[0].Virtual != 0 || list.Zip(list.Skip(1)).Any(p => p.Second.Virtual <= p.First.Virtual) || list.Any(e => e.Storage is not (0 or 1)))
            throw new Exception($"relocation table does not parse: {list.Count} entries of {count}");
        return list;
    }

    static string Hex(IStorage st, long at) { byte[] w = new byte[Window]; st.Read(at, w).ThrowIfFailure(); return Convert.ToHexString(w).ToLowerInvariant(); }

    // An NSP (PFS0) or an XCI's secure partition, with its common tickets imported (as nx_extract's OpenNsp)
    static List<Select.Packaged> Open(KeySet ks, string path) {
        var st0 = new LocalStorage(path, FileAccess.Read); byte[] m = new byte[4]; st0.Read(0x100, m).ThrowIfFailure(); IFileSystem pfs;
        if (System.Text.Encoding.ASCII.GetString(m) == "HEAD") pfs = new Xci(ks, st0).OpenPartition(XciPartitionType.Secure);
        else { var p = new PartitionFileSystem(); p.Initialize(st0).ThrowIfFailure(); pfs = p; }
        var outList = new List<Select.Packaged>();
        foreach (DirectoryEntryEx e in pfs.EnumerateEntries("*", SearchOptions.Default)) {
            using var f = new UniqueRef<IFile>(); pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure(); IStorage st = f.Release().AsStorage();
            if (e.Name.EndsWith(".tik")) { byte[] t = new byte[0x2c0]; st.Read(0, t).ThrowIfFailure(); ks.ExternalKeySet.Add(new RightsId(t.AsSpan(0x2a0, 0x10)), new AccessKey(t.AsSpan(0x180, 0x10))).ThrowIfFailure(); }
            else if (e.Name.EndsWith(".nca")) outList.Add(new Select.Packaged(e.Name, st, new Nca(ks, st)));
        }
        return outList;
    }
}
