"""The UI layouts for a canvas taller than 16:9 (games/totk/scripts/tall_layout.py): the edit table and what apply() writes, on a
synthetic archive (the game's own layouts are not in the repository)."""
import importlib.util
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('tall_layout', ROOT/'games/totk/scripts/tall_layout.py')
tall = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tall)
K = (16 / 10) / (16 / 9)   # a 16:10 canvas


def pane(name, y=0.0, sy=1.0, h=1080.0):
    out = bytearray(84)
    out[0:4] = b'pan1'
    struct.pack_into('<I', out, 4, 84)
    out[12:12 + len(name)] = name.encode()
    struct.pack_into('<f', out, 48, y)
    struct.pack_into('<ff', out, 68, 1.0, sy)
    struct.pack_into('<ff', out, 76, 1920.0, h)
    return bytes(out)


def archive(layouts):
    """A bytearray holding one BFLYT per layout ({name: [pane bytes]}) and the members map apply() takes."""
    blob, members = bytearray(), {}
    for name, panes in layouts.items():
        head = bytearray(0x14)
        head[0:4] = b'FLYT'
        head[4:6] = b'\xff\xfe'
        struct.pack_into('<H', head, 6, 0x14)
        struct.pack_into('<H', head, 0x10, len(panes))
        data = bytes(head) + b''.join(panes)
        members[f'blyt/{name}.bflyt'] = (len(blob), len(blob) + len(data))
        blob += data
    return blob, members


def field(blob, members, layout, pane, name):
    at = tall.panes(blob, *members[f'blyt/{layout}.bflyt'])[pane]
    return struct.unpack_from('<f', blob, at + tall.FIELD[name])[0]


class EditTable(unittest.TestCase):
    def test_a_canvas_of_16_9_changes_nothing(self):
        for layout, name, kind, shipped, value in tall.edits(1.0):
            if kind in ('sx', 'sy'): self.assertAlmostEqual(value, 1.0, msg=(layout, name))
            else: self.assertAlmostEqual(value, 0.0, msg=(layout, name))

    def test_the_distances_at_16_10(self):
        rows = {(l, n, f): v for l, n, f, _, v in tall.edits(K)}
        self.assertAlmostEqual(rows['LoadingFade_00', 'N_Tips_00', 'y'], -60.0)        # a pane that hangs from the bottom
        self.assertAlmostEqual(rows['AppAlbum_00', 'N_PicLoad_01', 'y'], -54.0)        # a pane under a shifted root
        self.assertAlmostEqual(rows['AppMap_00', 'N_BlurInOut_00', 'sy'], 1 / K)       # a backdrop stretched back
        self.assertAlmostEqual(rows['AppAlbum_00', 'N_FadeInOut_00', 'sy'], K)         # a widget that keeps its shape

    def test_the_layout_lists_are_distinct(self):
        lists = [set(tall.KEEP_ROOT), set(tall.ROOT_UP), set(tall.ROOT_DOWN)]
        self.assertEqual(sum(len(l) for l in lists), len(set().union(*lists)))
        self.assertEqual(len(tall.ROOT_UP), len(set(tall.ROOT_UP)))


class Apply(unittest.TestCase):
    def setUp(self):
        self.saved = {n: getattr(tall, n) for n in ('KEEP_ROOT', 'ROOT_UP', 'ROOT_DOWN', 'CENTERED', 'LIST_EXTENSION', 'edits', 'anim_edits')}
        tall.KEEP_ROOT, tall.ROOT_UP, tall.ROOT_DOWN, tall.CENTERED, tall.LIST_EXTENSION = {'Keep'}, ['Up'], ['Down'], (), []
        tall.edits = lambda k: [('Up', 'N_A', 'y', 10.0, -60.0), ('Mid', 'N_B', 'sy', 1.0, 1 / k)]
        tall.anim_edits = lambda k: []
        self.blob, self.members = archive({name: [pane('RootPane'), pane('N_A', y=10.0), pane('N_B')]
                                           for name in ('Up', 'Down', 'Mid', 'Keep')})

    def tearDown(self):
        for name, value in self.saved.items(): setattr(tall, name, value)

    def test_roots_edges_and_panes(self):
        changed = tall.apply(self.blob, self.members, 2560, 1600)
        get = lambda layout, name, f: field(self.blob, self.members, layout, name, f)
        for layout in ('Up', 'Down', 'Mid'): self.assertAlmostEqual(get(layout, 'RootPane', 'sy'), K, places=5)
        self.assertAlmostEqual(get('Keep', 'RootPane', 'sy'), 1.0)                      # full-screen layouts keep their size
        self.assertAlmostEqual(get('Up', 'RootPane', 'y'), 54.0, places=3)             # the layout moves out with the top edge
        self.assertAlmostEqual(get('Down', 'RootPane', 'y'), -54.0, places=3)
        self.assertAlmostEqual(get('Mid', 'RootPane', 'y'), 0.0)
        self.assertAlmostEqual(get('Up', 'N_A', 'y'), -50.0, places=3)                 # shipped 10 plus the distance
        self.assertAlmostEqual(get('Mid', 'N_B', 'sy'), 1 / K, places=5)
        self.assertEqual(sorted(changed), ['blyt/Down.bflyt', 'blyt/Mid.bflyt', 'blyt/Up.bflyt'])

    def test_hud_center_leaves_the_edges(self):
        tall.apply(self.blob, self.members, 2560, 1600, hud_corner=False)
        self.assertAlmostEqual(field(self.blob, self.members, 'Up', 'RootPane', 'y'), 0.0)
        self.assertAlmostEqual(field(self.blob, self.members, 'Up', 'N_A', 'y'), 10.0)
        self.assertAlmostEqual(field(self.blob, self.members, 'Mid', 'N_B', 'sy'), 1 / K, places=5)

    def test_a_pane_that_is_not_as_shipped_stops_the_script(self):
        blob, members = archive({name: [pane('RootPane'), pane('N_A', y=11.0), pane('N_B')] for name in ('Up', 'Down', 'Mid', 'Keep')})
        with self.assertRaises(SystemExit): tall.apply(blob, members, 2560, 1600)

    def test_a_missing_layout_stops_the_script(self):
        blob, members = archive({'Up': [pane('RootPane'), pane('N_A', y=10.0)]})
        with self.assertRaises(SystemExit): tall.apply(blob, members, 2560, 1600)

    def test_only_for_taller_canvases(self):
        with self.assertRaises(AssertionError): tall.apply(self.blob, self.members, 3440, 1440)


if __name__ == '__main__':
    unittest.main()
