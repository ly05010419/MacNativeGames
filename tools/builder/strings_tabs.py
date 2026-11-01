"""The Settings, Controls, storage, saves and checks tabs' texts (S5.1, E6), by id; strings.py merges them into its
table (kept apart so each file stays under 300 lines; the Mods tab's are in strings_mods.py). Same rules as
strings.py: a missing key raises KeyError, caught by the drift test."""

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
                                 'native 2K draws at 1440p, native 4K at 2160p (the next build makes it).',
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

    # --- S2.3 storage tab: the two caches, their size, cleaning and moving ---
    'gui.tab.storage': 'Storage',
    'gui.storage.title': 'Caches',
    'gui.storage.note': 'Two folders the builder and the games write. Cleaning removes only what is inside them; '
                        'the game or the next build makes it again. Saves and the app are not touched.',
    'gui.storage.work': 'Build cache (work folder)',
    'gui.storage.shader': 'Shader cache',
    'gui.storage.path': '{label}: {path}',
    'gui.storage.exists': '{size} in {files} files',
    'gui.storage.missing': 'Does not exist yet.',
    'gui.storage.measuring': 'Measuring...',
    'gui.storage.errors': 'Some files could not be read: {detail}',
    'gui.storage.clean': 'Clean',
    'gui.storage.confirm_clean': 'Delete everything inside {path}?\n\n{size} in {files} files. The game or the '
                                 'next build makes it again; saves and the app are not touched.',
    'gui.storage.cleaned': 'Removed {size} in {files} files.',
    'gui.storage.clean_failed': 'Could not clean: {reason}',
    'gui.storage.choose': 'Choose folder...',
    'gui.storage.chosen': 'The work folder is now {path} (saved to settings.json for this game).',
    'gui.storage.moved': 'Moved {count} entries to the new folder.',
    'gui.storage.move_failed': 'Could not move: {reason}',
    'gui.storage.move_offer': 'The cache was moved into {path}. The next build uses it.',
    'gui.storage.move_target': 'Choose the folder to move the cache into.',
    'gui.storage.confirm_move': 'Move everything in {path} into the new folder?\n\n{target}',
    'gui.storage.shader_note': 'The launcher writes this cache. To keep it elsewhere, start the game with {name} set '
                               'to the new folder; the builder shows that folder here after a restart.',
    'gui.storage.refresh': 'Measure again',
    'gui.storage.not_saved': 'Not saved: {error}',
    'gui.storage.refused.missing': 'That folder does not exist.',
    'gui.storage.refused.root': 'That is the disk root.',
    'gui.storage.refused.home': 'That is your home folder, or one of its parents.',
    'gui.storage.refused.system': 'That is a macOS system folder.',
    'gui.storage.refused.protected': 'That folder holds the game, its app or its saves.',
    'gui.storage.refused.file': 'That is a file, not a folder.',
    'gui.storage.refused.inside_source': 'The new folder is inside the old one.',
    'gui.storage.refused.target_not_empty': 'The new folder is not empty.',
    # --- S2.4 saves tab ---
    'gui.tab.saves': 'Saves',
    'gui.saves.title': 'Saves',
    'gui.saves.note': 'A backup is a plain folder: the same files, plus a backup.json saying what it holds.',
    'gui.saves.profile': 'Game save',
    'gui.saves.nxnative': 'nxnative title',
    'gui.saves.files': '{size} in {files} files',
    'gui.saves.none': 'No save yet.',
    'gui.saves.missing': 'Nothing at {path} (no save yet).',
    'gui.saves.backup': 'Back up...',
    'gui.saves.backup_choose': 'Choose where to keep backups.',
    'gui.saves.backed_up': 'Backed up to {path}.',
    'gui.saves.backup_failed': 'Could not back up: {reason}',
    'gui.saves.root': 'Backups: {path}',
    'gui.saves.restore': 'Restore...',
    'gui.saves.restore_choose': 'Choose a backup folder to restore.',
    'gui.saves.restore_plan': 'Restore {backup} over {target}? {write} files will be written, {kept} files kept as '
                              'they are, {same} already match. The current save is backed up first.',
    'gui.saves.pre_backup': 'Current save backed up to {path}.',
    'gui.saves.restored': 'Restored {write} files from {backup}.',
    'gui.saves.restore_failed': 'Could not restore: {reason}',
    'gui.saves.running': 'The game is running: quit it first, or it writes its own save back over the restored one.',
    'gui.saves.not_backup': 'That folder is not a backup this builder made.',
    'gui.saves.no_backups': 'No backups in {path} yet.',
    'gui.saves.list': 'Backups in {path}',
    'gui.saves.refresh': 'Read again',
    'gui.saves.kept': 'Kept (not in the backup): {count} files.',
    'gui.saves.hash_note': 'SHA-256 for files up to 2 GiB.',
    'gui.saves.reading': 'Reading the saves...',
    'gui.saves.restoring': 'Backing up the current save, then restoring...',
    'gui.saves.wrong_game': 'That backup is for {game}: choose that game in the Build tab first.',
    # --- Checks: logs summary and the report export ---
    'gui.checks.logs': 'Logs',
    'gui.checks.logs_dir': 'Logs folder: {path}',
    'gui.checks.log_builder': 'Builder log: {path}',
    'gui.checks.log_game': 'Game log: {path}',
    'gui.checks.log_missing': 'missing',
    'gui.checks.export': 'Export report...',
    'gui.checks.report_kind': 'Check report',
    'gui.checks.export_choose': 'Save the check report as JSON or text.',
    'gui.checks.exported': 'Report saved to {path}.',
    'gui.checks.export_failed': 'Could not write the report: {error}',
    'gui.checks.export_empty': 'Run the checks first: the report lists their results.',
    # --- Controls: the key test ---
    'gui.controls.test.title': 'Key test',
    'gui.controls.test.note': 'Press keys to see what this game has them doing. The test only reads the builder '
                              'window: system shortcuts still work, and nothing is sent to the game.',
    'gui.controls.test.start': 'Start key test',
    'gui.controls.test.stop': 'Stop key test',
    'gui.controls.test.stopped': 'Not testing.',
    'gui.controls.test.running': 'Testing: press a key (Escape also stops).',
    'gui.controls.test.none': 'Press a key...',
    'gui.controls.test.key': 'Pressed: {key}',
    'gui.controls.test.modifiers': 'Pressed: {modifiers}+{key}',
    'gui.controls.test.char': 'character {char}',
    'gui.controls.test.code': 'key code {code}',
    'gui.controls.test.actions': '{key} -> {actions}',
    'gui.controls.test.unbound': '{key} -> not bound',
    'gui.controls.test.unread': 'The layout could not be read: {detail}',
    'gui.controls.test.defaults': 'This game has no keyboard.txt yet: the defaults are shown.',
}
