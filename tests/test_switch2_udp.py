import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Switch2UDPTests(unittest.TestCase):
    def test_wire_protocol_rejects_invalid_input_and_maps_every_button(self):
        with tempfile.TemporaryDirectory() as d:
            binary=str(Path(d)/'probe')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/switch2_udp_probe.c'),'-lm','-o',binary],check=True,capture_output=True)
            result=subprocess.run([binary],check=True,capture_output=True,text=True)
            self.assertIn('S2UDP PASS',result.stdout)
