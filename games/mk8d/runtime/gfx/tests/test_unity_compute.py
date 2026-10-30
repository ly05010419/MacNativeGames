"""Stripped ComputeShader objects: legacy kernels and Unity 2021.3 keyword variants."""
from pathlib import Path
import contextlib
import io
import json
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split
import unityfs


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


def legacy_compute():
    # Unity 2019.4 release layout: one platform, one named kernel, no keyword variants.
    code = shader_split.CS_MAGIC + bytes(60)
    control = bytearray(0x880)
    control[:4] = shader_split.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    struct.pack_into('<III', control, 0x748, 64, 1, 1)
    header = [0] * 6 + [-1] * 6 + [0] * 12
    header[11], header[17], header[23] = 0, len(control), len(code)
    program = struct.pack('<24i', *header) + control + code
    resource = string('Result') + string('ResultBuffer') + struct.pack('<iii', 0, -1, 1)
    kernel = (string('SkinVertices') + struct.pack('<4i', 0, 0, 0, 0)
              + struct.pack('<i', 1) + resource + struct.pack('<i', len(program)) + program
              + struct.pack('<4I', 3, 64, 1, 1))
    param = string('Count') + struct.pack('<5i', 0, 0, 1, 1, 1)
    cb = string('Params') + struct.pack('<ii', 16, 1) + param
    payload = (string('LegacySkinning') + struct.pack('<4i', 1, 38, 50, 1) + kernel
               + struct.pack('<i', 1) + cb + b'\1\0\0\0')
    type_hash = bytes.fromhex('1a95529ac2eb1a97cd9d2a4356bd722f')
    metadata = (b'2019.4.27f1\0' + struct.pack('<i?iiBh', 38, False, 1, 72, 0, -1)
                + type_hash + struct.pack('<i', 1))
    metadata += bytes(-(20 + len(metadata)) % 4)
    metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
    data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
            + bytes(4) + metadata).ljust(0x100, b'\0') + payload
    return data, type_hash, bytes(control), code


class LegacyComputeTests(unittest.TestCase):
    def test_stripped_2019_kernel_reaches_shader_split(self):
        data, type_hash, control, code = legacy_compute()
        trees = {(72, type_hash): unityfs.known_tree(72, type_hash)}
        objects = list(unityfs.serialized_objects(data, {}, {72}, trees))
        self.assertEqual(objects[0][2]['variants'][0]['resourcesResolved'], True)
        shaders = shader_split.unity_compute(data, 'fixture', trees)
        self.assertEqual(shaders, [('fixture#LegacySkinning#SkinVertices.0', 'cs', control, code)])

    def test_stripped_2017_counters_and_kernel_reach_shader_split(self):
        data, old_hash, control, code = legacy_compute()
        type_hash = bytes.fromhex('20291e020d10074c35d8286f5191593f')
        # The 2017 resource's counter is two ints, like the two differently named
        # fields in 2019, so the same independent wire fixture exercises both trees.
        data = data.replace(old_hash, type_hash).replace(b'2019.4.27f1\0', b'2017.4.28f1\0')
        trees = {(72, type_hash): unityfs.known_tree(72, type_hash)}
        objects = list(unityfs.serialized_objects(data, {}, {72}, trees))
        variant = objects[0][2]['variants'][0]
        resource = variant['kernels'][0]['outBuffers'][0]
        self.assertEqual(resource, {'name': 'Result', 'generatedName': 'ResultBuffer', 'bindPoint': 0,
                                    'counter': {'bindpoint': -1, 'offset': 1}})
        self.assertEqual(variant['constantBuffers'][0]['params'][0]['colCount'], 1)
        self.assertTrue(variant['resourcesResolved'])
        le, doff, types, entries = unityfs._tables(data, {}, {72}, trees)
        _, start, size, ti = entries[0]
        reader = unityfs.Reader(data[doff + start:doff + start + size], le)
        unityfs._read(reader, types[ti][2])
        self.assertEqual(reader.p, size)
        log = io.StringIO()
        with contextlib.redirect_stdout(log): shaders = shader_split.unity_compute(data, 'fixture', trees)
        self.assertEqual(shaders, [('fixture#LegacySkinning#SkinVertices.0', 'cs', control, code)])
        lines = log.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertEqual(json.loads(lines[0].removeprefix('unity compute source: ')),
                         [shader_split.skey(control, code), 'fixture#LegacySkinning#SkinVertices.0'])


