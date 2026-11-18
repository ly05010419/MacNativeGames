"""The Mods tab of MNG Launcher: the player's own mods for the latest version of the game (tools/builder's
controller_mods, mod_library, mod_add, mod_applied, unchanged), what the old window's Mods tab showed,
in English or Chinese.

Actions (each answers the tab's state as the view below, besides apply and after_build):
  state(full=True)                 the full read imports a hand-copied folder and checks the mods and the load folder
                                   again (on opening the page and after Apply); a change reads the library quickly
  add(path)                        a mod folder or an archive (.zip, .7z, .rar); 'status' says what was added or why not
  enable(mod, on) · code(mod, on) · move(mod, step) · remove(mod) · remove_backups(names)
  apply()                          {'ok', 'message', 'lines', 'state'}: through mod_applied.apply_and_record
  after_build(ok, error='')        {'note'}: the done panel's line after a build (the mods no longer fit, or a code
                                   patch stopped it)

Rules kept from the old window (docs/builder.md): library.json has one writer at a time, so every action holds
Builder/mods-tab.lock and a second one is refused in words; every Apply goes through apply_and_record. The models
find the game's mod folder through the home folder (mod_folder.mods_folder, mod_tools.supported), so each action runs
with HOME set to the context's home: with MNG_HOME, nothing outside it is read or written."""
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path

from tools.builder import controller_mods, mod_library, mod_tools
from tools.builder.ports import CommandRunner, RealClock
from tools.builder_jobs.tabs import TabError
from tools.builder_jobs.text import t

LATEST = controller_mods.LATEST
LOCK = 'mods-tab.lock'

