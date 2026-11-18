"""Legacy Unity stages use distinct blob indices and need their parsed pass's interface."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split
import unityfs


def shader(stage, locations=(), tag=0):
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = split.GFX_MAGIC
    struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    for i in locations: code[0x48 + i] = 1
    code[-1] = tag
    return stage, bytes(ctl), bytes(code)


def shader_pass(vertex, pixel):
    return {field: {'m_SubPrograms': [{'m_BlobIndex': i} for i in indices]}
            for field, indices in (('progVertex', vertex), ('progFragment', pixel))}


def shader_object(*passes):
    return {'m_ParsedForm': {'m_Name': 'fixture', 'm_SubShaders': [{'m_Passes': list(passes)}]}}


def key(program): return split.skey(*program[1:])


class UnityPassPairsTests(unittest.TestCase):
    def read(self, obj, programs):
        with mock.patch.object(split, 'COMMON', {}), \
             mock.patch.object(unityfs, 'is_serialized', return_value=True), \
             mock.patch.object(unityfs, 'serialized_objects', return_value=[(48, 1, obj)]), \
             mock.patch.object(unityfs, 'shader_programs', return_value=programs), \
             mock.patch.object(unityfs, 'subprogram', side_effect=lambda b: (0, 0, [], [], b, len(b))), \
             mock.patch.object(split, 'unity_compute', return_value=[]), \
             mock.patch.object(split, 'unity_text_glslc', return_value=[]):
            found = []
            pairs = split.unity_shaders(b'serialized fixture', '/fixture', found, {})
        return pairs, found

    def test_distinct_indices_and_platforms_keep_the_pass_interface(self):
        vertex = shader('vs')
        fragment = shader('ps', (1,))
        other_platform = shader('ps', (3,), tag=2)
        obj = shader_object(shader_pass([0], [8]))
        programs = {(19, 0): vertex, (19, 8): fragment, (20, 8): other_platform}
        self.assertEqual(split.unity_pass_pairs(obj, programs), {(key(vertex), key(fragment))})

    def test_cover_includes_every_input_without_crossing_passes(self):
        vertex, other_vertex = shader('vs'), shader('vs', tag=2)
        first, second, redundant = shader('ps', (0, 1)), shader('ps', (2,)), shader('ps', (0,))
        other_fragment = shader('ps', (7,))
        obj = shader_object(shader_pass([0], [1, 2, 3]), shader_pass([4], [5, 999]))
        programs = {(19, i): p for i, p in enumerate((vertex, first, second, redundant, other_vertex, other_fragment))}
        self.assertEqual(split.unity_pass_pairs(obj, programs), {
            (key(vertex), key(first)), (key(vertex), key(second)), (key(other_vertex), key(other_fragment))})

    def test_reader_returns_pairs_for_legacy_programs_only(self):
        vertex, fragment = shader('vs'), shader('ps', (1,))
        obj = shader_object(shader_pass([0], [8]))
        def decoded(program):
            _, ctl, code = program
            return struct.pack('<4i', 0, 0, len(ctl), len(code)) + ctl + code
        programs = [(19, 0, decoded(vertex)), (19, 8, decoded(fragment))]
        pairs, found = self.read(obj, programs)
        self.assertEqual(pairs, {(key(vertex), key(fragment))})
        self.assertEqual(split.var_pairs(found), set())  # The two blob indices alone cannot pair them.
        self.assertEqual([p[1:] for p in found], [vertex, fragment])

    def test_combined_program_keeps_its_existing_pair(self):
        vertex, fragment = shader('vs'), shader('ps', (1,))
        header = [0] * 6 + [-1] * 6 + [0] * 12
        data = b''
        for stage, ctl, code in (vertex, fragment):
            i = split.NV_STAGES.index(stage)
            header[6 + i], header[12 + i], header[18 + i] = len(data), len(ctl), len(code)
            data += ctl + code
        payload = struct.pack('<24i', *header) + data
        pairs, found = self.read(shader_object(shader_pass([0], [8])), [(19, 0, payload)])
        self.assertEqual(pairs, set())
        self.assertEqual(split.var_pairs(found), {(key(vertex), key(fragment))})

    def test_scanner_routes_legacy_cover_to_supplemental_table(self):
        vertex, fragment = shader('vs'), shader('ps', (1,))
        pair = key(vertex), key(fragment)
        def read(data, label, out, trees):
            out.extend([(label + '#var19.0', *vertex), (label + '#var19.8', *fragment)])
            return {pair}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'resources.assets'
            path.write_bytes(bytes(64))
            with mock.patch.object(unityfs, 'is_serialized', return_value=True), \
                 mock.patch.object(split, 'unity_shaders', side_effect=read):
                _, _, pairs, _ = split.scan_file_in((str(path), 'resources.assets'))
        self.assertEqual(pairs, {(*pair, 'prog')})


if __name__ == '__main__': unittest.main()
