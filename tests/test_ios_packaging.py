"""games/totk/ios/tools/build_ipa.py: what an iOS build leaves out of the way and copies (2026-10-23)."""
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]/'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import build_ipa  # noqa: E402
import install_device  # noqa: E402


class Discard(unittest.TestCase):
    def test_a_folder_is_moved_aside_at_once_and_deleted_later(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); old = root/'TOTK.app'
            (old/'GameData/shader-seed').mkdir(parents=True)
            for i in range(50):
                (old/f'GameData/shader-seed/{i}.metallib').write_bytes(b'x')
            build_ipa.discard(old)
            self.assertFalse(old.exists())   # the path is free for the new build right away
            for _ in range(100):
                if not any(root.iterdir()):
                    break
                time.sleep(0.05)
            self.assertEqual(list(root.iterdir()), [])   # and the moved folder is gone once the background delete ran

    def test_a_missing_path_is_fine(self):
        with tempfile.TemporaryDirectory() as d:
            build_ipa.discard(Path(d)/'none')


class SeedBeside(unittest.TestCase):
    """--ios-game-data documents leaves the startup shaders beside the output; the README installs the .ipa, and the
    seed was pushed only for an .app folder, so the game said 'No shaders' after a successful install."""
    def layout(self, root, output):
        (root/'Tears of the Kingdom-shader-seed').mkdir()
        (root/'Tears of the Kingdom-shader-seed/fingerprint.txt').write_text('f' * 64)
        app = root/'signed/Payload/TOTK.app'; (app/'GameData').mkdir(parents=True)
        return root/output, app

    def test_an_ipa_gets_its_seed(self):
        with tempfile.TemporaryDirectory() as d:
            ipa, app = self.layout(Path(d), 'Tears of the Kingdom.ipa'); ipa.write_bytes(b'PK')
            self.assertEqual(install_device.seed_beside(ipa, app), Path(d)/'Tears of the Kingdom-shader-seed')

    def test_an_app_folder_gets_its_seed(self):
        with tempfile.TemporaryDirectory() as d:
            out, app = self.layout(Path(d), 'Tears of the Kingdom.app'); out.mkdir()
            self.assertEqual(install_device.seed_beside(out, out), Path(d)/'Tears of the Kingdom-shader-seed')

    def test_an_app_that_carries_its_seed_gets_none(self):
        with tempfile.TemporaryDirectory() as d:
            ipa, app = self.layout(Path(d), 'Tears of the Kingdom.ipa'); (app/'GameData/shader-seed').mkdir()
            self.assertIsNone(install_device.seed_beside(ipa, app))


def read_pack(parts):
    """ios/Sources/main.m seed_unpack, in Python: the files of a pack stream cut into parts."""
    data = b''.join(p.read_bytes() for p in parts); assert data[:9] == b'TOTKPACK1'; at, out = 9, {}
    while True:
        nl = int.from_bytes(data[at:at + 2], 'little'); at += 2
        if not nl: return out, at == len(data)
        name = data[at:at + nl].decode(); at += nl
        size = int.from_bytes(data[at:at + 8], 'little'); at += 8
        out[name] = data[at:at + size]; at += size


class ShaderPack(unittest.TestCase):
    def test_files_come_back_whole_across_part_boundaries(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); seed = root/'seed'; seed.mkdir(); parts = []
            files = {'a.metal': b'x' * 1000, 'b.json': b'{}', 'empty.variants': b'', 'c.metallib': bytes(range(256)) * 3}
            for n, b in files.items():
                (seed/n).write_bytes(b)
            install_device.write_pack(seed, sorted(files), root, 'shader-seed-x', 97, parts.append)
            self.assertGreater(len(parts), 10)   # cut in the middle of names, sizes and contents
            self.assertTrue(all(p.stat().st_size <= 97 for p in parts))
            got, exact = read_pack(parts)
            self.assertEqual(got, files)
            self.assertTrue(exact)   # nothing after the end mark



