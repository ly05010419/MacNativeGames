"""BAHS control metadata must not be reinterpreted as an entry table."""
import importlib.util,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
spec=importlib.util.spec_from_file_location('oot_corpus',Path(__file__).resolve().parents[1]/'scripts/ns2_shader_corpus.py');corpus=importlib.util.module_from_spec(spec);spec.loader.exec_module(corpus)
class CorpusTests(unittest.TestCase):
 def test_control_lengths_do_not_create_a_shader(self):
  data=bytearray(0xe00);data[:4]=b'BAHS';struct.pack_into('<II',data,0x18,0x400,0xa00)
  def control(at,size):
   data[at:at+8]=corpus.CTL_MAGIC+b'\xc9\0\0\0';struct.pack_into('<III',data,at+0x38,size,0,size);struct.pack_into('<I',data,at+0x44,size);data[at+0x56]=5
  control(0x100,0x100);control(0x400,16)
  struct.pack_into('<IIIi',data,0x40,0,0,0xe0,0x100-0x40)
  struct.pack_into('<IIIi',data,0x50,0x300,0,0xe0,0x400-0x50)
  # This unreferenced control makes the words at canonical_control+0x38
  # appear to be a third record in a scan which ignores control ownership.
  control(0x238,16)
  found=corpus.nintendo_shaders(bytes(data),'fixture')
  self.assertEqual(len(found),2)
  self.assertTrue(all('#BAHS@0x40' in r[4] or '#BAHS@0x50' in r[4]for r in found))
if __name__=='__main__':unittest.main()
