"""Start options for the latest versions (S5.1, P1-P5): what each choice sets, where the choices are kept, and Play.
The model: no Tk.

Only TOTK 1.4.3 (`v143`) and BotW 1.9.0 (`botw190`). Every choice is a closed list whose values the game's own parsers
take (tests/test_builder_start_drift.py reads them). "auto" sets nothing: the game decides, as without the builder.

The choices the game's settings panel also offers (picture, interpolation, smoothing, aspect) are kept in the panel's
own one-word files beside controls.txt in the game's data folder (graphics-output.txt, graphics-frame.txt,
graphics-taa.txt, graphics-aspect.txt; runtime/settings_panel.h settings_restore_begin), so the window and the panel
show and change the same setting, a Finder start keeps it, and Play passes no variable that would override the panel
(after PR #1184: environment variables win over the panel and grey its rows out). A value the panel set that the window
has no choice for (a 72 fps lock, 40+40) reads as "auto" with a note and is left alone. Only the 16x ground filter,
which the panel does not have, is passed by Play (`open --env`) and kept in Builder/start-options.json. Files are written
only while the game is not running (the panel saves its own over them). A player who sets TOTK_CONTROLS or
TOTK_GAME_SLOT_DIR by hand moves the game's files elsewhere; the builder keeps looking at the default places."""
from dataclasses import dataclass, field, replace
import json
import os
from pathlib import Path

from tools.builder import game_state
from tools.builder.ports import Outcome

PLAY, FILE = 'play', 'file'         # how a choice reaches the game: Play's environment, or the panel's own file
OPEN_S = 30.0


@dataclass(frozen=True)
class Option:
    id: str
    how: str
    choices: tuple                  # choice ids; the first is "auto"


V143 = (Option('picture', FILE, ('auto', 'metalfx', 'native-2k')),
        Option('interpolation', FILE, ('auto', 'interp30', 'interp', 'interpmax')),
        Option('smoothing', FILE, ('auto', 'on')),
        Option('ground', PLAY, ('auto', '16')),
        Option('aspect', FILE, ('auto', '21:9')))
BOTW190 = (Option('picture', FILE, ('auto', 'metalfx')),
           Option('interpolation', FILE, ('auto', 'interp30', 'interp', 'interpmax')),
           Option('smoothing', FILE, ('auto', 'on')),
           Option('ground', PLAY, ('auto', '16')))
OPTIONS = {'v143': V143, 'botw190': BOTW190}
ENV = {('ground', '16'): {'TOTK_ANISO': '16'}}
HOLD_ENV = {'interpolation': {'TOTK_MFX': '0'}}   # a held choice: this start without it, the panel's file kept
# The panel's file and the word it holds for each choice (settings_panel.h: graphics-output.txt the OUTPUT_MODE_*
# number, graphics-frame.txt interp-<MFX_*> / lock-<fps> / off, graphics-taa.txt 0 / 1, graphics-aspect.txt the ratio).
PANEL = {'picture': ('graphics-output.txt', {'metalfx': '1', 'native-2k': '2'}),
         'interpolation': ('graphics-frame.txt', {'interp30': 'interp-1', 'interp': 'interp-3', 'interpmax': 'interp-4'}),
         'smoothing': ('graphics-taa.txt', {'on': '1'}),
         'aspect': ('graphics-aspect.txt', {'21:9': '2.3333333'})}
PICTURE_WORDS = {'metalfx': 'metalfx', 'native-2k': 'native-2k', 'native-4k': 'native-2k', 'off': 'auto'}


@dataclass(frozen=True)
class StartPaths:
    store: Path                     # Builder/start-options.json
    data: Path                      # ~/Library/Application Support/MacNativeGames (the games' folders inside)


@dataclass(frozen=True)
class StartState:
    game: str
    app: Path | None
    version: str | None
    reason: str = ''                # '' or why the tab cannot offer options: no_game_file, no_app, unknown_version
    options: tuple = ()
    choices: dict = field(default_factory=dict)
    notes: tuple = ()               # one-line notes: a broken store, a value the panel set, the game running
    name: str = ''                  # the game's display name, for the tab's title
    version_label: str = ''         # the version as players know it (1.4.3), '' when unknown


@dataclass(frozen=True)
class PlayResult:
    ok: bool
    reason: str = ''                # '' or: running, no_app, unknown_version, open_failed
    detail: str = ''
    env: dict = field(default_factory=dict)
    held: tuple = ()                # chosen options not used this start: ('interpolation',) while the GPU has not said yes


GPU_OK = ('yes', 'unknown')         # interpolation is used only after the GPU said yes, or "unknown" was shown


def picture_file(spec, paths):
    """1.4.3's own picture file (game_patches_v143.c picture_mode_save): read when the panel's file is not there."""
    return paths.data / spec.data_folder / 'picture-mode.txt'


def panel_file(spec, paths, option_id):
    return paths.data / spec.data_folder / PANEL[option_id][0]


def _load_store(path):
    try:
        data = json.loads(Path(path).read_text())
        return (data.get('games') if isinstance(data, dict) and isinstance(data.get('games'), dict) else {}), ()
    except FileNotFoundError:
        return {}, ()
    except (OSError, ValueError) as e:
        return {}, (f'start-options.json could not be read ({e.__class__.__name__}): automatic settings',)


def _write(path, content):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.partial')
    partial.write_text(content)
    os.replace(partial, path)


