"""The English texts of MNG Launcher's Controls page, by id (S2.1, T1; E6 S6.2b). tools/builder_jobs/tab_controls.py
reads them (strings.EN) and pairs each with its Chinese; a missing key raises KeyError, caught by the drift test
(tests/test_builder_strings.py), not hidden at run time. The other pages keep their texts in their own
tools/builder_jobs/tab_*.py."""

EN = {
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
    'gui.controls.icons.not.not_offered': "The game's button icons can be updated for Tears of the Kingdom and Breath "
                                          'of the Wild only, for now.',
    'gui.controls.icons.not.no_app': 'Button icons: build the app first.',
    'gui.controls.icons.not.not_latest': 'Button icons: Breath of the Wild 1.9.0 only (this app is another version).',
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
    'gui.controls.icons.switch_off': '{mods} also replace the game\'s font archives (or Breath of the Wild\'s '
                                     'Pack/Bootup.pack), so the two would clash. Switch '
                                     'them off (they stay in your list)?',
    'gui.controls.icons.kept': 'Icons not updated: {mods} still replace the same fonts.',
    'gui.controls.icons.making': 'Making the button icons (Tears of the Kingdom: the first time, fontTools is '
                                 'downloaded first); then '
                                 'Apply mods runs for your whole list, as in the Mods tab...',
    'gui.controls.icons.done': 'Button icons updated and the mods applied: the game shows them at its next start.',
    'gui.controls.icons.failed': 'Button icons not updated ({reason}): {detail}',
    'gui.controls.restore': 'Restore defaults',
    'gui.controls.cancel': 'Cancel changes',
    'gui.controls.save': 'Save controls',
    # the key test
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


def text(key, **values):
    """The English text for key, with values filled in. KeyError for an unknown key."""
    return EN[key].format(**values)
