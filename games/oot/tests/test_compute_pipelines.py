import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from prepare_compute_pipelines import compute_rows

class ComputePlanTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.seed=Path(self.temp.name);self.files={}
    def shader(self,stage='cs',variant='b'*12,source='kernel void main0() {}'):
        key='a'*64+'-'+variant+'_b'
        values={key+'.json':json.dumps(dict(chain='fixture',key=key,stage=stage,entry='main0',msl_entry_kind='kernel' if stage=='cs' else 'mesh')),key+'.metal':source}
        for name,value in values.items():
            data=value.encode();(self.seed/name).write_bytes(data);self.files[name]=hashlib.sha256(data).hexdigest()
        (self.seed/'seed-manifest.json').write_text(json.dumps(dict(chain='fixture',files=self.files)))
        return key
    def test_all_compute_variants_not_just_played_base_key(self):
        a=self.shader();b=self.shader(variant='c'*12);self.shader(stage='ms',variant='d'*12)
        self.assertEqual(compute_rows(self.seed),['C\t'+a,'C\t'+b])
    def test_damaged_source_cannot_enter_archive(self):
        key=self.shader();(self.seed/(key+'.metal')).write_text('changed')
        with self.assertRaisesRegex(ValueError,'damaged compute source'):compute_rows(self.seed)
    def test_unbound_constants_cannot_silently_omit_pipeline(self):
        self.shader(source='constant int n [[function_constant(0)]];')
        with self.assertRaisesRegex(ValueError,'explicit specialization'):compute_rows(self.seed)
    def test_wrong_chain_rejected(self):
        key=self.shader();p=self.seed/(key+'.json');data=json.loads(p.read_text());data['chain']='other';p.write_text(json.dumps(data))
        self.files[p.name]=hashlib.sha256(p.read_bytes()).hexdigest()
        (self.seed/'seed-manifest.json').write_text(json.dumps(dict(chain='fixture',files=self.files)))
        with self.assertRaisesRegex(ValueError,'invalid compute reflection'):compute_rows(self.seed)
