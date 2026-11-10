#!/usr/bin/env python3
"""Stream a multipart RAR containing one OoT NUP directly into the build's ExeFS and RomFS."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from import_nup import ROOT, NupReader, extraction_output, package_files
from nup import format as nup_format

CHUNK = 8 << 20


def read_exact(stream, count):
    data = stream.read(count)
    if len(data) != count:
        raise ValueError("The RAR ended before the NUP was complete. Check all archive parts.")
    return data


def copy_archive(stream, source, output, scratch, finish):
    header = read_exact(stream, nup_format.HEADER_SIZE)
    superblock = nup_format.parse_super(header)
    toc_end = superblock["toc_offset"] + superblock["toc_size"]
    total = superblock["file_size"]
    if not len(header) <= toc_end <= min(total, 128 << 20):
        raise ValueError("Unexpected NUP table size")
    metadata = scratch / "metadata"
    with metadata.open("wb") as f:
        f.write(header)
        f.write(read_exact(stream, toc_end - len(header)))
        f.truncate(total)  # sparse: the validated reader only needs the superblock and table
    with NupReader(str(metadata)) as reader:
        code, data, directories = package_files(reader, 131072)
        identity = {"toc_sha256": reader.super["toc_sha256"].hex(), "version": 131072}
        expected = json.loads((ROOT / "assets/expected-v131072.json").read_text())
        code_sha = {path.removeprefix("exefs/"): reader.files[i][2].hex() for path, i in code}
        if any(code_sha.get(name) != digest for name, digest in expected["exefs_sha256"].items()):
            raise ValueError("Unsupported or damaged original ExeFS")
        romfs = output / "romfs"
        index = romfs / "index.json"
        image = romfs / "romfs.bin"
        if index.is_file() and image.is_file():
            previous = json.loads(index.read_text())
            if (previous.get("source_identity") != identity
                    or previous.get("romfs_sha256") != expected["romfs"]["sha256"]
                    or image.stat().st_size != expected["romfs"]["size"]):
                raise ValueError("Existing RomFS has different provenance; use another work directory")
            print("Reusing the verified RAR RomFS", flush=True)
            return
        if image.exists() or index.exists() or (output / "exefs").exists():
            raise ValueError("Existing incomplete extraction was left untouched; use another work directory")

        extents = defaultdict(list)
        rows = []
        code_manifest = []
        end = 0
        for path, i in data:
            _, size, sha, offset, stored, codec = reader.files[i]
            end = (end + 15) & ~15
            rows.append({"path": path, "offset": end, "size": size})
            extents[offset].append((size, sha, stored, codec, end))
            end += size
        for path, i in code:
            _, size, sha, offset, stored, codec = reader.files[i]
            target = scratch / path
            target.parent.mkdir(parents=True, exist_ok=True)
            code_manifest.append({"name": path.removeprefix("exefs/"), "size": size, "sha256": sha.hex()})
            extents[offset].append((size, sha, stored, codec, target))
    if end != expected["romfs"]["size"]:
        raise ValueError("Unexpected OoT RomFS size")
    if shutil.disk_usage(output.parent).free < end + (3 << 30):
        raise ValueError(f"Not enough disk space for {end/2**30:.1f} GiB of RomFS plus reserve")

    position = toc_end
    last = time.monotonic()
    staged = scratch / "romfs.bin"
    with staged.open("w+b", buffering=0) as out:
        out.truncate(end)
        for offset, members in sorted(extents.items()):
            size, wanted, stored, codec, _ = members[0]
            if any(member[:4] != members[0][:4] for member in members):
                raise ValueError("Conflicting shared NUP extent")
            if codec != nup_format.CODEC_RAW or stored != size:
                raise ValueError("This streaming path requires raw NUP extents")
            if size and offset < position:
                raise ValueError("Overlapping NUP extents")
            while size and position < offset:
                position += len(read_exact(stream, min(CHUNK, offset - position)))
            digest = hashlib.sha256()
            handles = []
            try:
                for _, _, _, _, target in members:
                    if isinstance(target, Path):
                        handles.append(target.open("wb"))
                written = 0
                while written < size:
                    block = read_exact(stream, min(CHUNK, size - written))
                    digest.update(block)
                    for handle in handles:
                        handle.write(block)
                    for _, _, _, _, target in members:
                        if isinstance(target, int):
                            if os.pwrite(out.fileno(), block, target + written) != len(block):
                                raise OSError("Short RomFS write")
                    written += len(block)
                    position += len(block)
                if digest.digest() != wanted:
                    raise ValueError(f"File SHA-256 mismatch at archive offset {offset}")
            finally:
                for handle in handles:
                    handle.close()
            if time.monotonic() - last > 15:
                print(f"RAR: {position/2**30:.2f} / {total/2**30:.2f} GiB read", flush=True)
                last = time.monotonic()
        while position < total:
            position += len(read_exact(stream, min(CHUNK, total - position)))
        if stream.read(1):
            raise ValueError("Expected exactly one NUP in the RAR")
        finish()
        print("Verifying the complete RomFS image", flush=True)
        out.seek(0)
        sha = hashlib.file_digest(out, "sha256").hexdigest()
    if sha != expected["romfs"]["sha256"]:
        raise ValueError("Unsupported RomFS image SHA-256")
    publish = scratch / "verified"
    (publish / "romfs").mkdir(parents=True)
    (scratch / "exefs").rename(publish / "exefs")
    staged.rename(publish / "romfs/romfs.bin")
    (publish / "extraction-manifest.json").write_text(json.dumps(
        dict(source=str(source), container="RAR/NUP", toc_sha256=identity["toc_sha256"], files=code_manifest)) + "\n")
    (publish / "romfs/index.json").write_text(json.dumps(
        dict(status="pass", source=str(source), source_identity=identity,
             integrity="NUP TOC, archive CRC, file SHA-256 and original RomFS image verified",
             romfs_size=end, romfs_sha256=sha, files=rows, directories=directories)) + "\n")
    publish.rename(output)
    print(f"Verified {len(rows):,} RomFS files and all original ExeFS modules", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tool = os.environ.get("OOT_7ZIP") or shutil.which("7zz") or shutil.which("7z")
    if not tool:
        parser.error("Install sevenzip or set OOT_7ZIP to the 7zz executable")
    output = extraction_output(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".rar-import-", dir=output.parent) as scratch:
        with subprocess.Popen([tool, "x", "-so", str(args.archive.resolve()), "*.nup"], stdout=subprocess.PIPE) as proc:
            def finish():
                if proc.wait():
                    raise ValueError("7-Zip could not verify the multipart archive")
            try:
                copy_archive(proc.stdout, args.archive.resolve(), output, Path(scratch), finish)
            finally:
                if proc.poll() is None:
                    proc.kill()


if __name__ == "__main__":
    main()