# What the old window said (its gui.mods.* texts, removed with the Tk window 2026-11-12), and in Chinese.
TEXT = {
    'note': ('You add the mods; the builder ships none. Only a valid mod can be switched on: its paths fit this version '
             'and it replaces a file of the game; code patches go in through a build. That does not test it in play.',
             '模组都由你自己添加，这里不带任何模组。只有有效的模组才能打开：路径符合这个版本，并且替换了游戏的至少一个文件；'
             '代码补丁要打包时合进去。有效不等于在游戏里试过。'),
    'add': ('Add mod...', '添加模组…'),
    'add_archive': ('Add archive...', '添加压缩包…'),
    'adding': ('Copying and checking the mod...', '正在复制并检查模组…'),
    'added': ('Added.', '已添加。'),
    'added_pack': ('Added {n} mods from {name}: switch on the ones you want.', '从 {name} 添加了 {n} 个模组：打开你要的那些。'),
    'switched_off': ('Switched off: {mods}. Made for another version, they cannot be switched on for 1.4.3, and the next '
                     'Apply mods takes all of each out of the game, its player pack too.',
                     '已关闭：{mods}。它们是为别的版本做的，1.4.3 上不能打开；下次“应用模组”会把它们整个从游戏里拿掉，包括玩家包。'),
    'empty': ("No mods yet. Add a mod as it came: its folder (romfs/, exefs/, or the game's own folders such as Pack/ or "
              'UI/, at most 4 levels down) or its archive (.zip, .7z, .rar).',
              '还没有模组。按下载时的样子添加：它的文件夹（romfs/、exefs/，或游戏自己的文件夹如 Pack/、UI/，最多往下 4 层），'
              '或它的压缩包（.zip、.7z、.rar）。'),
    'changed': ('The app changed since the mods were applied: Apply mods again before playing.',
                '应用模组之后 App 变了：玩之前再“应用模组”一次。'),
    'not_applied': ('Not applied: the game holds the mods of the last Apply, not these. Apply mods again.',
                    '还没应用：游戏里是上次应用的模组，不是现在这些。再“应用模组”一次。'),
    'code_pending': ('This app was built with other code mods than the ones switched on: build it again to apply them.',
                     '这个 App 打包时合进的代码模组和现在打开的不一样：重新打包才会生效。'),
    'code_line': ('Code switched on: {names}. The next full build on the Build page with the 1.4.3 update file merges it '
                  '(an express update does not).',
                  '打开了代码的模组：{names}。下一次在打包页用 1.4.3 更新文件完整打包时会合进去（极速更新不会）。'),
    'code_other_build': ('These code mods are for 1.4.3: take their code out in the Mods tab, or build with the 1.4.3 '
                         'update.', '这些代码模组是给 1.4.3 的：在模组页关掉它们的代码，或用 1.4.3 更新文件打包。'),
    'after_build': ('Your applied mods no longer fit this app: open the Mods tab and Apply mods again before playing, '
                    'or the game may stop at start.',
                    '已应用的模组和这个 App 对不上了：玩之前到模组页再“应用模组”一次，否则游戏可能一开就停。'),
    'valid': ('Valid: replaces {replaced} files, adds {added}.', '有效：替换 {replaced} 个文件，新增 {added} 个。'),
    'invalid.misspelt': ('Not valid: paths with the wrong upper or lower case: {paths}.', '无效：这些路径的大小写不对：{paths}。'),
    'invalid.only_adds': ('Not valid: it replaces no file of the game, it only adds files (is its folder one level off, '
                          'or is it for another game?).',
                          '无效：它没有替换游戏的任何文件，只是新增文件（是不是文件夹差了一层，或者是别的游戏的？）。'),
    'invalid.all_taken': ('Not valid here: everything it changes the app already replaces; it needs a build with this '
                          'mod (build_app.py --mod).',
                          '这里无效：它改的文件 App 自己都已替换；要带这个模组打包（build_app.py --mod）。'),
    'invalid.no_romfs': ('No RomFS part (romfs/) to apply: it has code patches only (exefs/).',
                         '没有可以应用的 RomFS 部分（romfs/）：它只有代码补丁（exefs/）。'),
    'invalid.no_app_romfs': ('Not checked: the app has no RomFS index for this version.', '没检查：App 里没有这个版本的 RomFS 索引。'),
    'invalid.unchecked': ('Not checked yet.', '还没检查。'),
    'invalid.other_version': ('Made for TOTK {versions}, not 1.4.3: the game would ignore its game data and size tables. '
                              'Convert it with TKMM (export for 1.4.3) and add the exported folder.',
                              '这是给王国之泪 {versions} 做的，不是 1.4.3：游戏会忽略它的游戏数据和尺寸表。'
                              '用 TKMM 转换（导出为 1.4.3），再添加导出的文件夹。'),
    'other_ignored': ('It also holds files for other versions of the game, which the game ignores.',
                      '它还带着给游戏其他版本的文件，游戏会忽略它们。'),
    'taken': ('The app replaces these itself (they need a build with this mod): {paths}',
              'App 自己替换了这些文件（要带这个模组打包）：{paths}'),
    'code.ok': ('Code patches for 1.4.3: switch on "its code" and build again to merge them.',
                '有给 1.4.3 的代码补丁：打开“代码合进下次打包”，再打包才会合进去。'),
    'code.other_build': ('Code patches for another build of the game: not usable here.', '代码补丁是给游戏另一个版本的：这里用不了。'),
    'code.not_for_game': ('Code patches (exefs/) are not supported for Breath of the Wild: only the romfs/ part is used.',
                          '旷野之息不支持代码补丁（exefs/）：只用 romfs/ 部分。'),
    'code.unchecked': ('Code patches (an .ips) whose build cannot be checked here: build_app.py checks them when it '
                       'merges them.', '代码补丁（.ips）在这里查不出是给哪个版本的：build_app.py 合进去时会检查。'),
    'code.whole_module': ('A whole replacement main (exefs/main): it cannot be merged, a patch is needed (.pchtxt or .ips).',
                          '整个替换的 main（exefs/main）：合不进去，要补丁格式（.pchtxt 或 .ips）。'),
    'code_toggle': ('Its code in the next build', '代码合进下次打包'),
    'notes.cheats_ignored': ('Its cheats/ folder is not used: MacNativeGames does not run cheat files.',
                             '它的 cheats/ 文件夹不会用：MacNativeGames 不运行金手指文件。'),
    'icons_outdated': ("Out of date: the saved controls or another mod's size table changed. Update it in the Controls tab.",
                       '过期了：保存的按键或别的模组的尺寸表变了。到按键页更新它。'),
    'conflicts': ('{n} files also replaced by another mod; the one lower in the list wins: {paths}',
                  '{n} 个文件别的模组也替换了，列表里靠下的那个生效：{paths}'),
    'refused.other_game': ('Not added: it is for another game ({detail}).', '没添加：它是别的游戏的（{detail}）。'),
    'refused.bcml_package': ('Not added: {detail} is a BCML package (.bnp), which the builder cannot merge. Install it '
                             'with BCML, or use a RomFS version of the mod.',
                             '没添加：{detail} 是 BCML 包（.bnp），这里合不了。用 BCML 安装，或者用这个模组的 RomFS 版本。'),
    'refused.tkmm_package': ('Not added: {detail} is a TKMM package. Import it in TKMM, export for 1.4.3, and add the '
                             'exported folder.', '没添加：{detail} 是 TKMM 包。在 TKMM 里导入、导出为 1.4.3，再添加导出的文件夹。'),
    'refused.several': ('Not added: it holds several mods ({detail}). Add the folder of the one you want.',
                        '没添加：里面有好几个模组（{detail}）。添加你要的那个的文件夹。'),
    'refused.too_big': ("Not added: the folder is too big to search. Pick the mod's own folder.",
                        '没添加：文件夹太大，搜不过来。选模组自己的文件夹。'),
    'refused.cheats_only': ('Not added: it only holds cheat files, which MacNativeGames does not run.',
                            '没添加：里面只有金手指文件，MacNativeGames 不运行它们。'),
    'refused.nothing': ("Not added: no romfs/ or exefs/ folder within 4 levels, and its folders are not the game's own "
                        '(Pack/, UI/...).', '没添加：往下 4 层内没有 romfs/ 或 exefs/，它的文件夹也不是游戏自己的（Pack/、UI/…）。'),
    'refused.links': ('Not added: it holds symbolic links ({detail}); copy the real files in.',
                      '没添加：里面有符号链接（{detail}）；把真正的文件复制进去。'),
    'refused.archive_kind': ('Not added: only .zip, .7z and .rar archives are read here ({detail}); extract it first.',
                             '没添加：这里只读 .zip、.7z 和 .rar（{detail}）；先解开。'),
    'refused.zip_entries': ('Not added: the archive has too many entries ({detail}).', '没添加：压缩包里的条目太多（{detail}）。'),
    'refused.zip_path': ('Not added: the archive has a path outside itself, or a file named like a folder ({detail}).',
                         '没添加：压缩包里有指向外面的路径，或和文件夹同名的文件（{detail}）。'),
    'refused.zip_link': ('Not added: the archive holds a link ({detail}).', '没添加：压缩包里有链接（{detail}）。'),
    'refused.zip_case': ('Not added: two entries have one name on a Mac disk (they differ only in upper or lower case, '
                         'or in how an accent is written): {detail}.',
                         '没添加：有两个条目在 Mac 磁盘上是同一个名字（只差大小写或重音写法）：{detail}。'),
    'refused.zip_space': ('Not added: not enough free disk space to extract {detail}.', '没添加：磁盘空间不够解开 {detail}。'),
    'refused.zip_big': ('Not added: {detail} extracts to more than 64 GB.', '没添加：{detail} 解开后超过 64 GB。'),
    'refused.zip_bad': ('Not added: the archive cannot be read ({detail}).', '没添加：压缩包读不了（{detail}）。'),
    'refused.cancelled': ('Not added: the extraction was cancelled.', '没添加：解压被取消了。'),
    'remove': ('Remove', '移除'),
    'confirm_remove': ('Remove "{name}" from the list? Only the builder\'s copy is deleted; your folder stays.',
                       '把“{name}”从列表里移除？只删这里的副本，你自己的文件夹不动。'),
    'backups': ('{n} backups of your own mod folder are kept next to it.', '你自己的模组文件夹旁边留着 {n} 个备份。'),
    'remove_backups': ('Remove them', '删掉它们'),
    'confirm_backups': ('Delete the {n} backups of your own mod folder? This cannot be undone.',
                        '删掉你自己模组文件夹的 {n} 个备份？删了就找不回来。'),
    'apply': ('Apply mods', '应用模组'),
    'applying': ('Applying the mods...', '正在应用模组…'),
    'why_not': ('Choose a built app of the latest version', '选一个最新版本的、打好的 App'),
    'applied.ok': ('Mods applied correctly.', '模组已正确应用。'),
    'applied.nothing': ('No mod is on: the mod folder was set aside, the game starts without mods.',
                        '没有打开的模组：模组文件夹已移到一边，游戏不带模组启动。'),
    'applied.set_aside': ('The mods on load nothing: the mod folder was set aside.', '打开的模组不会加载任何文件：模组文件夹已移到一边。'),
    'applied.running': ('The game is running: close it, then Apply mods.', '游戏开着：先关掉游戏，再应用模组。'),
    'applied.no_space': ('Not enough free disk space to apply these mods.', '磁盘空间不够，应用不了这些模组。'),
    'applied.install_failed': ('The mods could not be applied:', '模组没能应用：'),
    'reason.no_game_file': ('No app chosen.', '没有选 App。'),
    'reason.no_app': ('No built app here: build it first.', '这里没有打好的 App：先打包。'),
    'reason.not_latest': ('Mods here are for {name} {version} only: build that version to use them.',
                          '这里的模组只支持{name} {version}：打包这个版本才能用。'),
    'reason.not_supported': ("Mods for {name} {version} come when MacNativeGames' mod tools support it.",
                             '{name} {version} 的模组要等 MacNativeGames 的模组工具支持它。'),
    'busy': ('Another mods change is still running: try again when it ends.', '另一个模组操作还没做完：做完再试。'),
    'no_mod': ('There is no mod {mod} in the list (read the page again).', '列表里没有模组 {mod}（重新读一下这一页）。'),
    'no_path': ('{path} does not exist.', '{path} 不存在。'),
}
ARCHIVES = ('zip', '7z', 'rar')


