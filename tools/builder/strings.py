"""Every user-visible text of the builder's views, by id (S2.1, T1).

The checks and the controller emit ids (check ids, message ids, outcomes); this table turns them into English. A missing
key raises KeyError: that is a bug, caught by the drift test (tests/test_builder_strings.py), not hidden at run time.
Another language would be another table, not added now. cancel_report() is T7, shared by the text mode and the window
(moved here from cli.py in S2.3)."""
from tools.builder.session import SIGNAL_NAMES
from tools.builder.strings_tabs import TABS

EN = {
    # check labels, one per check id
    'check.apple_silicon': 'Apple silicon Mac',
    'check.macos_version': 'macOS 26 or later',
    'check.command_line_tools': 'Command Line Tools',
    'check.cxx_headers': 'C++ headers',
    'check.homebrew': 'Homebrew',
    'check.brew_packages': 'Homebrew packages (opus, libvpx)',
    'check.git': 'git',
    'check.build_python': 'Python 3.14 for the build',
    'check.metal_toolchain': 'Metal toolchain',
    'check.repository': 'MacNativeGames repository',
    'check.input_files': 'Game, keys and other input files',
    'check.input_rules': 'Input combination',
    'check.disk_space': 'Free disk space',
    'check.output_volume': 'Output folder volume',
    'check.keys': 'Keys file (prod.keys)',

    # one sentence per message id; {detail} is the check's detail
    'msg.ok': 'Ready.',
    'msg.skipped': 'Not checked ({detail}).',
    'msg.check_crashed': 'The check itself failed: {detail}.',
    'msg.command_failed': 'A system command did not answer as expected: {detail}.',
    'msg.intel_mac': 'This Mac is not an Apple silicon Mac ({detail}); build_app.py needs one.',
    'msg.macos_unknown': 'The macOS version could not be read: {detail}.',
    'msg.macos_too_old': 'macOS {detail} is too old; build_app.py needs macOS 26 or later.',
    'msg.developer_dir_override_invalid': 'DEVELOPER_DIR points to a folder without developer tools ({detail}).',
    'msg.command_line_tools_missing': 'The Command Line Tools are not installed.',
    'msg.developer_dir_selection_invalid': 'The selected developer folder has no tools: {detail}.',
    'msg.cxx_headers_needs_diagnosis': 'The C++ compiler cannot find its standard headers ({detail}).',
    'msg.homebrew_missing': 'Homebrew is not installed ({detail} is missing).',
    'msg.brew_packages_missing': 'Missing Homebrew packages: {detail}.',
    'msg.git_missing': 'git was not found.',
    'msg.build_python_missing': 'Python 3.14 was not found in {detail}.',
    'msg.python_not_arm64': 'The build Python does not run as arm64 ({detail}).',
    'msg.metal_toolchain_not_found': 'No Metal toolchain was found here ({detail}); the build may still find one.',
    'msg.repository_incomplete': 'The repository is incomplete: {detail}.',
    'msg.repository_newer_than_builder': 'The repository has versions this builder does not know yet: {detail}.',
    'msg.input_missing': 'Not found: {detail}.',
    'msg.input_unreadable': 'Cannot be opened: {detail}.',
    'msg.output_not_app': 'The output must end in .app: {detail}.',
    'msg.font_names_unknown': 'The shared font names could not be read from {detail}.',
    'msg.input_rules': 'These inputs cannot be built: {detail}.',
    # one per BLOCK_RULES reason of inputs.py; check_sentence() joins them for msg.input_rules (S3.4, f1)
    'reason.base_kind': 'the game file must be an XCI or NSP card image (NSZ and XCZ are not supported)',
    'reason.update_kind': 'the update must be an NSP or NSZ file',
    'reason.fonts_both': 'choose a firmware folder or a shared-fonts folder, not both',
    'reason.fonts_needed': 'this game needs the system fonts (choose a firmware folder or a shared-fonts folder)',
    'reason.botw_needs_update': 'a Breath of the Wild base NSP needs its 1.6.0 update',
    'msg.disk_space': 'Not enough free space: {detail}.',
    'msg.keys_unreadable': 'The keys file cannot be read: {detail}.',
    'msg.keys_invalid': 'The keys file has lines that are not "name = 32 or 64 hex digits": {detail}.',
    'msg.keys_missing_names': 'The keys file lacks {detail}.',
    'msg.output_other_volume': 'The app goes to another volume than the work folder ({detail}); copying takes longer.',

    # outcomes (CANCELLED is the sentence for a cancel before anything ran; T7 covers the others)
    'outcome.start_failed': 'The build could not start.',
    'outcome.game_mismatch': 'The game file is the other game; the build was stopped.',
    'outcome.cancelled': 'Cancelled before the build started.',
    'outcome.incomplete': 'The build ended, but its log could not be read to the end, so success cannot be shown.',
    'outcome.success': 'The build finished.',
    'outcome.failed': 'The build failed.',

    # the text mode's own lines
    'cli.settings_problem': '  ! saved settings: {problem}',
    'cli.load_slow': '  ! the saved settings did not answer within {seconds} s: going on without them',
    'cli.paths_missing': 'Missing paths: {fields}.',
    'cli.flag.game': '--game-file',
    'cli.flag.keys': '--keys',
    'cli.flag.update': '--update',
    'cli.flag.firmware': '--firmware',
    'cli.flag.shared_fonts': '--shared-fonts',
    'cli.flag.work': '--work',
    'cli.flag.out': '--out',
    'cli.check_ok': '  OK    {label}',
    'cli.check': '  {status:<5} {label}: {sentence}',
    'cli.fix': '        fix: {fix}',
    'cli.checks_ready': 'Checks: ready.',
    'cli.checks_blocked': 'Checks: blocked. Nothing was started.',
    'cli.logs': '  logs: {path}',
    'cli.command': 'Command: {command}',
    'cli.prompt': 'Build now? [y/N] ',
    'cli.not_started': 'Not started.',
    'cli.interrupted': 'Interrupted. Nothing was started.',
    'cli.started': 'Started. Log: {log}',
    'cli.stage': '[{index}/{total} {name}] {text}',
    'cli.heartbeat': '  ... still running ({minutes} min)',
    'cli.process_error': '  ! {where}: {error}',
    'cli.retry_hint': '    press Ctrl-C again to retry',
    'cli.settings_not_saved': '  ! settings not saved: {error}',
    'cli.cancel_stuck': 'The build did not stop even after SIGKILL. It is still tracked; waiting for it to end.',
    'cli.cancelling': 'Cancelling; waiting for the build to stop.',
    'cli.retrying': 'Retrying the cancel.',
    'cli.still_stopping': 'The build is still stopping; waiting.',
    'cli.no_finished': '  ! the build ended without a final report',
    'cli.tick_error': '  ! tick: {error}',
    'cli.tracking_lost': 'Stopped tracking the build after repeated errors; build_app.py may still be running. '
                         'Log: {log}',
    'cli.cancelled_compatible': 'Cancelled: the build ended with {termination}, which matches a signal that was '
                                'accepted ({names}).',
    'cli.cancel_not_explained': 'Cancel requested; the build ended with {termination}, which no accepted signal '
                                'explains ({names}). Without the request it would have been: {observed}.',
    'cli.app': 'App: {app}',
    'cli.error': 'Error: {error}',
    'cli.last_lines': 'Last {n} log lines:',
    'cli.log': 'Log: {log}',

    # the window (S2.2)
    'gui.title': 'MacNativeGames builder',
    'gui.loading': 'Reading the saved settings...',
    'gui.load_slow': 'The saved settings did not answer within {seconds} s: going on without them.',
    'gui.load_late': 'The saved settings arrived late: they fill the next game you choose; nothing you chose changed.',
    'gui.settings_problem': 'Saved settings: {problem}',
    'gui.load_error': 'The saved settings could not be read: {error}',
    'gui.no_tk': 'Tk is not available in this Python (brew install python-tk@3.14); using the text mode.',
    'gui.choose': 'Choose...',
    'gui.hint.default_next_to_game': 'Empty: next to the game file.',
    'gui.field.game': 'Game file (XCI or NSP)',
    'gui.field.keys': 'Keys (prod.keys)',
    'gui.field.update': 'Update (NSP or NSZ)',
    'gui.field.firmware': 'Firmware folder',
    'gui.field.shared_fonts': 'Shared fonts folder',
    'gui.field.work': 'Work folder',
    'gui.field.out': 'Output app',
    'gui.check': 'Check files',
    'gui.build': 'Build app',
    'gui.copy': 'Copy',
    'gui.check_ok': 'OK     {label}',
    'gui.check_line': '{status:<6} {label}: {sentence}',
    'gui.fix': 'Fix: {fix}',
    'gui.paths_missing': 'Missing: {fields}.',
    'gui.checks_ready': 'Ready to build.',
    'gui.checks_blocked': 'Blocked: fix the lines above, then check again.',
    'gui.checks_stale': 'A path changed: check again.',
    'gui.process_error': '{where}: {error}',
    'gui.log_path': 'Log: {log}',
    'gui.stage': '{index}/{total} {name}: {text}',
    'gui.elapsed': 'Elapsed: {minutes} min {seconds:02d} s',
    'gui.show_log': 'Show log',
    'gui.hide_log': 'Hide log',
    'gui.cancel': 'Cancel',
    'gui.cancelling': 'Cancelling...',
    'gui.retry_cancel': 'Retry cancel',
    'gui.cancel_stuck': 'The build did not stop even after SIGKILL. It is still tracked.',
    'gui.settings_not_saved': 'Settings not saved: {error}',
    'gui.open_app': 'Open app',
    'gui.show_in_finder': 'Show in Finder',
    'gui.app': 'App: {app}',
    'gui.error': 'Error: {error}',
    'gui.last_lines': 'Last {n} log lines:',
    'gui.new_build': 'New build',
    'gui.confirm_close': 'A build or a mod change is running, or a build is finishing its saves. Cancel '
                         'what can be cancelled and close the window when it has stopped?',
    'gui.confirm.yes': 'Yes',
    'gui.confirm.no': 'No',
    'gui.still_stopping': 'Still stopping the build...',
    'gui.details.where': 'Repository: {repo}',
    'gui.background.label': 'Picture:',
    'gui.background.choose': 'Choose picture...',
    'gui.background.remove': 'Remove',
    'gui.background.focus.left': 'Left',
    'gui.background.focus.center': 'Center',
    'gui.background.focus.right': 'Right',
    'gui.background.kinds': 'Images',
    'gui.background.saving': 'Saving the background...',
    'gui.background.unloadable': 'This image cannot be shown: {path}',
    'gui.background.kind': 'Choose a PNG, JPEG, HEIC, GIF or TIFF image.',
    'gui.background.too_big': 'This image is over 50 MB: choose a smaller one.',
    'gui.background.unreadable': 'This image could not be read: choose another one.',
    'gui.background.failed': 'The background was not changed: {error}',
    'gui.details': 'Details',
    'gui.choose_game': 'Choose a game to start.',
    'gui.section.game': 'Game',
    'gui.section.files': 'Game files',
    'gui.section.update_fonts': 'Update and system fonts',
    'gui.section.advanced': 'Advanced',
    'gui.tab.build': 'Build',
    'gui.tab.settings': 'Settings',
    'gui.tab.mods': 'Mods',
    'gui.tab.controls': 'Controls',
    'gui.tab.settings_soon': 'Start settings come here in a later version. Until then the game starts with its '
                             'automatic settings.',
    'gui.tab.mods_soon': 'Mods for Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 come here in a later '
                         'version, as toggles before the game starts.',
    'gui.not_stopped': 'The build has not stopped after {seconds} s; the builder keeps waiting for it.',
    'gui.exit_requested': 'Asked to quit: stopping the build first, then closing.',
}
EN.update(TABS)                 # the Settings and Mods tabs' texts (S5.1, S5.2), in their own table


def text(key, **values):
    """The English text for key, with values filled in. KeyError for an unknown key."""
    return EN[key].format(**values)


def version_span(versions):
    """S5.5: '1.1.0', or '1.1.0-1.2.1' for several sorted version names."""
    return versions[0] if len(versions) == 1 else f'{versions[0]}-{versions[-1]}'


def check_sentence(message_id, detail):
    """A check result's sentence. For input_rules the detail is a comma-separated list of reason ids, each with its
    own text, so the player reads what to do instead of an id (S3.4, f1)."""
    if message_id == 'input_rules' and detail:
        detail = '; '.join(text(f'reason.{reason}') for reason in detail.split(', '))
    return text(f'msg.{message_id}', detail=detail)


def cancel_report(finished):
    """T7: what a cancelled build's Finished can honestly say; never that the cancel worked."""
    if finished.termination is None:
        return text('outcome.cancelled')
    names = ', '.join(SIGNAL_NAMES.get(n, str(n)) for n in finished.signals_sent) or 'none'
    if finished.cancel_compatible:
        return text('cli.cancelled_compatible', termination=finished.termination, names=names)
    observed = finished.observed.value if finished.observed else 'unknown'
    return text('cli.cancel_not_explained', termination=finished.termination, names=names, observed=observed)