def read_picture(path):
    """1.4.3's picture choice from the game's file: (choice, note)."""
    try:
        word = Path(path).read_text().strip()
    except FileNotFoundError:
        return 'auto', ''
    except OSError as e:
        return 'auto', f'{path} could not be read ({e.__class__.__name__})'
    if word in PICTURE_WORDS:
        return PICTURE_WORDS[word], ''
    return 'auto', f'picture-mode.txt holds "{word[:40]}", which the game reads as off'


def _panel_word(path):
    """The panel file's word, None when there is none: (word, note)."""
    try:
        return Path(path).read_text().strip(), ''
    except FileNotFoundError:
        return None, ''
    except OSError as e:
        return None, f'{path.name} could not be read ({e.__class__.__name__})'


def read_panel(spec, paths, option):
    """One option's choice from the panel's file: (choice, note). A word the window has no choice for is the panel's
    own setting: "auto" here, with a note, and choose() leaves it alone unless another choice is made."""
    word, note = _panel_word(panel_file(spec, paths, option.id))
    if word is None:
        if option.id == 'picture' and spec.data_folder == 'TOTK':   # 1.4.3: the game's own picture-mode.txt
            return read_picture(picture_file(spec, paths))
        return 'auto', note
    words = PANEL[option.id][1]
    choice = next((c for c, w in words.items() if w == word), None)
    if choice in option.choices:
        return choice, ''
    if word in ('', '0', 'off'):
        return 'auto', ''
    return 'auto', f'{option.id}: the game\'s settings panel set "{word[:40]}" ({PANEL[option.id][0]}), kept'


def read_state(spec, app, paths):
    state = _read_state(spec, app, paths)
    label = next((v.display for v in spec.versions if v.key == state.version), '')
    return replace(state, name=spec.display_name, version_label=label)


def _read_state(spec, app, paths):
    if app is None:
        return StartState(spec.id, None, None, 'no_game_file')
    version = game_state.app_version(app)
    if not Path(app).is_dir():
        return StartState(spec.id, app, None, 'no_app')
    if version not in OPTIONS:
        return StartState(spec.id, app, version, 'unknown_version')
    games, notes = _load_store(paths.store)
    saved = games.get(spec.id) if isinstance(games.get(spec.id), dict) else {}
    choices = {}
    for option in OPTIONS[version]:
        if option.how == FILE:
            choices[option.id], note = read_panel(spec, paths, option)
            notes += (note,) if note else ()
        else:
            value = saved.get(option.id, 'auto')
            choices[option.id] = value if value in option.choices else 'auto'
    return StartState(spec.id, app, version, '', OPTIONS[version], choices, notes)


def choose(spec, app, paths, option_id, choice):
    """Keep one choice; the new state. ValueError for an option or choice outside the table."""
    state = read_state(spec, app, paths)
    option = next((o for o in state.options if o.id == option_id), None)
    if option is None or choice not in option.choices:
        raise ValueError(f'no choice {choice!r} for {option_id!r} on {state.version}')
    if option.how == FILE:
        if game_state.game_running(paths.data / spec.data_folder):
            return _with_note(read_state(spec, app, paths), 'running')
        target = panel_file(spec, paths, option_id)
        word, _ = _panel_word(target)
        if choice != 'auto':
            _write(target, PANEL[option_id][1][choice] + '\n')
        elif word in PANEL[option_id][1].values():   # only a value the window wrote; the panel's own one stays
            target.unlink(missing_ok=True)
        if option_id == 'picture' and spec.data_folder == 'TOTK' and (choice != 'auto' or word in (None, *PANEL['picture'][1].values())):
            picture_file(spec, paths).unlink(missing_ok=True)   # the game writes it again from the panel's choice
    else:
        games, _ = _load_store(paths.store)
        mine = dict(games.get(spec.id) or {})
        if choice == 'auto':
            mine.pop(option_id, None)
        else:
            mine[option_id] = choice
        games = {**games, spec.id: mine}
        _write(paths.store, json.dumps({'format': 1, 'games': games}, indent=2) + '\n')
    return read_state(spec, app, paths)


def _with_note(state, note):
    return replace(state, notes=state.notes + (note,))


def held_back(state, gpu):
    """Chosen options Play must not pass now: interpolation unless the GPU said yes or the warning for "unknown" was
    shown (while it is still being checked, or after "no", the game could stop at start)."""
    chosen = state.choices.get('interpolation', 'auto') != 'auto'
    return ('interpolation',) if chosen and gpu not in GPU_OK else ()


def env_for(state, gpu):
    """The environment Play passes: the PLAY choices that are not auto, and for a held FILE choice the variable that
    starts the game without it this time (the panel's file is kept for the next start)."""
    env, held = {}, held_back(state, gpu)
    for option in state.options:
        choice = state.choices.get(option.id, 'auto')
        if option.id in held:
            env.update(HOLD_ENV[option.id])
        elif option.how == PLAY and choice != 'auto':
            env.update(ENV[(option.id, choice)])
    return env


def play(runner, spec, app, paths, gpu):
    state = read_state(spec, app, paths)
    if state.reason:
        return PlayResult(False, state.reason)
    if game_state.game_running(paths.data / spec.data_folder):
        return PlayResult(False, 'running')
    env = env_for(state, gpu)
    argv = ['/usr/bin/open'] + [a for k, v in sorted(env.items()) for a in ('--env', f'{k}={v}')] + [str(app)]
    result = runner.run(argv, timeout_s=OPEN_S)
    if result.outcome is not Outcome.OK:
        return PlayResult(False, 'open_failed', (result.stderr or result.outcome.name).strip()[-300:], env)
    return PlayResult(True, env=env, held=held_back(state, gpu))
