"""games/botw/scripts/location_markers.py: the BYML and MSBT readers on synthetic data (no game data)."""
import importlib.util, pathlib, struct, unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('location_markers', ROOT / 'games/botw/scripts/location_markers.py')
lm = importlib.util.module_from_spec(spec); spec.loader.exec_module(lm)

def strtab(strings):
    offs = []; blob = b''
    for s in strings: offs.append(len(blob)); blob += s.encode() + b'\0'
    offs.append(len(blob)); head = 4 + 4 * (len(strings) + 1)
    return struct.pack('<I', 0xc2 | len(strings) << 8) + b''.join(struct.pack('<I', head + o) for o in offs) + blob

def byml_doc():
    """{"LocationMarker": [{"MessageID": "Dungeon038", "Translate": {"X": -672.46, "Y": 186.09, "Z": 1513.63}}]}"""
    hashes = strtab(['LocationMarker', 'MessageID', 'Translate', 'X', 'Y', 'Z'])   # sorted, as the game writes them
    strings = strtab(['Dungeon038'])
    header = 16; hash_off = header; str_off = hash_off + len(hashes); root_off = str_off + len(strings)
    # root dict (1 entry) -> array (1 element) -> dict (2 entries) -> dict (3 floats)
    root = struct.pack('<I', 0xc1 | 1 << 8); arr_off = root_off + 4 + 8
    root += struct.pack('<I', 0 | 0xc0 << 24) + struct.pack('<I', arr_off)
    arr = struct.pack('<I', 0xc0 | 1 << 8) + bytes([0xc1, 0, 0, 0]); d1_off = arr_off + len(arr) + 4
    arr += struct.pack('<I', d1_off)
    d1 = struct.pack('<I', 0xc1 | 2 << 8); d2_off = d1_off + 4 + 16
    d1 += struct.pack('<I', 1 | 0xa0 << 24) + struct.pack('<I', 0) + struct.pack('<I', 2 | 0xc1 << 24) + struct.pack('<I', d2_off)
    d2 = struct.pack('<I', 0xc1 | 3 << 8)
    for k, v in ((3, -672.46), (4, 186.09), (5, 1513.63)): d2 += struct.pack('<I', k | 0xd2 << 24) + struct.pack('<f', v)
    body = root + arr + d1 + d2
    return b'YB' + struct.pack('<HIII', 2, hash_off, str_off, root_off) + hashes + strings + body

def msbt_doc(labels):
    """One group, labels in order, texts UTF-16LE with a terminator."""
    lbl = struct.pack('<I', 1) + struct.pack('<II', len(labels), 12)
    for i, (name, _) in enumerate(labels): lbl += bytes([len(name)]) + name.encode() + struct.pack('<I', i)
    txt = struct.pack('<I', len(labels)); offs = []; body = b''
    for _, text in labels: offs.append(4 + 4 * len(labels) + len(body)); body += (text + '\0').encode('utf-16-le')
    txt += b''.join(struct.pack('<I', o) for o in offs) + body
    def section(name, data): return name + struct.pack('<I', len(data)) + b'\0' * 8 + data + b'\xab' * ((16 - len(data) % 16) % 16)
    return b'MsgStdBn' + b'\xff\xfe' + b'\0' * 4 + struct.pack('<H', 2) + b'\0' * 16 + section(b'LBL1', lbl) + section(b'TXT2', txt)

class LocationMarkers(unittest.TestCase):
    def test_byml_reader(self):
        doc = lm.byml(byml_doc())
        self.assertEqual(doc['LocationMarker'][0]['MessageID'], 'Dungeon038')
        t = doc['LocationMarker'][0]['Translate']
        self.assertAlmostEqual(t['X'], -672.46, places=2); self.assertAlmostEqual(t['Z'], 1513.63, places=2)
    def test_msbt_reader(self):
        names = lm.msbt_labels(msbt_doc([('Dungeon038', '玛·奥努神庙'), ('StartPoint', '复苏神庙')]))
        self.assertEqual(names, {'Dungeon038': '玛·奥努神庙', 'StartPoint': '复苏神庙'})

if __name__ == '__main__': unittest.main()