def compute_2021_3(platforms=1, code_padding=0):
    # Independent wire fixture from the 2021.3.18f1 release dump (PR #440).
    # Several platforms expose the alignment after each resourcesResolved bool;
    # odd-sized code exposes the byte vector's padding before threadGroupSize.
    code = shader_split.CS_MAGIC + bytes(60 + code_padding)
    control = bytearray(0x880)
    control[:4] = shader_split.CTL_MAGIC
    struct.pack_into('<I', control, 0x20, len(control))
    struct.pack_into('<I', control, 0x704, len(code))
    struct.pack_into('<III', control, 0x748, 64, 1, 1)
    header = [0] * 6 + [-1] * 6 + [0] * 12
    header[11], header[17], header[23] = 0, len(control), len(code)
    program = struct.pack('<24i', *header) + control + code
    resource = string('Result') + string('ResultBuffer') + struct.pack('<iii', 0, -1, 1)
    kernel = (struct.pack('<iI', 1, 7)                         # cbVariantIndices
              + struct.pack('<i', 1) + resource              # cbs
              + struct.pack('<4i', 0, 0, 0, 0)               # textures, samplers, in/out buffers
              + struct.pack('<i', len(program)) + program + bytes(-len(program) % 4)
              + struct.pack('<4Iq', 3, 64, 1, 1, -0x123456789ab))
    parent = (string('SkinVertices') + struct.pack('<i', 2)
              + string('') + kernel + string('SKIN_NORMALS') + kernel
              + struct.pack('<i', 1) + string('GLOBAL')
              + struct.pack('<i', 1) + string('SKIN_NORMALS'))
    param = string('Count') + struct.pack('<i4I', 0, 4, 2, 3, 4)
    cb = string('Params') + struct.pack('<ii', 64, 1) + param
    variants = [struct.pack('<iii', 38 + i, 50, 1) + parent
                + struct.pack('<i', 1) + cb + b'\1\0\0\0' for i in range(platforms)]
    payload = string('Skinning2021') + struct.pack('<i', platforms) + b''.join(variants)
    type_hash = bytes.fromhex('abd9135b8ce83d043fef4e9ec7f53366')
    metadata = (b'2021.3.18f1\0' + struct.pack('<i?iiBh', 38, False, 1, 72, 0, -1)
                + type_hash + struct.pack('<i', 1))
    metadata += bytes(-(20 + len(metadata)) % 4)
    metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
    data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
            + bytes(4) + metadata).ljust(0x100, b'\0') + payload
    return data, type_hash, bytes(control), code


class Compute2021Tests(unittest.TestCase):
    def test_stripped_2021_kernel_variants_reach_shader_split(self):
        data, type_hash, control, code = compute_2021_3()
        with self.assertRaises(unityfs.NoTree):
            list(unityfs.serialized_objects(data, {}, {72}, {}))
        trees = {(72, type_hash): unityfs.known_tree(72, type_hash)}
        log = io.StringIO()
        with contextlib.redirect_stdout(log): shaders = shader_split.unity_compute(data, 'fixture', trees)
        sources = [f'fixture#Skinning2021#SkinVertices.{i}' for i in range(2)]
        self.assertEqual(shaders, [(source, 'cs', control, code) for source in sources])
        evidence = [json.loads(line.removeprefix('unity compute source: ')) for line in log.getvalue().splitlines()]
        self.assertEqual(evidence, [[shader_split.skey(control, code), source] for source in sources])

    def test_platform_alignment_and_byte_code_padding(self):
        for padding in range(4):
            with self.subTest(padding=padding):
                data, type_hash, _, _ = compute_2021_3(platforms=2, code_padding=padding)
                trees = {(72, type_hash): unityfs.known_tree(72, type_hash)}
                obj, = unityfs.serialized_objects(data, {}, {72}, trees)
                variants = obj[2]['variants']
                self.assertEqual([v['targetRenderer'] for v in variants], [38, 39])
                for variant in variants:
                    self.assertTrue(variant['resourcesResolved'])
                    parent, = variant['kernels']
                    self.assertEqual(parent['globalKeywords'], ['GLOBAL'])
                    self.assertEqual(parent['localKeywords'], ['SKIN_NORMALS'])
                    self.assertEqual([v['first'] for v in parent['variantMap']], ['', 'SKIN_NORMALS'])
                    for pair in parent['variantMap']:
                        kernel = pair['second']
                        self.assertEqual(kernel['cbVariantIndices'], [7])
                        self.assertEqual(kernel['cbs'][0]['generatedName'], 'ResultBuffer')
                        self.assertEqual(kernel['threadGroupSize'], [64, 1, 1])
                        self.assertEqual(kernel['requirements'], -0x123456789ab)
                    self.assertEqual(variant['constantBuffers'][0]['params'][0]['colCount'], 4)
                le, doff, types, entries = unityfs._tables(data, {}, {72}, trees)
                _, start, size, ti = entries[0]
                reader = unityfs.Reader(data[doff + start:doff + start + size], le)
                unityfs._read(reader, types[ti][2])
                self.assertEqual(reader.p, size)


if __name__ == '__main__':
    unittest.main()