class FullHD(unittest.TestCase):
    """Settings › Full HD (docked): off by default on an iPhone, on by default on an iPad (DefaultValuePad, user
    2026-11-01), for both games. Tears of the Kingdom renders its scene at 1600x900 (1.4.3 renders 960x540 on the phone
    otherwise); Breath of the Wild's pane adds its docked scene row (build_ipa.py botw_pane): the Switch's 1600x900 by
    default, or HD 1920x1080."""
    def test_switch_off_by_default_and_docked(self):
        import plistlib, re
        ios = TOOLS.parent
        specs = plistlib.loads((ios/'Settings.bundle/Root.plist').read_bytes())['PreferenceSpecifiers']
        switch = [s for s in specs if s.get('Key') == 'TOTK_DOCKED']
        self.assertEqual(len(switch), 1)
        self.assertIs(switch[0]['DefaultValue'], False)
        self.assertIs(switch[0]['DefaultValuePad'], True)
        main = (ios/'Sources/main.m').read_text()
        # The switch is the pane's preset layer (main.m's apply_settings): it hands its own drawable and scene over
        # through settings_map.c's table, which is where the docked values live.
        self.assertIn('totk_preset_env_build([d boolForKey:@"TOTK_DOCKED"], &prenv)', main)
        for line in ('setenv("TOTK_IOS_DRAWABLE_H", prenv.drawable, 0)',
                     # docked: the pane's TOTK_DOCKED_SCENE row picks 1600x900 or 1920x1080 (settings_map.c totk_docked_scene)
                     'if (prenv.scene)', 'setenv("TOTK_RENDER_RES", totk_docked_scene([d stringForKey:@"TOTK_DOCKED_SCENE"].UTF8String), 0);'):
            self.assertIn(line, main)
        preset = re.search(r'int totk_preset_env_build\(int docked.*?\n}', (ios/'Sources/settings_map.c').read_text(), re.S)
        self.assertIsNotNone(preset)
        table = preset.group(0) if preset else ''
        for value in ('"docked"', '"1080"', '"1600x900"'):
            self.assertIn(value, table)
        # The presets' picture is handed over once, when the game starts (apply_display_settings, after the start
        # screen's pick), and not while the custom layer is on (after PR #433).
        body = re.search(r'static void apply_display_settings\(void\) \{.*?\n}', main, re.S).group(0)
        self.assertIn('if (prenv.drawable) setenv("TOTK_IOS_DRAWABLE_H", prenv.drawable, 0);', body)
        self.assertEqual(main.count('totk_preset_env_build('), 1, 'the presets\' picture goes in one place')
        # Both games have the switch (Breath of the Wild's docked scene is its own row, TOTK_BOTW_SCENE), so no
        # game-specific exception is left in the helper. The only other operation-mode site is visionOS's default.
        self.assertNotIn('#ifndef GAME_BOTW', main)
        vision = 'setenv("TOTK_OPERATION_MODE","docked",1);'
        self.assertIn(vision, main)
        self.assertEqual(main.replace(vision, '').count('setenv("TOTK_OPERATION_MODE",'), 1,
                         'the other operation-mode site is the helper')
        build = (TOOLS/'build_ipa.py').read_text()
        self.assertIn("'      - path: Gen/Settings.bundle'", build)
        self.assertIn("s.get('Key')=='TOTK_DOCKED'", build)
        self.assertIn("specs[at-1]['FooterText']", build)
        self.assertNotIn('del specs[at-1:at+1]', build)

    def test_seed_fingerprint_names_the_gpu(self):
        # The pipelines' index (.tsv) is the same for every GPU: the fingerprint that decides whether the app's seed is
        # copied again must take the pipelines-<arch>-* files themselves, or an iPhone build keeps an iPad's pipelines.
        build = (TOOLS/'build_ipa.py').read_text()
        self.assertIn("for p in sorted(seed.glob('pipelines-*')): fp.update(p.name.encode()", build)

    def test_ipad_default_is_stored_and_used(self):
        main = (TOOLS.parent/'Sources/main.m').read_text()
        self.assertIn('item[@"DefaultValuePad"] && UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad', main)
        self.assertIn('for (NSString *key in pad) if (!stored[key]) [d setObject:pad[key] forKey:key];', main)

    def test_ios_export_leaves_the_mac_only_vr_hook_out(self):
        """The iOS export runs the 1.4.3 patches on the Mac; the VR head-look hook branches into the Mac runtime, so it
        must not be installed there (2026-11-03: the iPad jumped to a Mac address, "Invalid Page")."""
        src = (TOOLS.parents[1]/'runtime/game_patches_v143.c').read_text()
        self.assertIn('if (!getenv("TOTK_EXPORT_IOS")) vr_headlook_install(m);', src)
        self.assertEqual(src.count('vr_headlook_install(m);'), 1)

    def test_mac_pipelines_are_made_again_for_another_runtime(self):
        """The cached Mac pipelines (build/ios-pipelines) are this runtime's descriptions: their stamp holds the runtime
        binary as well as the shaders and lists (2026-11-03: narrowed vertex formats were never seen otherwise)."""
        ipa = (TOOLS/'build_ipa.py').read_text()
        self.assertIn("+host.read_bytes()).hexdigest()", ipa)
        self.assertLess(ipa.index("host=tree/"), ipa.index("+host.read_bytes()).hexdigest()"))



