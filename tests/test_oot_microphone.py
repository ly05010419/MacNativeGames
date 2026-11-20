"""Ocarina of Time on the Mac: the microphone as the game's virtual input device, so its "Ocarina Mic Input" option can play
the ocarina from it (after seedds/nxnative c25d4011, PR #1822, MIT; user 2026-11-20: "只做 mac"). Off unless the settings
panel's switch (microphone.txt) or OOT_MIC=1 turns it on; the iPhone / iPad keep no input. probe-audio checks the streams
themselves, off and on (fed a 440 Hz tone through OOT_MIC_FILE); these check what holds it together."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OOT = ROOT/'games/oot'
AUDIO_OUT = (OOT/'runtime/ns2_audio_out.c').read_text(encoding='utf-8')
CORE = (OOT/'runtime/audio_input.c').read_text(encoding='utf-8')
MAC = (OOT/'runtime/audio_input_mac.m').read_text(encoding='utf-8')
HEADER = (OOT/'runtime/audio_input.h').read_text(encoding='utf-8')
LAUNCHER = (OOT/'app/launcher.m').read_text(encoding='utf-8')
SETTINGS = (OOT/'runtime/settings_game.h').read_text(encoding='utf-8')
MAKEFILE = (OOT/'Makefile').read_text(encoding='utf-8')
BUILD = (OOT/'build.py').read_text(encoding='utf-8')
MAIN = Path('/Volumes/开发/MacNativeGames-artifacts/scratch/build/oot/oot/rewritten/oot/main.patched.bin')


def hooked():
    return set(re.findall(r'\{"(_ZN2nn5audio\w+)", (?:input_\w+|list_virtual_input|raw_input_device)\}', AUDIO_OUT))


class Microphone(unittest.TestCase):
    def test_off_unless_switched_on(self):
        self.assertIn('getenv("OOT_MIC")', CORE)
        self.assertIn('enabled && !strcmp(enabled, "1")', CORE)
        self.assertIn('@"microphone.txt"', LAUNCHER)
        self.assertIn('settings_read_int("microphone.txt",&v)&&v==1', SETTINGS)

    def test_the_mac_only(self):
        self.assertTrue(MAC.split('\n#if TARGET_OS_OSX\n', 1)[0].count('#include') == 1)   # the whole file is the Mac's
        self.assertTrue(MAC.rstrip().endswith('#endif'))
        phone = HEADER[HEADER.index('#else'):HEADER.index('#endif')]
        self.assertIn('device_available(void) { return 0; }', phone)   # the iPhone / iPad list nothing
        # the switch heads the 陶笛 page (user 2026-11-20), the Mac's only (SETTINGS_HAS_OCARINA under !TARGET_OS_IPHONE)
        ocarina = SETTINGS[SETTINGS.index('static void settings_oot_ocarina_rows('):]
        self.assertTrue(ocarina.split('\n')[1].strip().startswith('[rows addObject:settings_oot_mic_row()];'))
        self.assertEqual(SETTINGS.count('[rows addObject:settings_oot_mic_row()];'), 1)   # not on the Input page too
        self.assertIn('#if !TARGET_OS_IPHONE\n#define SETTINGS_HAS_OCARINA 1', SETTINGS)
        self.assertIn('rows[i].page=SETTINGS_PAGE_OCARINA', SETTINGS)

    def test_permission_is_asked_before_the_game_and_described(self):
        setting = LAUNCHER[LAUNCHER.index('static void microphone_setting(NSString *data) {'):]
        setting = setting[:setting.index('\n}\n')]
        self.assertIn('AVAudioApplicationRecordPermissionUndetermined', setting)
        self.assertIn('requestRecordPermissionWithCompletionHandler', setting)
        self.assertIn('getenv("OOT_MIC_FILE")', setting)   # the test feed never asks
        self.assertIn('microphone_setting(data);', LAUNCHER)
        self.assertIn("info['NSMicrophoneUsageDescription']=MICROPHONE_USAGE", BUILD)
        self.assertNotIn('requestRecordPermission', CORE + MAC)   # the game itself never prompts

    def test_built_and_linked(self):
        self.assertIn('runtime/audio_input.c runtime/audio_input_mac.m', MAKEFILE)
        self.assertIn('-framework CoreAudio -framework AVFAudio', MAKEFILE)

    def test_every_input_stream_call_the_game_imports_is_answered(self):
        if not MAIN.is_file():
            self.skipTest(f'{MAIN} is not on this Mac')
        names = subprocess.run(['strings', '-n', '20', str(MAIN)], capture_output=True, text=True, check=True).stdout.split()
        imported = {n for n in names if n.startswith('_ZN2nn5audio') and ('InputStream' in n or 'InputDevice' in n)}
        self.assertEqual(len(imported), 11)
        self.assertEqual(imported - hooked(), set())


if __name__ == '__main__':
    unittest.main()
