import json,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class GuestAddressTests(unittest.TestCase):
 def test_production_reservation_handles_captured_fragmentation(self):
  with tempfile.TemporaryDirectory() as d:
   exe=str(Path(d)/'guest-address')
   build=subprocess.run(['xcrun','clang','-O1','-g','-Wl,-dead_strip','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'runtime'),'-I'+str(ROOT/'third_party/d2r_gfx/gfx/nvn/include'),str(ROOT/'tests/bench/guest_address_probe.c'),'-framework','CoreFoundation','-o',exe],capture_output=True,text=True)
   self.assertEqual(build.returncode,0,build.stderr)
   run=subprocess.run([exe],capture_output=True,text=True,timeout=10)
   self.assertEqual(run.returncode,0,run.stderr);self.assertTrue(all(json.loads(run.stdout).values()))