class PicturePane(unittest.TestCase):
    """The Settings app keeps only the picture profile and MetalFX for the two Zelda games (user 2026-11-17: "dentro do
    jogo é o que vale"; spec "App Ajustes"); the picture rows of before go, so the pane and the game cannot disagree.
    Bananza and Pokopia keep their own rows (Full HD / output size, frame rate)."""
    PICTURE = {'TOTK_DOCKED', 'TOTK_DOCKED_SCENE', 'TOTK_BOTW_SCENE', 'TOTK_PHONE_FPS', 'TOTK_CUSTOM_PICTURE',
               'TOTK_IOS_PICTURE_MODE', 'TOTK_RENDER_RES', 'TOTK_FPS_CAP', 'TOTK_MFX_UPSCALE', 'TOTK_TAA', 'TOTK_ASPECT',
               'TOTK_OUTPUT_RES', 'TOTK_SWITCH_SIZE'}

    def root(self):
        import plistlib
        return plistlib.loads((TOOLS.parent/'Settings.bundle/Root.plist').read_bytes())

    def panes(self):
        return {'totk': build_ipa.pane_for(self.root(), 'totk'), 'botw': build_ipa.botw_pane(self.root()),
                'pokopia': build_ipa.pane_for(self.root(), 'ns2'), 'bananza': build_ipa.bananza_pane(self.root())}

    def keys(self, pane):
        return [s.get('Key') for s in pane['PreferenceSpecifiers'] if s.get('Key')]

    def test_zelda_panes_have_only_the_profile_and_metalfx(self):
        for game in ('totk', 'botw'):
            with self.subTest(game=game):
                keys = self.keys(self.panes()[game])
                self.assertIn('TOTK_PICTURE_PROFILE', keys)
                self.assertIn('TOTK_METALFX', keys)
                self.assertEqual(self.PICTURE & set(keys), set(), 'no picture row of before')

    def test_values_are_the_models_keys(self):
        # The pane and the app store the same words (picture_profile.c pp_profile_key / pp_metalfx_key).
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import test_picture_profile
        profiles, metalfx = test_picture_profile.keys(self)
        rows = {s['Key']: s for s in self.root()['PreferenceSpecifiers'] if s.get('Key')}
        self.assertEqual(rows['TOTK_PICTURE_PROFILE']['Values'], profiles)
        self.assertEqual(rows['TOTK_METALFX']['Values'], metalfx)
        self.assertEqual(rows['TOTK_PICTURE_PROFILE']['DefaultValue'], 'performance', 'Desempenho on a new install')
        self.assertEqual(rows['TOTK_METALFX']['DefaultValue'], 'off', 'the game\'s own upscale unless the player turns it on')
        for key in ('TOTK_PICTURE_PROFILE', 'TOTK_METALFX'):
            self.assertEqual(len(rows[key]['Titles']), len(rows[key]['Values']), key)
            self.assertNotIn('DefaultValuePad', rows[key], 'the same default on iPad and iPhone')

    def test_ns2_panes_keep_their_rows(self):
        panes = self.panes()
        for game, own in (('pokopia', 'TOTK_DOCKED'), ('bananza', 'TOTK_OUTPUT_RES')):
            with self.subTest(game=game):
                keys = self.keys(panes[game])
                self.assertIn(own, keys)
                self.assertIn('TOTK_PHONE_FPS', keys)
                self.assertNotIn('TOTK_PICTURE_PROFILE', keys)
                self.assertNotIn('TOTK_METALFX', keys)

    def test_no_empty_group(self):
        for game, pane in self.panes().items():
            specs = pane['PreferenceSpecifiers']
            for i, spec in enumerate(specs):
                if spec.get('Type') == 'PSGroupSpecifier':
                    with self.subTest(game=game, group=spec.get('Title')):
                        self.assertLess(i + 1, len(specs))
                        self.assertNotEqual(specs[i + 1].get('Type'), 'PSGroupSpecifier')

    def test_export_scene_is_a_profile_size(self):
        # build_ipa.py embeds one scene size so the scene patch exists in the exported code; it must be a size the
        # profiles can ask for.
        import re
        build = (TOOLS/'build_ipa.py').read_text()
        size = re.search(r"TOTK_RENDER_RES='(\d+x\d+)'", build).group(1)
        self.assertIn(f'{{"{size}", ', (TOOLS.parent/'Sources/picture_profile.c').read_text())


