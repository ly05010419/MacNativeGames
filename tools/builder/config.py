"""The builder's saved settings: per game, the paths the player chose, and the last successful build (S1.6).

Only paths and facts are stored, never key or game contents. load() never raises for anything on disk and keeps every
key it does not know, so a newer builder's data survives. save() is atomic and never leaves settings.json missing; a
file load() found broken is kept as a hard-linked backup."""
from dataclasses import dataclass, field
import errno
import json
import os
from pathlib import Path
import tempfile
import time

from tools.builder.games import GAMES

FORMAT = 1
PATH_FIELDS = ('game', 'keys', 'update', 'firmware', 'shared_fonts', 'work', 'out')
LAST_BUILD_FIELDS = ('game', 'version', 'app', 'commit', 'finished_at')
TOP_LEVEL = ('format', 'games', 'last_build')
BACKUP_NAMES = 100


@dataclass(frozen=True)
class SavedPaths:
    game: str | None = None
    keys: str | None = None
    update: str | None = None
    firmware: str | None = None
    shared_fonts: str | None = None
    work: str | None = None
    out: str | None = None
    extra: dict = field(default_factory=dict)           # unknown keys of this game entry, verbatim


@dataclass(frozen=True)
class LastBuild:
    game: str
    version: str
    app: str
    commit: str
    finished_at: str                                    # ISO-8601, from the clock's wall()
    extra: dict = field(default_factory=dict)


@dataclass(frozen=True)
class Settings:
    paths: dict = field(default_factory=dict)           # known game id -> SavedPaths
    extra_games: dict = field(default_factory=dict)     # unknown game ids, verbatim
    last_build: LastBuild | None = None
    extra: dict = field(default_factory=dict)           # unknown top-level keys, verbatim
    format: int = FORMAT


@dataclass(frozen=True)
class LoadResult:
    settings: Settings
    existed: bool
    problems: tuple = ()


def default_settings_path(home):
    return Path(home) / 'Library/Application Support/MacNativeGames/Builder/settings.json'


def _read_bytes(path):
    """The file's bytes; patched by tests to simulate read errors."""
    return Path(path).read_bytes()


def to_json(settings):
    games = dict(settings.extra_games)
    for game_id, saved in settings.paths.items():
        games[game_id] = {**saved.extra, **{f: getattr(saved, f) for f in PATH_FIELDS}}
    last = settings.last_build
    return {**settings.extra, 'format': settings.format, 'games': games,
            'last_build': None if last is None else {**last.extra, **{f: getattr(last, f) for f in LAST_BUILD_FIELDS}}}


def from_json(data, problems):
    """Settings from a JSON object; each invalid part is dropped alone and named in problems (C2)."""
    version = data.get('format')
    if type(version) is not int or version < 1:
        problems.append('format_invalid')
        version = FORMAT
    elif version > FORMAT:
        problems.append('format_newer')
    paths, extra_games = {}, {}
    games = data.get('games', {})
    if not isinstance(games, dict):
        problems.append('games_invalid')
        games = {}
    for game_id, entry in games.items():
        if game_id not in GAMES:
            extra_games[game_id] = entry
        elif not isinstance(entry, dict):
            problems.append(f'game_invalid:{game_id}')
        else:
            paths[game_id] = _saved_paths(game_id, entry, problems)
    return Settings(paths, extra_games, _last_build(data.get('last_build'), problems),
                    {k: v for k, v in data.items() if k not in TOP_LEVEL}, version)


def _saved_paths(game_id, entry, problems):
    values = {}
    for f in PATH_FIELDS:
        value = entry.get(f)
        if value is not None and not isinstance(value, str):
            problems.append(f'field_invalid:{game_id}.{f}')
            value = None
        values[f] = value
    return SavedPaths(**values, extra={k: v for k, v in entry.items() if k not in PATH_FIELDS})


def _last_build(value, problems):
    if value is None:
        return None
    if not isinstance(value, dict) or not all(isinstance(value.get(f), str) for f in LAST_BUILD_FIELDS):
        problems.append('last_build_invalid')
        return None
    return LastBuild(**{f: value[f] for f in LAST_BUILD_FIELDS},
                     extra={k: v for k, v in value.items() if k not in LAST_BUILD_FIELDS})


class SettingsStore:
    def __init__(self, path, clock):
        self.path = Path(path)
        self.clock = clock
        self._broken = False        # the last load() could not read the file at all (C2 read stage)
        self._backup = None         # the backup made for that broken file, once made

    def load(self) -> LoadResult:
        try:
            raw = _read_bytes(self.path)
        except FileNotFoundError:
            self._broken, self._backup = False, None
            return LoadResult(Settings(), False, ())
        except OSError:
            return self._broken_file('unreadable')
        try:
            data = json.loads(raw.decode('utf-8'))
        except UnicodeDecodeError:
            return self._broken_file('not_utf8')
        except (ValueError, RecursionError):        # JSONDecodeError, an integer over the digit limit, deep nesting
            return self._broken_file('invalid_json')
        if not isinstance(data, dict):
            return self._broken_file('not_object')
        self._broken, self._backup = False, None
        problems = []
        return LoadResult(from_json(data, problems), True, tuple(problems))

    def _broken_file(self, problem):
        self._broken = True
        return LoadResult(Settings(), True, (problem,))

    def save(self, settings):
        """Temporary file written and fsynced, then a backup of a broken file (hard link), then os.replace."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=self.path.parent, prefix='.settings-',
                                          suffix='.tmp', delete=False)
        try:
            with tmp:
                json.dump(to_json(settings), tmp, indent=2, sort_keys=True, ensure_ascii=False)
                tmp.write('\n')
                tmp.flush()
                os.fsync(tmp.fileno())
            if self._broken and self._backup is None:
                self._backup = self._link_backup()
            os.replace(tmp.name, self.path)
        except BaseException:
            try:
                os.unlink(tmp.name)
            except FileNotFoundError:
                pass
            raise
        self._broken, self._backup = False, None

    def _link_backup(self):
        stamp = time.strftime('%Y%m%d-%H%M%S', time.localtime(self.clock.wall()))
        base = f'{self.path.name}.broken-{stamp}'
        for n in range(1, BACKUP_NAMES + 1):
            target = self.path.with_name(base if n == 1 else f'{base}-{n}')
            try:
                os.link(self.path, target)          # never overwrites: FileExistsError if the name is taken
                return target
            except FileExistsError:
                continue
        raise FileExistsError(errno.EEXIST, f'no free backup name for {base} (tried {BACKUP_NAMES})')
