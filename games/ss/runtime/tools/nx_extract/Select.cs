// Which programs of a package are the base game and the update, decided by their content metas (Skyward Sword HD 1.0.1 over the base game).
// The cartridge dump carries the base game and patch 1.0.1, and both Program NCAs have the application's title ID in their headers, so a header alone cannot tell them
// apart: the first one listed was taken as the base (the 36 MB patch), which gave the right ExeFS (an update's is complete) and failed on the RomFS.
// Exit codes: 4 no single program as the metas name it, 5 a header signature that is not valid, 6 an NCA whose SHA-256 is not the one its meta records.
// One signature can be tolerated: the base game's meta, when its own SHA-256 is the one --tolerated-base-meta gives (extract_ss.py passes it for the one cartridge dump
// assets/integrity-exceptions.json names). Never an update's meta, never a program's. Such a meta still selects the program, and the version it declares is then not authenticated.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Ncm;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.Ncm;
using ContentType = LibHac.Ncm.ContentType;

static class Select {
    public const ulong Application = 0x01002DA013484000, Patch = 0x01002DA013484800;
    public const int BaseVersion = 0, UpdateVersion = 65536;

    public sealed class Stop : Exception {
        public readonly int Code;
        public Stop(int code, string message) : base(message) { Code = code; }
    }
    public sealed record Packaged(string Name, IStorage Storage, Nca Nca);

    public static (Nca, Nca) Programs(KeySet ks, List<Packaged> baseFiles, List<Packaged> updFiles, bool skipHash, string toleratedMeta) {
        Nca b = One(baseFiles, ContentMetaType.Application, Application, BaseVersion, false, "base game", skipHash, toleratedMeta);
        return (b, updFiles == null ? null : One(updFiles, ContentMetaType.Patch, Patch, UpdateVersion, true, "update", skipHash, null));
    }

    static Nca One(List<Packaged> files, ContentMetaType type, ulong title, int version, bool patch, string what, bool skipHash, string toleratedMeta) {
        var metas = files.Where(f => f.Nca.Header.ContentType == NcaContentType.Meta).Select(f => (f, Meta(f))).ToList();
        var wanted = metas.Where(m => m.Item2.Type == type && m.Item2.TitleId == title && m.Item2.TitleVersion.Version == (uint)version).ToList();
        if (wanted.Count != 1) throw new Stop(4, $"{what}: {wanted.Count} content metas of type {type}, title {title:x16}, version {version} (one is needed)");
        var (metaFile, meta) = wanted[0];
        if (patch && meta.ApplicationTitleId != Application) throw new Stop(4, $"{what}: the patch is of application {meta.ApplicationTitleId:x16}");
        var programs = meta.ContentEntries.Where(c => c.Type == ContentType.Program).ToList();
        if (programs.Count != 1) throw new Stop(4, $"{what}: its meta lists {programs.Count} Program contents (one is needed)");
        string id = Convert.ToHexString(programs[0].NcaId).ToLowerInvariant();
        var found = files.Where(f => f.Name.Equals(id + ".nca", StringComparison.OrdinalIgnoreCase)).ToList();
        if (found.Count != 1) throw new Stop(4, $"{what}: {found.Count} files named {id}.nca in the package (one is needed)");
        Packaged p = found[0];
        if (p.Nca.Header.ContentType != NcaContentType.Program || p.Nca.Header.TitleId != Application)
            throw new Stop(4, $"{what}: {id} is {p.Nca.Header.ContentType} of title {p.Nca.Header.TitleId:x16}");
        bool isPatch = p.Nca.SectionExists(1) && p.Nca.GetFsHeader(1).IsPatchSection();
        if (isPatch != patch) throw new Stop(4, $"{what}: {id} {(isPatch ? "is" : "is not")} a patch section");
        Console.WriteLine($"[select] {what}: {id} version {version} size {p.Nca.Header.NcaSize} patch section {isPatch}");
        Validity metaSig = metaFile.Nca.VerifyHeaderSignature(), programSig = p.Nca.VerifyHeaderSignature();
        string metaSha = Sha256(metaFile.Storage);
        Console.WriteLine($"[signature] {what} meta: {metaSig} sha256 {metaSha}");
        if (metaSig != Validity.Valid) {
            if (toleratedMeta == null || metaSha != toleratedMeta) throw new Stop(5, $"{what}: the meta NCA's header signature is {metaSig}");
            Console.WriteLine($"[tolerated] {what} meta: invalid signature accepted by SHA-256; the version it declares is not authenticated");
        }
        Console.WriteLine($"[signature] {what} program: {programSig}");
        if (programSig != Validity.Valid) throw new Stop(5, $"{what}: the program NCA's header signature is {programSig}");
        if (skipHash) { Console.WriteLine($"[hash] {what} program: NOT checked (--skip-content-hash, integrity test)"); return p.Nca; }
        string got = Sha256(p.Storage), want = Convert.ToHexString(programs[0].Hash).ToLowerInvariant();
        Console.WriteLine($"[hash] {what} program: computed {got} expected {want}");
        if (got != want) throw new Stop(6, $"{what}: {id} SHA-256 {got}, its meta records {want}");
        return p.Nca;
    }

    static Cnmt Meta(Packaged f) {
        IFileSystem fs = f.Nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        DirectoryEntryEx e = fs.EnumerateEntries("*.cnmt", SearchOptions.Default).Single();
        using var file = new UniqueRef<IFile>();
        fs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        return new Cnmt(file.Release().AsStream());
    }

    static string Sha256(IStorage st) {
        st.GetSize(out long size).ThrowIfFailure();
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buf = new byte[4 << 20];
        for (long off = 0; off < size;) {
            int n = (int)Math.Min(buf.Length, size - off);
            st.Read(off, buf.AsSpan(0, n)).ThrowIfFailure();
            h.AppendData(buf, 0, n); off += n;
        }
        return Convert.ToHexString(h.GetHashAndReset()).ToLowerInvariant();
    }
}