class Profiles(unittest.TestCase):
    """The picture profiles reach the runtime when the game starts (spec "Restrições"): main.m builds the environment
    from the saved profile, MetalFX and custom combination (picture_settings.m), after a one-time migration of the old
    rows; Tears of the Kingdom's custom layer is gone. Bananza and Pokopia keep their presets."""
    def setUp(self):
        import re
        self.main = (TOOLS.parent/'Sources/main.m').read_text()
        self.display = re.search(r'static void apply_display_settings\(void\) \{.*?\n}', self.main, re.S).group(0)
        self.launch = re.search(r'static void apply_settings\(void\) \{.*?\n}', self.main, re.S).group(0)

    def test_picture_settings_interface(self):
        header = (TOOLS.parent/'Sources/picture_settings.h').read_text()
        for decl in ('pp_picture totk_current_picture(pp_profile *profile, pp_metalfx *mfx);', 'BOOL totk_has_custom_picture(void);',
                     'void totk_choose_profile(pp_profile p);', 'void totk_save_custom_picture(const pp_picture *p);',
                     'void totk_choose_metalfx(pp_metalfx m);', 'void totk_migrate_picture_rows(void);'):
            self.assertIn(decl, header)

    def test_the_profile_drives_the_runtime_at_game_start(self):
        zelda = self.display[self.display.index('#ifndef GAME_NS2'):]
        self.assertIn('pp_picture pic = totk_current_picture(&profile, &mfx);', zelda)
        self.assertIn('pp_env_build(&pic, mfx, PP_GAME_BOTW, &env);', zelda)
        self.assertIn('pp_env_build(&pic, mfx, PP_GAME_TOTK, &env);', zelda)
        # overwrite 0: a devicectl value still wins; NULL leaves a variable alone
        self.assertIn('if (env.v[i].value) setenv(env.v[i].name, env.v[i].value, 0);', zelda)

    def test_the_profiles_30_fps_reaches_a_runtime_that_already_read_the_hold(self):
        # iPad A16 2026-11-18: Economy ran at 60. The touch controls draw their 60/30 button when the view loads, before
        # Play, and the runtime keeps the hold it read then (hle_nvn.c phone_holds_30): TOTK_PHONE_FPS set at Play
        # came too late. The hold is set again from the environment (a devicectl value still wins).
        zelda = self.display[self.display.index('#ifndef GAME_NS2'):self.display.index('totk_preset_env prenv;')]
        sync = 'runtime_phone_fps_set(phone_fps && atoi(phone_fps) == 30 ? 30 : 60);'
        self.assertIn(sync, zelda)
        self.assertLess(zelda.index('setenv(env.v[i].name, env.v[i].value, 0);'), zelda.index(sync))

    def test_migration_runs_once_before_the_rows_are_handed_over(self):
        self.assertLess(self.launch.index('totk_migrate_picture_rows();'), self.launch.index('for (NSString *key in defaults)'))
        settings = (TOOLS.parent/'Sources/picture_settings.m').read_text()
        self.assertIn('TOTK_PICTURE_MIGRATED', settings)
        self.assertIn('pp_migrate(&old, ', settings)
        self.assertIn('persistentDomainForName', settings, 'only what the player\'s app stored counts, not the pane defaults')

    def test_tears_of_the_kingdoms_custom_layer_is_gone(self):
        self.assertNotIn('TOTK_CUSTOM_PICTURE', self.main, 'only the migration (picture_settings.m) reads the old switch')
        self.assertNotIn('totk_key_layer_of', self.main)
        self.assertNotIn('totk_picture_env_build', self.main)

    def test_the_frame_button_holds_for_the_session(self):
        # 2026-11-17: the 60/30 button in play does not change the saved profile (Zelda); Bananza / Pokopia save it.
        pad = (TOOLS.parent/'Sources/touch_pad.m').read_text()
        body = pad[pad.index('- (void)switchFrameRate {'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('#ifdef GAME_NS2', body)
        self.assertLess(body.index('#ifdef GAME_NS2'), body.index('forKey:@"TOTK_PHONE_FPS"'))
        self.assertNotIn('totk_frame_button_key', pad)


class StartScreen(unittest.TestCase):
    """The start screen (ios/Sources/start_screen.m): the game starts from its button, the picture and frame rate are
    applied then (apply_display_settings in main.m), the picture row only where the Settings pane has Full HD."""
    def test_boots_from_the_start_screen_with_the_settings_applied_there(self):
        import re
        ios = TOOLS.parent
        main = (ios/'Sources/main.m').read_text()
        self.assertIn('#import "start_screen.h"', main)
        start = re.search(r'void \(\^start\)\(void\) = \^\{(.*?)\n    \};', main, re.S)
        self.assertTrue(start, 'the start screen before the boot')
        self.assertIn('TOTKStartView alloc] initWithFrame:self.view.bounds onStart:start]', main)
        # a script's run (TOTK_START_SCREEN=0) starts the same way without the screen (after PR #1174)
        self.assertRegex(main, r'getenv\("TOTK_START_SCREEN"\);\n    if \(screen && !strcmp\(screen, "0"\)\) start\(\);')
        body = start.group(1)
        self.assertLess(body.index('apply_display_settings()'), body.index('runtime_ios_sync_drawable'))
        self.assertLess(body.index('runtime_ios_sync_drawable'), body.index('boot]'))
        finish = main.index('- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:')
        self.assertNotIn('boolForKey:@"TOTK_DOCKED"', main[finish:], 'Full HD is applied when the game starts, not at launch')
        self.assertNotIn('stringForKey:@"TOTK_PHONE_FPS"', main[finish:])

    def test_shows_only_what_the_settings_pane_offers_and_writes_the_same_defaults(self):
        screen = (TOOLS.parent/'Sources/start_screen.m').read_text()
        self.assertIn('settingsPaneHas:@"TOTK_DOCKED"', screen)
        for key in ('forKey:@"TOTK_DOCKED"', 'forKey:@"TOTK_PHONE_FPS"'):
            self.assertIn(key, screen)
        self.assertNotIn('PLAYSHELF', screen.upper(), 'no launcher name on the game\'s screen')

class ProfileStartScreen(unittest.TestCase):
    """The start screen of Tears of the Kingdom and Breath of the Wild (spec "Tela inicial"): the three profiles (and
    Personalizado when a custom combination is saved), a line saying what the chosen one does, MetalFX on its own row
    (changing it keeps the profile), an Advanced button and Play. Bananza and Pokopia keep their picture / frame rows."""
    def setUp(self):
        self.screen = (TOOLS.parent/'Sources/start_screen.m').read_text()

    def test_the_profile_layout_where_the_pane_has_the_profile(self):
        self.assertIn('_hasProfiles = [self settingsPaneHas:@"TOTK_PICTURE_PROFILE"];', self.screen)
        self.assertIn('totk_has_custom_picture()', self.screen, 'Personalizado shows when a combination is saved')
        self.assertIn('totk_choose_profile(', self.screen)
        self.assertIn('totk_choose_metalfx(', self.screen)
        self.assertIn('totk_current_picture(&profile, &mfx)', self.screen)
        self.assertIn('@selector(openAdvanced)', self.screen)

    def test_metalfx_on_keeps_its_saved_variant_and_says_it(self):
        # Final review: with "Scene + screen" saved, Off then On saved "scene" while the segment still read "to the
        # screen". On restores the last On value of this screen, and the title follows what is saved.
        metalfx = self.screen[self.screen.index('- (void)metalfxChanged {'):]
        metalfx = metalfx[:metalfx.index('\n}\n')]
        self.assertIn('_lastMfxOn', metalfx)
        self.assertIn('setTitle:', metalfx)

    def test_scrolls_on_a_short_screen(self):
        # Final review: profile, two notes, MetalFX, Advanced and Play are taller than an iPhone in landscape.
        self.assertIn('UIScrollView', self.screen)

    def test_metalfx_does_not_touch_the_profile(self):
        metalfx = self.screen[self.screen.index('- (void)metalfxChanged {'):]
        metalfx = metalfx[:metalfx.index('\n}\n')]
        self.assertNotIn('totk_choose_profile', metalfx)
        self.assertNotIn('totk_save_custom_picture', metalfx)

    def test_the_presets_screens_stay(self):
        for key in ('settingsPaneHas:@"TOTK_DOCKED"', 'settingsPaneHas:@"TOTK_OUTPUT_RES"', 'forKey:@"TOTK_PHONE_FPS"', 'forKey:@"TOTK_DOCKED"'):
            self.assertIn(key, self.screen)

    def test_the_controller_moves_through_the_shown_controls(self):
        self.assertIn('NSArray<UIView *> *_controls;', self.screen)
        self.assertIn('NSInteger last = (NSInteger)_controls.count - 1;', self.screen)


class Advanced(unittest.TestCase):
    """The Advanced screen (spec "O Avançado"): every control of the profiles, one line on what each choice does, inside
    a scroll view (an iPhone in landscape); any change but MetalFX makes the profile Personalizado; MetalFX is its own
    setting. The Switch size only on an iPad, the aspect ratio only in Tears of the Kingdom."""
    def setUp(self):
        self.view = (TOOLS.parent/'Sources/advanced_view.m').read_text()

    def test_interface(self):
        header = (TOOLS.parent/'Sources/advanced_view.h').read_text()
        self.assertIn('- (instancetype)initWithFrame:(CGRect)frame onBack:(void (^)(void))onBack onStart:(void (^)(void))onStart;', header)
        screen = (TOOLS.parent/'Sources/start_screen.m').read_text()
        opened = screen[screen.index('- (void)openAdvanced {'):]
        self.assertIn('[[TOTKAdvancedView alloc] initWithFrame:', opened[:opened.index('\n}\n')])

    def test_changes_make_the_profile_custom_but_metalfx(self):
        self.assertIn('totk_save_custom_picture(&_pic);', self.view)
        mfx = self.view[self.view.index('- (void)setMetalfx:'):]
        mfx = mfx[:mfx.index('\n}\n')]
        self.assertIn('totk_choose_metalfx(', mfx)
        self.assertNotIn('totk_save_custom_picture', mfx)

    def test_effect_lines_follow_the_model(self):
        self.assertIn('pp_scene_effective(&_pic, PP_THIS_GAME)', self.view, 'says when the scene is held at the limit')
        self.assertIn('pp_mfx_scene_acts(&_pic, PP_THIS_GAME)', self.view, 'says when the scene upscale has nothing to do')

    def test_device_and_game_rows(self):
        self.assertIn('UIUserInterfaceIdiomPad', self.view[self.view.index('_switchSize'):], 'the Switch size row only on an iPad')
        aspect = self.view.index('_aspect = ')
        self.assertIn('#ifndef GAME_BOTW', self.view[self.view.rindex('#if', 0, aspect) - 1:aspect])
        self.assertIn('UIScrollView', self.view)

    def test_the_controller_reaches_every_control(self):
        self.assertIn('pollController', self.view)
        # Final review: the A press that opened Advanced was still down on its first poll, with the focus on Play, so the
        # game started; the screen waits for every button to be let go first.
        init = self.view[self.view.index('- (instancetype)initWithFrame:'):self.view.index('_poll = [NSTimer')]
        self.assertIn('_held = YES;', init)
        self.assertIn('buttonB', self.view, 'B goes back')


class SwitchSize(unittest.TestCase):
    """The game at the Switch's 6.2-inch screen on an iPad (TOTK_SWITCH_SIZE=1, user 2026-11-17), centred, so a
    low-resolution picture looks sharper. The app's picture settings hand the variable over (2026-11-17 profiles); a
    phone screen is already smaller, so there it changes nothing."""
    def test_the_game_view_takes_the_switch_size_when_the_game_starts(self):
        import re
        main = (TOOLS.parent/'Sources/main.m').read_text()
        start = re.search(r'void \(\^start\)\(void\) = \^\{(.*?)\n    \};', main, re.S).group(1)
        # read after the picture settings are handed over, placed before the drawable takes the view's shape
        self.assertLess(start.index('apply_display_settings()'), start.index('getenv("TOTK_SWITCH_SIZE")'))
        self.assertLess(start.index('getenv("TOTK_SWITCH_SIZE")'), start.index('[weak placeGameView];'))
        self.assertLess(start.index('[weak placeGameView];'), start.index('runtime_ios_sync_drawable(weak.gameView);'))
        place = re.search(r'- \(void\)placeGameView \{.*?\n}', main, re.S).group(0)
        self.assertIn('UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad', place, 'iPad only')
        self.assertIn('totk_switch_size(b.size.width, b.size.height, totk_ipad_ppi(machine), scale, &w, &h)', place)
        front = (TOOLS.parents[1]/'runtime/frontend_ios.m').read_text()
        self.assertIn('bootStage.autoresizingMask=centred;bootDetail.autoresizingMask=centred;', front)

    def test_not_on_vision_pro(self):
        """visionOS has no UIScreen (the xros build failed, 2026-11-18) and no Switch-sized picture: the placement code
        and its window-resize hook sit inside #if !TARGET_OS_VISION."""
        main = (TOOLS.parent/'Sources/main.m').read_text()
        at = main.index('- (void)placeGameView {')
        guard = main.rindex('#if !TARGET_OS_VISION', 0, at)
        self.assertNotIn('#endif', main[guard:at], 'placeGameView is inside the non-visionOS block')
        self.assertLess(main.index('- (void)viewDidLayoutSubviews', at), main.index('#endif', at))

    def test_no_pane_row(self):
        import plistlib
        root = plistlib.loads((TOOLS.parent/'Settings.bundle/Root.plist').read_bytes())
        self.assertNotIn('TOTK_SWITCH_SIZE', {s.get('Key') for s in root['PreferenceSpecifiers']},
                         'the picture settings in the app own it (profiles)')

if __name__ == '__main__':
    unittest.main()
