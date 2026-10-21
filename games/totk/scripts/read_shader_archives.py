#!/usr/bin/env python3
"""Read-only BFSHA v9 metadata probe. Writes identifiers/counts, never shader code.

Format references (MIT; consulted, not imported):
https://github.com/KillzXGaming/ShaderLibrary/blob/main/ShaderLibrary/Switch/BfshaLoader.cs
https://github.com/KillzXGaming/ShaderLibrary/blob/main/ShaderLibrary/Structs.cs
MCPK body framing:
https://github.com/UltimateBomb/totk-meshcodec/blob/main/docs/FORMAT.md
This is research tooling, not a source of trusted translator dependencies yet.
"""
import argparse
import json
import struct
from pathlib import Path

from prepare_shader_seed import ZSTD, decoder


class Binary:
    def __init__(self, data):
        self.data = data

    def unpack(self, fmt, at):
        size = struct.calcsize('<' + fmt)
        if at < 0 or at + size > len(self.data):
            raise ValueError('metadata field outside archive')
        return struct.unpack_from('<' + fmt, self.data, at)

    def u64(self, at):
        return self.unpack('Q', at)[0]

    def string(self, at):
        if not at:
            return ''
        size = self.unpack('H', at)[0]
        if at + 2 + size > len(self.data):
            raise ValueError('string outside archive')
        return self.data[at + 2:at + 2 + size].decode('utf-8')

    def dictionary(self, at):
        if not at:
            return []
        count = self.unpack('I', at + 4)[0]
        if count > 100000 or at + 8 + (count + 1) * 16 > len(self.data):
            raise ValueError('invalid resource dictionary')
        return [self.string(self.u64(at + 8 + i * 16 + 8)) for i in range(1, count + 1)]


def options(b, array, names):
    result = []
    for i, name in enumerate(names):
        at = array + i * 40
        if b.string(b.u64(at)) != name:
            raise ValueError('option dictionary/array mismatch')
        choices = b.dictionary(b.u64(at + 8))
        count, default = b.unpack('Hh', at + 24)
        if len(choices) != count or not 0 <= default < count:
            raise ValueError('invalid option choices/default')
        value_array = b.u64(at + 16)
        values = b.unpack('I' * count, value_array) if value_array else [None] * count
        result.append(dict(name=name, choices=dict(zip(choices, values)), default=choices[default],
                           key_offset=b.unpack('B', at + 31)[0], mask=b.unpack('I', at + 32)[0],
                           word=b.unpack('B', at + 36)[0], shift=b.unpack('B', at + 37)[0]))
    return result


def bfsha_metadata(data):
    b = Binary(data)
    if data[:4] != b'FSHA' or b.unpack('H', 10)[0] != 9 or data[12:14] != b'\xff\xfe':
        raise ValueError('probe supports little-endian BFSHA v9 only')
    models = []
    model_array = b.u64(0x48)
    for index, name in enumerate(b.dictionary(b.u64(0x50))):
        at = model_array + index * 0x100
        if b.string(b.u64(at)) != name:
            raise ValueError('model dictionary/array mismatch')
        static, dynamic, count = b.unpack('HHH', at + 228)
        attributes = b.dictionary(b.u64(at + 48))
        samplers = b.dictionary(b.u64(at + 64))
        static_names = b.dictionary(b.u64(at + 16))
        dynamic_names = b.dictionary(b.u64(at + 32))
        if (len(static_names), len(dynamic_names)) != (static, dynamic):
            raise ValueError('option counts differ from dictionary')
        static_metadata = options(b, b.u64(at + 8), static_names)
        dynamic_metadata = options(b, b.u64(at + 24), dynamic_names)
        program_array, bnsh = b.u64(at + 136), b.u64(at + 168)
        if data[bnsh:bnsh + 4] != b'BNSH':
            raise ValueError('model does not reference BNSH')
        variations = b.u64(bnsh + 128)
        variation_count = b.unpack('I', bnsh + 124)[0]
        programs = []
        for i in range(count):
            p = program_array + i * 64
            variation = b.u64(p + 32) - bnsh - variations
            if variation < 0 or variation % 64 or variation // 64 >= variation_count:
                raise ValueError('program variation outside embedded BNSH')
            sampler_array = b.u64(p)
            sampler_count = b.unpack('H', p + 54)[0]
            if sampler_count != len(samplers):
                raise ValueError('program sampler count differs from model')
            locations = {name: dict(zip(('vs', 'ps'), b.unpack('ii', sampler_array + j * 8)))
                         for j, name in enumerate(samplers)}
            programs.append(dict(variation=variation // 64, samplers=locations))
        models.append(dict(name=name, programs=count, embedded_variations=variation_count,
                           static_options=static_metadata, dynamic_options=dynamic_metadata,
                           attributes=attributes, samplers=samplers,
                           program_examples=programs[:2]))
    return dict(archive=b.string(b.u64(0x38)), models=models)


def probe(romfs):
    index = json.loads((romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS must pass integrity verification first')
    entries = {row['path']: row for row in index['files']}
    result = dict(shader_archives=[], model_examples=[], errors=[])
    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise ValueError('short RomFS read')
            return raw
        decode = decoder(romfs, read)
        for name in sorted(entries):
            if name.startswith('Shader/') and name.endswith('.bfsha.zs'):
                try:
                    result['shader_archives'].append(dict(path=name, **bfsha_metadata(decode(read(name)))))
                except (ValueError, struct.error, UnicodeError) as error:
                    result['errors'].append(dict(path=name, error=str(error)))
        # The magicless zstd body contains material metadata; no mesh geometry decode is needed.
        import compression.zstd as zstd
        for name in ('Shader/ExternalBinaryString.bfres.mc', 'Model/Animal_Bass.Bass.bfres.mc'):
            if name not in entries:
                continue
            raw = read(name)
            if raw[:4] != b'MCPK':
                raise ValueError('expected MCPK model')
            body = zstd.ZstdDecompressor().decompress(ZSTD + raw[12:], max_length=64 << 20)
            if body[:4] != b'FRES':
                raise ValueError('MCPK body is not BFRES')
            # Only record presence of expected categories, not an unverified material parse.
            markers = ('gsys_assign_material', 'gsys_assign_gbuffer', 'gsys_weight', 'gsys_renderstate')
            result['model_examples'].append(dict(path=name, body_bytes=len(body),
                                                  markers_present=[s for s in markers if s.encode() in body]))
    result['summary'] = dict(archives=len(result['shader_archives']),
                             models=sum(len(a['models']) for a in result['shader_archives']),
                             programs=sum(m['programs'] for a in result['shader_archives'] for m in a['models']))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = probe(args.romfs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(**report['summary'], errors=report['errors'])))