def say(key, **values):
    return t(*TEXT[key]).format(**values)


def version_span(versions):
    """'1.1.0', or '1.1.0-1.2.1' (as the removed Tk window wrote it)."""
    return versions[0] if len(versions) == 1 else f'{versions[0]}-{versions[-1]}'


@contextmanager
def _session(ctx):
    """One writer at a time (a lock beside the old window's files), with HOME at the context's home (see above)."""
    ctx.builder_dir.mkdir(parents=True, exist_ok=True)
    fd = os.open(ctx.builder_dir / LOCK, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)
    home = os.environ.get('HOME')
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise TabError(say('busy')) from None
        os.environ['HOME'] = str(ctx.home)
        yield
    finally:
        if home is None:
            os.environ.pop('HOME', None)
        else:
            os.environ['HOME'] = home
        os.close(fd)                                # closing drops the lock


def _support(ctx):
    """What controller_mods calls data_dir: the folder that holds TOTK/, BotW/ and Builder/."""
    return ctx.data_dir.parent


def _details(mod, outdated):
    v = mod.validation
    lines = [say('valid', replaced=v.replaced, added=v.added) if v.valid else
             say(f'invalid.{v.reason}', paths=', '.join(v.misspelt[:3]),
                 versions=version_span(v.versions) if v.versions else '')]
    if v.other_ignored:
        lines.append(say('other_ignored'))
    if v.taken:
        lines.append(say('taken', paths=', '.join(v.taken[:3])))
    if v.code != 'none':
        lines.append(say(f'code.{v.code}'))
    lines += [say(f'notes.{n}') for n in mod.notes]
    if mod.id in outdated:
        lines.append(say('icons_outdated'))
    if mod.conflicts:
        lines.append(say('conflicts', n=len(mod.conflicts), paths=', '.join(mod.conflicts[:2])))
    return lines


