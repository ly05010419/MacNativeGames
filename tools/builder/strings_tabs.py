"""The Settings and Mods tabs' texts (S5.1, S5.2), by id; strings.py merges them into its table (kept apart so each
file stays under 300 lines). Same rules as strings.py: a missing key raises KeyError, caught by the drift test."""

TABS = {
    'gui.settings.choose_game': 'Choose a game in the Build tab first.',
    'gui.settings.reading': 'Reading the settings...',
    'gui.settings.reason.no_game_file': 'Choose the game file in the Build tab: the app is found next to it, or at '
                                        'the Output app under Advanced.',
    'gui.settings.reason.no_app': 'No built app at {app}: build it in the Build tab first.',
    'gui.settings.reason.unknown_version': 'The app at {app} is not Tears of the Kingdom 1.4.3 or Breath of the Wild '
                                           '1.9.0: these settings cover the latest versions only.',
    'gui.settings.option.picture': 'Picture at start',
    'gui.settings.option.interpolation': 'Frame interpolation',
    'gui.settings.option.smoothing': 'Edge smoothing',
    'gui.settings.option.ground': 'Sharper ground at a distance',
    'gui.settings.option.aspect': 'Picture shape',
    'gui.settings.choice.auto': 'Automatic',
    'gui.settings.choice.metalfx': 'MetalFX 2K',
    'gui.settings.choice.native-2k': 'Native 2K',
    'gui.settings.choice.interp30': '30+30',
    'gui.settings.choice.interp': 'Half the refresh rate',
    'gui.settings.choice.interpmax': 'Uncapped',
    'gui.settings.choice.on': 'On',
    'gui.settings.choice.16': '16x',
    'gui.settings.choice.21:9': '21:9',
    'gui.settings.gpu.checking': 'Checking the GPU for MetalFX frame interpolation...',
    'gui.settings.gpu.no': "This Mac's GPU has no Metal 4 MetalFX frame interpolation: the game would stop, so it is "
                           'not offered.',
    'gui.settings.gpu.unknown': 'The GPU could not be checked: if it has no Metal 4 MetalFX frame interpolation, the '
                                'game stops at start with interpolation on.',
    'gui.settings.note.running': 'The game is running: close it to change these settings (or use its settings panel, key 1).',
    'gui.settings.play': '▶ Play',
    'gui.settings.title': '{name} · {version}',
    'gui.settings.applies': 'These are the game\'s own settings, the same as its settings panel (key 1): they hold for '
                            'every start, from Play or from Finder.',
    'gui.settings.desc.picture': 'Automatic starts at 1920x1080 without MetalFX. MetalFX 2K sharpens it to 1440p; '
                                 'native 2K draws at 1440p.',
    'gui.settings.desc.interpolation': 'MetalFX adds frames between the game\'s own.',
    'gui.settings.desc.smoothing': 'MetalFX smooths the stair-stepped edges of the 3D scene.',
    'gui.settings.desc.ground': 'Sharper ground, walls and foliage far away (16x anisotropic filtering).',
    'gui.settings.desc.aspect': '21:9, or the shape of the window, instead of 16:9.',
    'gui.settings.also_finder': 'Kept in the game\'s settings panel (key 1): changing it in either place changes both.',
    'gui.settings.play_only': 'Only when the game is started with Play (the settings panel does not have it).',
    'gui.settings.why_not.app': 'Choose a built app to play',
    'gui.settings.why_not.build': 'Wait until the build ends',
    'gui.settings.playing': 'Starting the game...',
    'gui.settings.played': 'Started with these settings.',
    'gui.settings.play_failed.running': 'The game is already running: close it, then Play.',
    'gui.settings.play_failed.no_game_file': 'Choose the game file in the Build tab first.',
    'gui.settings.play_failed.no_app': 'No built app: build it first.',
    'gui.settings.play_failed.unknown_version': 'This app is not 1.4.3 or 1.9.0.',
    'gui.settings.play_failed.open_failed': 'The game could not be started: {detail}',
    'gui.settings.held.interpolation': 'This start is without frame interpolation (your choice is kept): the GPU '
                                       'check had not answered yet, or it said no. Play again once it has answered.',
    'gui.settings.error': 'Settings: {error}',
    'gui.mods.note': 'You add the mods; the builder ships none. Only a valid mod can be switched on: its paths fit '
                     'this version and it replaces a file of the game; code patches go in through a build. That '
                     'does not test it in play.',
    'gui.mods.add': 'Add mod...',
    'gui.mods.adding': 'Copying and checking the mod...',
    'gui.controls.title': '{name} · controls',
    'gui.controls.not_offered': 'Controls for this game are not in the builder yet.',
    'gui.controls.note': 'Pick an action, then the button on the controller. Each action is the game\'s button named '
                         'after its main use: moving it also moves everything that button does elsewhere (menus, '
                         'riding, shields...).',
    'gui.controls.pick': 'Pick an action on the left, then a button on the controller.',
    'gui.controls.current': '{name}: {button} now.',
    'gui.controls.pending': '{name} → {button} (not saved).',
    'gui.controls.drives': '{button} does {name} now.',
    'gui.controls.unconfirmed': '{button} — function to confirm',
    'gui.controls.none': 'no button',
    'gui.controls.which': 'Which controller do you play with? The game reads its face buttons by position on an Xbox '
                          'or PlayStation controller and by label on a Nintendo one (key 6 says "auto").',
    'gui.controls.answer.xbox': 'Xbox or PlayStation',
    'gui.controls.answer.nintendo': 'Nintendo',
    'gui.controls.mode.xbox': 'Controller type (key 6): Xbox, face buttons by position. Changing it in the game moves '
                              'the face buttons of this layout.',
    'gui.controls.mode.playstation': 'Controller type (key 6): PlayStation, face buttons by position. Changing it in '
                                     'the game moves the face buttons of this layout.',
    'gui.controls.mode.switch': 'Controller type (key 6): Switch, face buttons by label (the A button is A): the '
                                'drawing\'s A, B, X and Y are the letters on your controller, wherever they sit. '
                                'Changing it in the game moves the face buttons of this layout.',
    'gui.controls.mode.auto.xbox': 'Controller type (key 6): auto, read for an Xbox or PlayStation controller (by '
                                   'position). TOTK_FACE_BUTTONS set by hand would win.',
    'gui.controls.mode.auto.nintendo': 'Controller type (key 6): auto, read for a Nintendo controller (by label): '
                                       'the drawing\'s A, B, X and Y are the letters on your controller, wherever '
                                       'they sit. TOTK_FACE_BUTTONS set by hand would win.',
    'gui.controls.link': 'controls.txt is a link: it is saved to the file it points to.',
    'gui.controls.broken': 'controls.txt has a line the game refuses (line {line}: {error}); the game would not '
                           'start. Restore defaults keeps it as a backup.',
    'gui.controls.custom.tap': 'tap', 'gui.controls.custom.toggle': 'toggle', 'gui.controls.custom.aim': 'aim',
    'gui.controls.custom.none': 'none', 'gui.controls.custom.shared': 'shared',
    'gui.controls.convert': 'Make {buttons} plain buttons? What you set by hand on them (a mode, or none) is dropped '
                            'when you save; another button\'s aim that fires with one of them keeps its own line.',
    'gui.controls.saving': 'Saving...',
    'gui.controls.saved': 'Saved. The game reads it at its next start. The old file is kept as {backup}.',
    'gui.controls.no_backup': 'nothing (there was none)',
    'gui.controls.not_saved.other_game': 'Not saved: another game is chosen now. Its controls are read again.',
    'gui.controls.not_saved.running': 'Not saved: the game is running. Close it, then save.',
    'gui.controls.not_saved.changed': 'Not saved: controls.txt or the controller type changed meanwhile. Check the '
                                      'new layout and save again.',
    'gui.controls.not_saved.refused': 'Not saved: the new file would not be what you chose ({detail}).',
    'gui.controls.confirm_restore': 'Move controls.txt aside (kept as a backup) and use the game\'s default layout?',
    'gui.controls.preset.xbox': 'Xbox letters',
    'gui.controls.preset.xbox_note': 'A does what the game calls A, B what it calls B, X and Y the same: the letters '
                                     'in the game\'s prompts are the buttons to press.',
    'gui.controls.preset.done': 'Xbox letters: A, B, X and Y now do what the game shows with those letters (not '
                                'saved).',
    'gui.controls.preset.same': 'A, B, X and Y already do what the game shows with those letters.',
    'gui.controls.icons.update': 'Update the game\'s button icons',
    'gui.controls.icons.none': 'The game\'s prompts still show Switch buttons. Update them to show the Xbox button '
                               'that does each one in this layout (saved layouts only).',
    'gui.controls.icons.outdated': 'The game\'s button icons no longer match the saved layout (or another mod\'s '
                                   'size table changed): update them.',
    'gui.controls.icons.current': 'The game\'s button icons show the buttons of the saved layout.',
    'gui.controls.icons.shared': 'Two buttons do {inputs}: its prompt names the first.',
    'gui.controls.icons.not.not_offered': "The game's button icons can be updated for Tears of the Kingdom only, for "
                                          'now.',
    'gui.controls.icons.not.no_app': 'Button icons: build the app first.',
    'gui.controls.icons.not.ask': 'Button icons: say which controller you use first.',
    'gui.controls.icons.not.nintendo': 'Button icons: with a Nintendo controller (or key 6 on Switch) the game\'s own '
                                       'icons are kept; this draws Xbox names.',
    'gui.controls.icons.not.broken_file': 'Button icons: fix or restore controls.txt first.',
    'gui.controls.icons.download.missing': 'Drawing the icons needs fontTools 4.67.0, downloaded from pypi.org '
                                           '(checked against pinned SHA-256 hashes) into the builder\'s own Python '
                                           'environment (Builder/icons-venv). Download it now?',
    'gui.controls.icons.download.broken': 'The builder\'s Python environment for the icons no longer works (Python '
                                          'was probably upgraded). Make it again, downloading fontTools 4.67.0 from '
                                          'pypi.org (pinned SHA-256 hashes)?',
    'gui.controls.icons.switch_off': '{mods} also replace the game\'s font archives, so the two would clash. Switch '
                                     'them off (they stay in your list)?',
    'gui.controls.icons.kept': 'Icons not updated: {mods} still replace the same fonts.',
    'gui.controls.icons.making': 'Making the button icons (the first time, fontTools is downloaded first); then '
                                 'Apply mods runs for your whole list, as in the Mods tab...',
    'gui.controls.icons.done': 'Button icons updated and the mods applied: the game shows them at its next start.',
    'gui.controls.icons.failed': 'Button icons not updated ({reason}): {detail}',
    'gui.controls.restore': 'Restore defaults',
    'gui.controls.cancel': 'Cancel changes',
    'gui.controls.save': 'Save controls',
    'gui.mods.icons_outdated': 'Out of date: the saved controls or another mod\'s size table changed. Update it in '
                               'the Controls tab.',
    'gui.mods.add_archive': 'Add archive...',
    'gui.mods.added': 'Added.',
    'gui.mods.added_pack': 'Added {n} mods from {name}: switch on the ones you want.',
    'gui.mods.zip_kind': 'Mod archives (.zip, .7z, .rar; mod manager packages are refused with a reason)',
    'gui.mods.switched_off': 'Switched off: {mods}. Made for another version, they cannot be switched on for 1.4.3, '
                             'and the next Apply mods takes all of each out of the game, its player pack too.',
    'gui.mods.invalid.other_version': 'Made for TOTK {versions}, not 1.4.3: the game would ignore its game data and '
                                      'size tables. Convert it with TKMM (export for 1.4.3) and add the exported '
                                      'folder.',
    'gui.mods.other_ignored': 'It also holds files for other versions of the game, which the game ignores.',
    'gui.mods.refused.tkmm_package': 'Not added: {detail} is a TKMM package. Import it in TKMM, export for 1.4.3, '
                                     'and add the exported folder.',
    'gui.mods.refused.cancelled': 'Not added: the extraction was cancelled.',
    'gui.mods.empty': 'No mods yet. Add a mod as it came: its folder (romfs/, exefs/, or the game\'s own folders such '
                      'as Pack/ or UI/, at most 4 levels down) or its archive (.zip, .7z, .rar).',
    'gui.mods.changed': 'The app changed since the mods were applied: Apply mods again before playing.',
    'gui.mods.valid': 'Valid: replaces {replaced} files, adds {added}.',
    'gui.mods.invalid.misspelt': 'Not valid: paths with the wrong upper or lower case: {paths}.',
    'gui.mods.invalid.only_adds': 'Not valid: it replaces no file of the game, it only adds files (is its folder one '
                                  'level off, or is it for another game?).',
    'gui.mods.invalid.all_taken': 'Not valid here: everything it changes the app already replaces; it needs a build '
                                  'with this mod (build_app.py --mod).',
    'gui.mods.invalid.no_romfs': 'No RomFS part (romfs/) to apply: it has code patches only (exefs/).',
    'gui.mods.invalid.no_app_romfs': 'Not checked: the app has no RomFS index for this version.',
    'gui.mods.invalid.unchecked': 'Not checked yet.',
    'gui.mods.taken': 'The app replaces these itself (they need a build with this mod): {paths}',
    'gui.mods.code.ok': 'Code patches for 1.4.3: switch on "its code" and build again to merge them.',
    'gui.mods.code.other_build': 'Code patches for another build of the game: not usable here.',
    'gui.mods.code.not_for_game': 'Code patches (exefs/) are not supported for Breath of the Wild: only the romfs/ '
                                  'part is used.',
    'gui.mods.code.unchecked': 'Code patches (an .ips) whose build cannot be checked here: build_app.py checks '
                               'them when it merges them.',
    'gui.mods.code.whole_module': 'A whole replacement main (exefs/main): it cannot be merged, a patch is needed '
                                  '(.pchtxt or .ips).',
    'gui.mods.code_toggle': 'Its code in the next build',
    'gui.mods.code_pending': 'This app was built with other code mods than the ones switched on: build it again '
                             'to apply them.',
    'gui.mods.code_line': 'The next build merges the code of: {names}. They are for 1.4.3; build_app.py checks '
                          'each patch and stops on one it cannot merge.',
    'gui.mods.code_line_no_update': 'Code mods switched on ({names}) are merged only in a build with the 1.4.3 '
                                    'update file: choose it above.',
    'gui.mods.code_other_build': 'These code mods are for 1.4.3: take their code out in the Mods tab, or build with '
                                 'the 1.4.3 update.',
    'gui.mods.notes.cheats_ignored': 'Its cheats/ folder is not used: MacNativeGames does not run cheat files.',
    'gui.mods.refused.other_game': 'Not added: it is for another game ({detail}).',
    'gui.mods.refused.bcml_package': 'Not added: {detail} is a BCML package (.bnp), which the builder cannot merge. '
                                     'Install it with BCML, or use a RomFS version of the mod.',
    'gui.mods.refused.several': 'Not added: it holds several mods ({detail}). Add the folder of the one you want.',
    'gui.mods.refused.too_big': 'Not added: the folder is too big to search. Pick the mod\'s own folder.',
    'gui.mods.refused.cheats_only': 'Not added: it only holds cheat files, which MacNativeGames does not run.',
    'gui.mods.refused.nothing': 'Not added: no romfs/ or exefs/ folder within 4 levels, and its folders are not the '
                                'game\'s own (Pack/, UI/...).',
    'gui.mods.refused.links': 'Not added: it holds symbolic links ({detail}); copy the real files in.',
    'gui.mods.refused.archive_kind': 'Not added: only .zip, .7z and .rar archives are read here ({detail}); '
                                     'extract it first.',
    'gui.mods.refused.zip_entries': 'Not added: the archive has too many entries ({detail}).',
    'gui.mods.refused.zip_path': 'Not added: the archive has a path outside itself, or a file named like a folder '
                                 '({detail}).',
    'gui.mods.refused.zip_link': 'Not added: the archive holds a link ({detail}).',
    'gui.mods.refused.zip_case': 'Not added: two entries have one name on a Mac disk (they differ only in upper or '
                                 'lower case, or in how an accent is written): {detail}.',
    'gui.mods.refused.zip_space': 'Not added: not enough free disk space to extract {detail}.',
    'gui.mods.refused.zip_big': 'Not added: {detail} extracts to more than 64 GB.',
    'gui.mods.refused.zip_bad': 'Not added: the archive cannot be read ({detail}).',
    'gui.mods.conflicts': '{n} files also replaced by another mod; the one lower in the list wins: {paths}',
    'gui.mods.remove': 'Remove',
    'gui.mods.confirm_remove': 'Remove "{name}" from the list? Only the builder\'s copy is deleted; your folder stays.',
    'gui.mods.backups': '{n} backups of your own mod folder are kept next to it.',
    'gui.mods.remove_backups': 'Remove them',
    'gui.mods.confirm_backups': 'Delete the {n} backups of your own mod folder? This cannot be undone.',
    'gui.mods.after_build': 'Your applied mods no longer fit this app: open the Mods tab and Apply mods again '
                            'before playing, or the game may stop at start.',
    'gui.mods.apply': 'Apply mods',
    'gui.mods.applying': 'Applying the mods...',
    'gui.mods.why_not': 'Choose a built app of the latest version',
    'gui.mods.applied.ok': 'Mods applied correctly.',
    'gui.mods.applied.nothing': 'No mod is on: the mod folder was set aside, the game starts without mods.',
    'gui.mods.applied.set_aside': 'The mods on load nothing: the mod folder was set aside.',
    'gui.mods.applied.running': 'The game is running: close it, then Apply mods.',
    'gui.mods.applied.no_space': 'Not enough free disk space to apply these mods.',
    'gui.mods.applied.install_failed': 'The mods could not be applied:',
    'gui.mods.not_applied': 'Not applied: the game holds the mods of the last Apply, not these. Apply mods again.',
    'gui.mods.reason.no_game_file': 'Choose the game file in the Build tab: the app is found next to it.',
    'gui.mods.reason.no_app': 'No built app yet: build it in the Build tab first.',
    'gui.mods.reason.not_latest': 'Mods here are for {name} {version} only: build that version to use them.',
    'gui.mods.reason.not_supported': "Mods for {name} {version} come when MacNativeGames' mod tools support it.",
}
