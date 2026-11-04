#!/usr/bin/env python3
"""Prepare optional Mac TOTK textures for an installed app, without changing the source pack or the app's stock data.

Usage: install_texture_pack.py <pack's RomFS> --app <app> --version v143 --output <external folder>
Then put the output's absolute path in texture-pack-path.txt beside controls.txt. The pack is off by default and has no
settings row; TOTK_TEXTURE_PACK=1 explicitly enables it for a launch. All resources retain the usual SHA-256 checks.
"""
import argparse
import hashlib
import json
import shutil
import struct
import zlib
from pathlib import Path
from compression import zstd

from make_mod_manifest import HEAPDEF, MANIFEST_HEADER, app_image_dir, read_index, sha256_range
from texture_pack_tables import fit_heapdef, grow_restbl, heap_size_fields, restbl_entries


def keep_pack_budgets(table, pack_tables, paths):
    """Keep the pack's declared allocation for each replaced texture, matched by full path or resource CRC32.

    Start with this game's table: unrelated records and collision slots from other versions are never imported.
    """
    out = bytearray(table)
    crcs, names = restbl_entries(table)
    raised = set()
    for source in pack_tables:
        src_crcs, src_names = restbl_entries(source)
        for path in paths:
            crc = zlib.crc32(path.encode())
            dst, src = names.get(path, crcs.get(crc)), src_names.get(path, src_crcs.get(crc))
            if dst is None or src is None:
                continue
            budget = struct.unpack_from('<I', source, src)[0]
            if budget > struct.unpack_from('<I', out, dst)[0]:
                struct.pack_into('<I', out, dst, budget)
                raised.add(path)
    return bytes(out), len(raised)


def resolution_heap(original, fitted, canvas):
    """Carry exactly the installed app's two canvas-heap increments onto the fitted texture-pack table."""
    base, out_fields, canvas_fields = (heap_size_fields(b) for b in (original, fitted, canvas))
    out = bytearray(fitted)
    for name, at in base.items():
        old = struct.unpack_from('<i', original, at)[0]
        new = struct.unpack_from('<i', canvas, canvas_fields[name])[0]
        if new != old:
            if name not in ('Graphics', 'DynamicTextureAllocatorMem') or new < old:
                raise ValueError(f'app canvas changes unrelated heap {name}; rebuild stock canvas overlays first')
            target = out_fields[name]
            value = struct.unpack_from('<i', fitted, target)[0] + new - old
            struct.pack_into('<i', out, target, value)
    return bytes(out)


def prepare(pack, app, version, output):
    pack, app, output = (p.expanduser().resolve() for p in (pack, app, output))
    if output.exists():
        raise ValueError(f'{output} already exists; choose a new output folder')
    if output.is_relative_to(pack):
        raise ValueError('output must be outside the source pack')
    res = app / 'Contents/Resources/totk'
    romfs = res / f'extracted/{version}/romfs'
    index = read_index(romfs / 'runtime-index.bin')
    image_dir = romfs if (romfs / 'romfs.bin').is_file() else app_image_dir(app, version)
    files = sorted((pack / 'TexToGo').rglob('*.txtg'))
    if not files or not (pack / HEAPDEF).is_file():
        raise ValueError('pack must contain TexToGo/*.txtg and its System/HeapSize table')
    unknown = [str(f.relative_to(pack)) for f in files if f.relative_to(pack).as_posix() not in index]
    if unknown:
        raise ValueError(f'{len(unknown)} textures absent from this game version: {unknown[:5]}')
    stage = output.with_name(output.name + '.partial')
    if stage.exists():
        raise ValueError(f'{stage} already exists; inspect the incomplete installation first')
    stage.mkdir(parents=True)
    try:
        with (image_dir / 'romfs.bin').open('rb') as image:
            def read(name):
                offset, size = index[name]
                image.seek(offset)
                data = image.read(size)
                if len(data) != size:
                    raise ValueError('RomFS ends early')
                return data

            def write_manifest(folder, replacements):
                rows = []
                for name, raw in sorted(replacements.items()):
                    target = folder / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(raw)
                    rows.append(f'{name}\t{len(raw)}\t{hashlib.sha256(raw).hexdigest()}\t{hashlib.sha256(read(name)).hexdigest()}')
                (folder / 'manifest.tsv').write_text(MANIFEST_HEADER + '\n'.join(rows) + '\n')

            heap_raw = read(HEAPDEF)
            original = zstd.decompress(heap_raw)
            fitted, raised = fit_heapdef(original, zstd.decompress((pack / HEAPDEF).read_bytes()))
            growth = {f.relative_to(pack).as_posix(): max(0, f.stat().st_size - index[f.relative_to(pack).as_posix()][1]) for f in files}
            table_name = next(n for n in index if n.startswith('System/Resource/ResourceSizeTable.') and n.endswith('.rsizetable.zs'))
            table = zstd.decompress(read(table_name))
            table, grown, missing = grow_restbl(table, table, growth)
            if missing:
                raise ValueError(f'{len(missing)} textures have no resource size entry: {missing[:5]}')
            budget_files = sorted((pack / 'System/Resource').glob('*.rsizetable.zs'))
            table, budgets = keep_pack_budgets(table, [zstd.decompress(f.read_bytes()) for f in budget_files], growth)
            write_manifest(stage, {table_name: zstd.compress(table, level=19)})
            rows = (stage / 'manifest.tsv').read_text().splitlines()
            for file in files:
                name = file.relative_to(pack).as_posix()
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, target)
                with target.open('rb') as handle:
                    digest = sha256_range(handle, 0, target.stat().st_size)
                offset, size = index[name]
                rows.append(f'{name}\t{target.stat().st_size}\t{digest}\t{sha256_range(image, offset, size)}')
            (stage / 'manifest.tsv').write_text('\n'.join(rows) + '\n')
            write_manifest(stage / 'heap-default', {HEAPDEF: zstd.compress(fitted, level=19)})
            modes = []
            for manifest in sorted((res / f'extracted/{version}/romfs-patches-out').glob('*/manifest.tsv')):
                entries = [line.split('\t') for line in manifest.read_text().splitlines() if line and not line.startswith('#')]
                if len(entries) != 1 or entries[0][0] != HEAPDEF:
                    raise ValueError(f'{manifest}: expected a canvas-only heap overlay')
                _, length, digest, orig = entries[0]
                raw = (manifest.parent / HEAPDEF).read_bytes()
                if len(raw) != int(length) or hashlib.sha256(raw).hexdigest() != digest or hashlib.sha256(heap_raw).hexdigest() != orig:
                    raise ValueError(f'{manifest}: canvas overlay integrity mismatch')
                canvas = resolution_heap(original, fitted, zstd.decompress(raw))
                write_manifest(stage / 'romfs-patches-out' / manifest.parent.name, {HEAPDEF: zstd.compress(canvas, level=19)})
                modes.append(manifest.parent.name)
        report = dict(version=version, source=str(pack), app=str(app), textures=len(files), resource_entries_grown=grown,
                      pack_budget_entries_raised=budgets,
                      pack_budget_sha256={f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in budget_files},
                      raised_heaps=raised, canvas_modes=modes,
                      installer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        (stage / 'installation.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        stage.rename(output)
        return report
    except BaseException:
        shutil.rmtree(stage)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--version', choices=('v0', 'v121', 'v143'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.pack, args.app, args.version, args.output), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
