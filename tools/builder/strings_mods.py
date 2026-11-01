"""The Mods tab's texts (S5.2), by id; strings.py merges them into its table (kept apart from strings_tabs.py so
each file stays under 300 lines). Same rules as strings.py: a missing key raises KeyError, caught by the drift test."""

MODS = {
    'gui.mods.note': 'You add the mods; the builder ships none. Only a valid mod can be switched on: its paths fit '
                     'this version and it replaces a file of the game; code patches go in through a build. That '
                     'does not test it in play.',
    'gui.mods.add': 'Add mod...',
    'gui.mods.adding': 'Copying and checking the mod...',
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
    'gui.mods.applied.cancelled': 'Not applied: cancelled while the texture pack was being fitted. The game keeps '
                                  'the mods it had; Apply mods again.',
    'gui.mods.applied.textures_failed': 'Not applied: the texture pack\'s size tables could not be fitted to the app:',
    'gui.mods.not_applied': 'Not applied: the game holds the mods of the last Apply, not these. Apply mods again.',
    'gui.mods.reason.no_game_file': 'Choose the game file in the Build tab: the app is found next to it.',
    'gui.mods.reason.no_app': 'No built app yet: build it in the Build tab first.',
    'gui.mods.reason.not_latest': 'Mods here are for {name} {version} only: build that version to use them.',
    'gui.mods.reason.not_supported': "Mods for {name} {version} come when MacNativeGames' mod tools support it.",
}