def _mod(mod, outdated):
    includable = mod.validation.code in ('ok', 'unchecked')        # mod_code.INCLUDABLE: a build merges it
    return {'id': mod.id, 'name': mod.name, 'enabled': mod.enabled, 'valid': mod.validation.valid,
            'code': mod.code if includable else None, 'details': _details(mod, outdated),
            'bad': not (mod.validation.valid or includable), 'confirm_remove': say('confirm_remove', name=mod.name)}


def view(state):
    """The page as the old tab drew it: a reason instead of the list when the mods cannot be used here."""
    reason = state.reason and say(f'reason.{state.reason}', name=state.name, version=state.version_label)
    switched = ', '.join(f'{name} ({version_span(v)})' if v else name for name, v in state.switched_off)
    return {
        'game': state.game, 'name': state.name, 'version': state.version_label,
        'app': str(state.app) if state.app else '', 'reason': reason or '', 'checked': state.checked,
        'note': say('note'), 'empty': say('empty'),
        'changed': say('changed') if state.changed else '',
        'code_pending': say('code_pending') if state.code_pending else '',
        'not_applied': say('not_applied') if state.not_applied else '',
        'switched_off': say('switched_off', mods=switched) if switched else '',
        'code_line': say('code_line', names=', '.join(state.code_next)) if state.code_next else '',
        'mods': [] if reason else [_mod(m, state.outdated) for m in state.mods],
        'backups': {'names': list(state.player_backups), 'text': say('backups', n=len(state.player_backups)),
                    'confirm': say('confirm_backups', n=len(state.player_backups))} if state.player_backups and not reason
        else None,
        'labels': {k: say(k) for k in ('add', 'add_archive', 'adding', 'remove', 'remove_backups', 'code_toggle',
                                       'apply', 'applying', 'why_not')},
        'archives': list(ARCHIVES),
    }


