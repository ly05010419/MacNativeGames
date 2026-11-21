"""The layouts unityfs.py writes out for stripped Unity files: one per supported type hash."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs

# The Shader (class 48) and ComputeShader (class 72) hashes of the Unity versions read without a bundle's tree.
# An entry dropped from KNOWN_TREES stops the split on every stripped player of that version.
SHADER_HASHES = {'5.5': '4496e93f2179252104401c8dda0a1751', '2017.4': '266d53113fa30d2b858f2768f92eaa14', '2018.3': '8db760073e1392061e4a36995f0735af',
                 '2019.4': 'ed2a06ce7fb8203f6efb5ab5e0ce20d5', '2020.3': 'a30e3dcf6c6d25153d6f64e5e41caf4e',
                 '2021.2': 'd8236c9bd87d85edb203acf352348124', '2022.3': 'ea424a5fc1e48db62144df956f9e1005',
                 '6000.0': '2ec7223dce3af4232b14ae1441bd7999'}
COMPUTE_HASHES = {'2017.4': '20291e020d10074c35d8286f5191593f', '2019.4': '1a95529ac2eb1a97cd9d2a4356bd722f',
                  '2020.3': 'd0171b849bb064b2424893e7d4809be5', '2021.3': 'abd9135b8ce83d043fef4e9ec7f53366', '2022.3': 'bfdf63ab0ce84d5a2c765ee560d90fe7',
                  '6000.3': '43a0948a3f403068c0b31bff90ba0c9a'}


class KnownTreeTests(unittest.TestCase):
    def test_every_supported_hash_has_a_layout(self):
        for cid, hashes in ((unityfs.CLASS_SHADER, SHADER_HASHES), (unityfs.CLASS_COMPUTE_SHADER, COMPUTE_HASHES)):
            for version, h in hashes.items():
                with self.subTest(cid=cid, version=version):
                    self.assertIn((cid, bytes.fromhex(h)), unityfs.KNOWN_TREES)

    def test_every_layout_builds_its_root(self):
        for (cid, h), (root, _) in unityfs.KNOWN_TREES.items():
            with self.subTest(cid=cid, hash=h.hex()):
                self.assertEqual(unityfs.known_tree(cid, h).type, root)


if __name__ == '__main__':
    unittest.main()
