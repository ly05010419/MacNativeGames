"""The game of a game card (XCI) or ticketless package, read with the player's keys before the build, the way
build_app.py does at its start (games/totk/scripts/identify_game.py: the title IDs in the NCA headers). It also proves
the keys open the game's headers, so keys that cannot are said before a 30-minute build, not in it.

Contract: identify(path, keys) returns (title or None, problem or None) and never raises for a bad file or keys. The
headers need the cryptography package; without it the answer is (None, None) and build_app.py tells the game later.
Nothing from the keys is returned or printed."""
from pathlib import Path
import sys

from tools.builder_jobs import repo
from tools.builder_jobs.text import t

SCRIPTS = repo.REPO / 'games/totk/scripts'


def _reader():
    if str(SCRIPTS) not in sys.path:
        sys.path.insert(0, str(SCRIPTS))
    try:
        import extract_exefs
    except ImportError:   # no cryptography in this Python
        return None
    return extract_exefs


def identify(path, keys):
    reader = _reader()
    if reader is None:
        return None, None
    path = Path(path)
    try:
        loaded = reader.load_keys(Path(keys))
    except (OSError, ValueError) as e:
        return None, t(f'the keys file cannot be read: {e}', f'keys 文件读不了：{e}')
    if 'header_key' not in loaded:
        return None, t('the keys file has no header_key: dump prod.keys again from your Switch',
                       'keys 文件没有 header_key：从 Switch 重新导出 prod.keys')
    try:
        with path.open('rb') as f:
            _, entries = reader.container_entries(f, path.stat().st_size)
            records = reader.nca_inventory(f, entries, loaded)
    except Exception as e:   # the reader raises its own errors (require) as well as OSError and struct errors
        return None, t(f'these keys cannot read {path.name} ({e}): dump prod.keys again from your Switch, with its '
                       f'newest system version', f'这份 keys 读不了 {path.name}（{e}）：把 Switch 升到最新系统后重新导出 prod.keys')
    titles = sorted({r['title_id'] for r, _ in records})
    games = [title for title in titles if repo.game_of_title(title)]
    if not games:
        found = ', '.join(titles) or t('no game', '没有游戏')
        return None, t(f'{path.name} holds {found}, not a game this repository builds', f'{path.name} 里是 {found}，不是本项目能打包的游戏')
    missing = missing_generation(records, loaded)
    if missing is not None:
        return games[0], t(f'these keys are older than {path.name}: it needs key generation {missing:02x} '
                           f'(titlekek_{missing:02x}). Dump prod.keys again from a Switch with a newer system version',
                           f'这份 keys 比 {path.name} 旧：它要第 {missing:02x} 代密钥（titlekek_{missing:02x}）。把 Switch 升级系统后重新导出 prod.keys')
    return games[0], None


def missing_generation(records, keys):
    """The newest key generation (0-based) an NCA needs that the keys lack, or None. An NCA's generation is the larger
    of its two header fields (0x206, 0x220), 0 and 1 both meaning the first; a ticketed NCA needs titlekek_<gen>, a
    ticketless one key_area_key_application_<gen>. Either name is enough here: one dump holds both or neither."""
    need = set()
    for _, hdr in records:
        gen = max(hdr[0x206], hdr[0x220])
        need.add(gen - 1 if gen else 0)
    for gen in sorted(need, reverse=True):
        if f'titlekek_{gen:02x}' not in keys and f'key_area_key_application_{gen:02x}' not in keys:
            return gen
    return None