def _read(ctx, full):
    return controller_mods.read_state(ctx.repo, ctx.spec, ctx.app, _support(ctx), full=full)


def _ready(ctx):
    """The quick state, refused in words when the mods cannot be used with this app (the old tab showed no list)."""
    state = _read(ctx, False)
    if state.reason:
        raise TabError(say(f'reason.{state.reason}', name=state.name, version=state.version_label))
    return state


def _known(state, mod):
    if not isinstance(mod, str) or not any(m.id == mod for m in state.mods):
        raise TabError(say('no_mod', mod=mod))


def _flag(value, name):
    if not isinstance(value, bool):
        raise TabError(f'{name} must be true or false')
    return value


def _change(ctx, action, *args):
    return controller_mods.change(ctx.repo, ctx.spec, ctx.app, _support(ctx), action, *args)


def state(ctx, full=True):
    with _session(ctx):
        return view(_read(ctx, _flag(full, 'full')))


def add(ctx, path):
    """A folder or an archive the player picked; the answer's status says what was added, or why not."""
    source = Path(str(path)).expanduser()
    if not str(path) or not source.exists():
        raise TabError(say('no_path', path=path))
    with _session(ctx):
        _ready(ctx)
        changed = _change(ctx, 'add', CommandRunner(RealClock()), source, None)
    if changed.refused:
        status = {'ok': False, 'text': say(f'refused.{changed.refused[0]}', detail=changed.refused[1])}
    elif changed.added and changed.added[1] > 1:
        status = {'ok': True, 'text': say('added_pack', n=changed.added[1], name=changed.added[0])}
    else:
        status = {'ok': True, 'text': say('added')}
    return {**view(changed), 'status': status}


def _one(ctx, action, mod, *args):
    with _session(ctx):
        _known(_ready(ctx), mod)
        return view(_change(ctx, action, mod, *args))


def enable(ctx, mod, on):
    return _one(ctx, 'enable', mod, _flag(on, 'on'))


def code(ctx, mod, on):
    return _one(ctx, 'code', mod, _flag(on, 'on'))


def move(ctx, mod, step):
    if step not in (-1, 1) or isinstance(step, bool):
        raise TabError('step must be -1 (up) or 1 (down)')
    return _one(ctx, 'move', mod, step)


def remove(ctx, mod):
    return _one(ctx, 'remove', mod)


def remove_backups(ctx, names):
    """Only the player's own backups the page listed (mod_library.remove_backups never removes another folder)."""
    if not isinstance(names, list) or not all(isinstance(n, str) for n in names):
        raise TabError('names must be a list of backup folder names')
    with _session(ctx):
        _ready(ctx)
        return view(_change(ctx, 'remove_backups', tuple(names)))


def apply(ctx):
    """Apply mods (controller_mods.apply: mod_applied.apply_and_record), then the full read, as the old tab did."""
    with _session(ctx):
        _ready(ctx)
        result = controller_mods.apply(ctx.repo, ctx.spec, ctx.app, _support(ctx))
        after = view(_read(ctx, True))
    message = say(f'applied.{result.reason or "ok"}')
    return {'ok': result.ok, 'message': message, 'lines': [] if result.ok else list(result.lines[-3:]), 'state': after}


def after_build(ctx, ok, error=''):
    """The done panel's line after a build of this app: a code patch for another build of main stopped it, or (after a
    success) the applied mods no longer fit the new app. '' when there is nothing to say."""
    if not _flag(ok, 'ok'):
        return {'note': say('code_other_build') if 'for another build of main' in str(error or '') else ''}
    with _session(ctx):
        now = _read(ctx, True)
    return {'note': say('after_build') if not now.reason and now.changed else ''}


ACTIONS = {'state': state, 'add': add, 'enable': enable, 'code': code, 'move': move, 'remove': remove,
           'remove_backups': remove_backups, 'apply': apply, 'after_build': after_build}
