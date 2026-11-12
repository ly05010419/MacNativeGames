"""The Controls tab of MNG Launcher: the old window's Controls page (tools/builder/gui_controls*.py) over the same models,
unchanged: controller_controls (read, set_answer, save, restore), controls_file (swap, the Xbox letters preset),
controller_icons and controller_mods.install_icons (the button icons of Tears of the Kingdom and Breath of the Wild), and
keyboard_layout (the key test). The files are the game's: <data>/controls.txt and controller-type.txt, keyboard.txt, and
Builder/controls-tab.json for the controller answered with type 'auto'.

The window keeps no state but what the last answer gave it: `token` (what the files said when it was read) and the
pending layout (`desired`, drawn button -> Switch input or None, and `converted`, the custom buttons the player agreed
to make plain). Every action reads the files again; a token that no longer matches drops the pending layout and says so
(as the old page's 'changed'), and Save and Restore are refused for it.

Actions: state(token, desired, converted) -> the view; answer(controller) -> the view; swap(token, desired, converted,
input, button, confirmed) and xbox_letters(token, desired, converted, confirmed) -> the view, with `confirm` (words to ask)
when custom buttons would be converted and confirmed is false; save(token, desired, converted) and restore(token) -> the
view with `message` and `ok`; icons_info() -> the icons line; icons(install_env, switch_off) -> made and applied (minutes:
the fontTools download the first time, then the generator and Apply); keyboard() -> the layout and the key test's table."""
import hashlib
import json
from pathlib import Path
import re
import threading

from tools.builder import controller_controls as cc, controller_icons as ci, controller_mods as cm
from tools.builder import controls_file as cf, keyboard_layout as kb
from tools.builder.gui_controls_table import NAMES, ORDER, SWITCH_LABELS, XBOX, button_for
from tools.builder.strings import EN
from tools.builder_jobs.tabs import TabError
from tools.builder_jobs.text import t

KEYMAP = 'games/totk/runtime/keyboard_map.h'
KEYMAP_INPUT = re.compile(r'\{(\d+), "([a-z0-9_]+)", "')

# The old page's texts (tools/builder/strings.py gui.controls.*, read from there in English) in Chinese.
ZH = {
    'gui.controls.title': '{name} · 按键',
    'gui.controls.not_offered': '这个游戏的按键还不能在这里改。',
    'gui.controls.note': '先点左边的动作，再点手柄上的按键。每个动作就是游戏里以它的主要用途命名的那个键：移动它，这个键在别处的用途'
                         '（菜单、骑马、盾牌……）也一起移动。',
    'gui.controls.pick': '先点左边的动作，再点手柄上的按键。',
    'gui.controls.current': '{name}：现在是 {button}。',
    'gui.controls.pending': '{name} → {button}（未保存）。',
    'gui.controls.drives': '{button} 现在是 {name}。',
    'gui.controls.unconfirmed': '{button}（用途待确认）',
    'gui.controls.none': '没有按键',
    'gui.controls.which': '你用哪种手柄？游戏对 Xbox 和 PlayStation 手柄按位置读取正面按键，对任天堂手柄按标签读取（按 6 选的类型是“自动”）。',
    'gui.controls.answer.xbox': 'Xbox 或 PlayStation',
    'gui.controls.answer.nintendo': '任天堂',
    'gui.controls.mode.xbox': '手柄类型（按 6）：Xbox，正面按键按位置。在游戏里改类型会让这套布局的正面按键跟着变。',
    'gui.controls.mode.playstation': '手柄类型（按 6）：PlayStation，正面按键按位置。在游戏里改类型会让这套布局的正面按键跟着变。',
    'gui.controls.mode.switch': '手柄类型（按 6）：Switch，正面按键按标签（A 键就是 A）：图上的 A、B、X、Y 就是你手柄上印的字母，'
                                '不管它们在哪个位置。在游戏里改类型会让这套布局的正面按键跟着变。',
    'gui.controls.mode.auto.xbox': '手柄类型（按 6）：自动，按 Xbox 或 PlayStation 手柄读取（按位置）。手动设置的 TOTK_FACE_BUTTONS 优先。',
    'gui.controls.mode.auto.nintendo': '手柄类型（按 6）：自动，按任天堂手柄读取（按标签）：图上的 A、B、X、Y 就是你手柄上印的字母，'
                                       '不管它们在哪个位置。手动设置的 TOTK_FACE_BUTTONS 优先。',
    'gui.controls.link': 'controls.txt 是一个链接：保存时写到它指向的文件。',
    'gui.controls.broken': 'controls.txt 里有游戏不接受的一行（第 {line} 行：{error}）；这样游戏启动不了。“恢复默认”会把它留作备份。',
    'gui.controls.custom.tap': '点按', 'gui.controls.custom.toggle': '切换', 'gui.controls.custom.aim': '瞄准',
    'gui.controls.custom.none': '不用', 'gui.controls.custom.shared': '共用',
    'gui.controls.convert': '把 {buttons} 改成普通按键吗？你手动给它们设的（模式或“不用”）保存时会去掉；'
                            '别的按键用其中一个开火的瞄准行会保留。',
    'gui.controls.saving': '正在保存……',
    'gui.controls.saved': '已保存。游戏下次启动时读取。旧文件保留为 {backup}。',
    'gui.controls.no_backup': '无（原来没有）',
    'gui.controls.not_saved.other_game': '没有保存：现在选的是另一个游戏。已重新读取它的按键。',
    'gui.controls.not_saved.running': '没有保存：游戏开着。先关掉游戏再保存。',
    'gui.controls.not_saved.changed': '没有保存：controls.txt 或手柄类型在这期间变了。看一下新的布局再保存。',
    'gui.controls.not_saved.refused': '没有保存：新文件不会是你选的样子（{detail}）。',
    'gui.controls.confirm_restore': '把 controls.txt 移到一边（留作备份），使用游戏的默认布局吗？',
    'gui.controls.preset.xbox': 'Xbox 字母',
    'gui.controls.preset.xbox_note': 'A 做游戏里叫 A 的事，B 做叫 B 的事，X、Y 也一样：游戏提示里的字母就是要按的键。',
    'gui.controls.preset.done': 'Xbox 字母：A、B、X、Y 现在做游戏里用这些字母表示的事（未保存）。',
    'gui.controls.preset.same': 'A、B、X、Y 已经在做游戏里用这些字母表示的事。',
    'gui.controls.icons.update': '更新游戏的按键图标',
    'gui.controls.icons.none': '游戏提示里还是 Switch 的按键。更新后显示这套布局里做每件事的 Xbox 按键（只按保存过的布局）。',
    'gui.controls.icons.outdated': '游戏的按键图标和保存的布局（或其他 mod 的大小表）对不上了：请更新。',
    'gui.controls.icons.current': '游戏的按键图标显示的是保存的布局里的按键。',
    'gui.controls.icons.shared': '有两个键都做 {inputs}：提示里显示第一个。',
    'gui.controls.icons.not.not_offered': '目前只有王国之泪和旷野之息能更新游戏的按键图标。',
    'gui.controls.icons.not.no_app': '按键图标：先打包 App。',
    'gui.controls.icons.not.not_latest': '按键图标：只支持旷野之息 1.9.0（这个 App 是别的版本）。',
    'gui.controls.icons.not.ask': '按键图标：先说你用哪种手柄。',
    'gui.controls.icons.not.nintendo': '按键图标：用任天堂手柄（或按 6 选了 Switch）时保留游戏自己的图标；这里画的是 Xbox 的名字。',
    'gui.controls.icons.not.broken_file': '按键图标：先修好或恢复 controls.txt。',
    'gui.controls.icons.download.missing': '画图标需要 fontTools 4.67.0，从 pypi.org 下载（按固定的 SHA-256 核对）到打包工具自己的 '
                                           'Python 环境（Builder/icons-venv）。现在下载吗？',
    'gui.controls.icons.download.broken': '打包工具画图标用的 Python 环境不能用了（可能是 Python 升级了）。重新建一个吗？会从 pypi.org '
                                          '下载 fontTools 4.67.0（固定的 SHA-256）。',
    'gui.controls.icons.switch_off': '{mods} 也替换游戏的字体文件（或旷野之息的 Pack/Bootup.pack），两者会冲突。关掉它们吗（它们还留在你的列表里）？',
    'gui.controls.icons.kept': '没有更新图标：{mods} 仍在替换同样的字体。',
    'gui.controls.icons.making': '正在画按键图标（王国之泪第一次会先下载 fontTools）；然后像 Mod 页一样对整个列表应用 mod……',
    'gui.controls.icons.done': '按键图标已更新，mod 已应用：游戏下次启动时显示。',
    'gui.controls.icons.failed': '按键图标没有更新（{reason}）：{detail}',
    'gui.controls.restore': '恢复默认',
    'gui.controls.cancel': '取消改动',
    'gui.controls.save': '保存按键',
    'gui.controls.test.title': '按键测试',
    'gui.controls.test.note': '按下键盘上的键，看它在这个游戏里做什么。测试只读这个窗口：系统快捷键照常可用，什么都不会发给游戏。',
    'gui.controls.test.start': '开始按键测试',
    'gui.controls.test.stop': '停止按键测试',
    'gui.controls.test.stopped': '没有在测试。',
    'gui.controls.test.running': '测试中：按一个键（Esc 也会停止）。',
    'gui.controls.test.none': '请按一个键……',
    'gui.controls.test.key': '按下：{key}',
    'gui.controls.test.modifiers': '按下：{modifiers}+{key}',
    'gui.controls.test.char': '字符 {char}',
    'gui.controls.test.code': '键码 {code}',
    'gui.controls.test.actions': '{key} -> {actions}',
    'gui.controls.test.unbound': '{key} -> 没有绑定',
    'gui.controls.test.unread': '布局读不了：{detail}',
    'gui.controls.test.defaults': '这个游戏还没有 keyboard.txt：显示的是默认布局。',
}
# The rows' and buttons' names (gui_controls_table, keyboard_layout.TITLES) in Chinese.
ZH_NAMES = {'Action': '动作', 'Put Away': '收起武器', 'Jump': '跳跃', 'Attack': '攻击', 'Focus': '注目',
            'Throw Weapon': '投掷武器', 'Purah Pad': '普尔亚平板', 'Pause Menu': '暂停菜单', 'Crouch': '蹲下',
            'Cancel / Sprint': '取消 / 冲刺', 'Target': '注目', 'Bow': '弓', 'Rune': '希卡之石', 'Map': '地图',
            'Look': '主观视角', 'Left stick press': '按下左摇杆', 'Right stick press': '按下右摇杆',
            'D-pad up': '十字键上', 'D-pad down': '十字键下', 'D-pad left': '十字键左', 'D-pad right': '十字键右',
            'Menu': '菜单键', 'View': '视图键', 'A (confirm)': 'A（确认）', 'B (cancel)': 'B（取消）',
            'X (jump)': 'X（跳跃）', 'Y (attack)': 'Y（攻击）', 'Left stick': '左摇杆（按下）',
            'Right stick': '右摇杆（按下）', 'Plus (menu)': '+（菜单）', 'Minus (map)': '−（地图）',
            'Move up': '向前移动', 'Move down': '向后移动', 'Move left': '向左移动', 'Move right': '向右移动',
            'Camera up': '镜头向上', 'Camera down': '镜头向下', 'Camera left': '镜头向左', 'Camera right': '镜头向右',
            'Screenshot': '截图'}
KEYBOARD_WHERE = (t("Change the keys in the game's settings panel (key 1) → Key bindings; it writes keyboard.txt.",
                    '在游戏里的设置面板（按 1）→“按键绑定”里改键盘；它写 keyboard.txt。'))


def say(text_id, /, **values):
    """The old page's English text (strings.EN, which strings.text reads; its 'key' argument would clash with the key
    test's {key}), or its Chinese (MNG_LANG)."""
    return t(EN[text_id], ZH[text_id]).format(**values)


def word(name):
    return t(name, ZH_NAMES.get(name, name))


def files(ctx):
    """(the game's data folder, Builder/controls-tab.json, the game): controller_controls' leading arguments."""
    return ctx.data_dir, ctx.builder_dir / 'controls-tab.json', ctx.spec.id


def fingerprint(state):
    """What the files said: controls.txt, controller-type.txt and the controller answered (by_label)."""
    digest = lambda b: None if b is None else hashlib.sha256(b).hexdigest()   # noqa: E731
    return hashlib.sha256(json.dumps([digest(state.raw), digest(state.type_raw), state.by_label]).encode()).hexdigest()[:32]


def _desired(state, desired, converted):
    """The pending layout the window sent, checked against the drawn buttons and the Switch inputs."""
    if desired is None:
        return dict(state.inputs), set()
    if not isinstance(desired, dict) or set(desired) != set(cf.DRAWN) or \
            any(v is not None and v not in cf.SWITCH for v in desired.values()):
        raise ValueError(f'desired must give each of {", ".join(cf.DRAWN)} a Switch input or null')
    if not set(converted or ()) <= set(cf.DRAWN):
        raise ValueError('converted names a button that is not drawn')
    return dict(desired), set(converted or ())


def current(ctx, token=None, desired=None, converted=()):
    """(the state read now, the pending layout, a note): a token of other files drops the pending layout."""
    state = cc.read_state(*files(ctx))
    if token and token != fingerprint(state):
        return state, dict(state.inputs), set(), say('gui.controls.not_saved.changed')
    if not state.inputs:                    # asking which controller, or a file the game refuses: nothing to edit
        return state, {}, set(), ''
    return state, *_desired(state, desired, converted), ''


def _name(game, switch_input):
    if switch_input is None:
        return say('gui.controls.none')
    name = NAMES[game][switch_input]
    return word(name) if name else say('gui.controls.unconfirmed', button=word(SWITCH_LABELS[switch_input]))


def _button(drawn):
    return word(XBOX[drawn]) if drawn else say('gui.controls.none')


def _custom(state, converted, drawn):
    why = state.custom.get(drawn) if drawn and drawn not in converted else None
    return say(f'gui.controls.custom.{why}') if why else ''


def _mode(state):
    if state.reason or state.error or state.by_label is None:
        return ''
    key = f'gui.controls.mode.{state.type_word}' + (f'.{state.answer}' if state.type_word == 'auto' else '')
    return say(key) + (' ' + say('gui.controls.link') if state.link else '')


def view(ctx, state, desired, converted, message='', ok=True, confirm='', convert=()):
    game = state.game
    rows = []
    for switch_input in ORDER if state.inputs else ():
        drawn, before = button_for(desired, switch_input), button_for(state.inputs, switch_input)
        custom = _custom(state, converted, drawn)
        rows.append({'input': switch_input, 'name': _name(game, switch_input), 'button': drawn,
                     'shown': ('→ ' if drawn != before else '') + _button(drawn) + (' · ' + custom if custom else ''),
                     'changed': drawn != before, 'custom': custom,
                     'caption': say('gui.controls.current', name=_name(game, switch_input), button=_button(drawn))})
    buttons = [{'id': d, 'label': XBOX[d], 'input': desired.get(d), 'custom': _custom(state, converted, d),
                'caption': say('gui.controls.drives', button=_button(d), name=_name(game, desired.get(d)))}
               for d in cf.DRAWN] if state.inputs else []
    texts = {k.replace('.xbox', '').replace('.', '_'): say(f'gui.controls.{k}') for k in
             ('note', 'pick', 'which', 'restore', 'cancel', 'save', 'saving', 'confirm_restore', 'preset.xbox',
              'preset.xbox_note')}
    return {'game': game, 'title': say('gui.controls.not_offered') if state.reason else
            say('gui.controls.title', name=ctx.spec.display_name),
            'token': fingerprint(state), 'mode': _mode(state), 'which': state.by_label is None and not state.error,
            'ask': not state.reason and not state.error and (state.by_label is None or state.type_word == 'auto'),
            'answers': [{'id': a, 'label': say(f'gui.controls.answer.{a}')} for a in cf.ANSWERS], 'answer': state.answer,
            'broken': say('gui.controls.broken', line=state.error[0], error=state.error[1]) if state.error else '',
            'rows': rows, 'buttons': buttons, 'desired': desired, 'converted': sorted(converted),
            'pending': bool(state.inputs) and (desired != state.inputs or bool(converted)),
            'on_disk': state.raw is not None or state.link, 'texts': texts,
            'message': message, 'ok': ok, 'confirm': confirm, 'convert': list(convert)}


def state(ctx, token=None, desired=None, converted=()):
    s, d, c, note = current(ctx, token, desired, converted)
    return view(ctx, s, d, c, note, not note)


def answer(ctx, controller):
    s = cc.set_answer(*files(ctx), controller)
    return view(ctx, s, dict(s.inputs), set())


def _custom_moved(state, converted, moved):
    custom = [d for d in moved if d and d in state.custom and d not in converted]
    what = ', '.join(f'{XBOX[d]} ({say("gui.controls.custom." + state.custom[d])})' for d in custom)
    return custom, say('gui.controls.convert', buttons=what) if custom else ''


def swap(ctx, token, desired, converted, input, button, confirmed=False):
    """A row (input) given to a drawn button: the swap of controls_file; custom buttons converted once confirmed."""
    s, d, c, note = current(ctx, token, desired, converted)
    if note or not s.inputs:
        return view(ctx, s, d, c, note, False)
    if input not in ORDER or button not in cf.DRAWN:
        raise ValueError(f'unknown input {input!r} or button {button!r}')
    custom, ask = _custom_moved(s, c, (button, button_for(d, input)))
    if custom and not confirmed:
        return view(ctx, s, d, c, confirm=ask, convert=custom)
    d, c = cf.swap(d, input, button), c | set(custom)
    return view(ctx, s, d, c, say('gui.controls.pending', name=_name(s.game, input), button=_button(button)))


def xbox_letters(ctx, token, desired, converted, confirmed=False):
    """The preset: A, B, X and Y on the buttons with those letters (shown, not saved)."""
    s, d, c, note = current(ctx, token, desired, converted)
    if note or not s.inputs:
        return view(ctx, s, d, c, note, False)
    target = cf.xbox_letters(d)
    moved = [b for b in cf.DRAWN if target[b] != d[b]]
    custom, ask = _custom_moved(s, c, moved)
    if custom and not confirmed:
        return view(ctx, s, d, c, confirm=ask, convert=custom)
    return view(ctx, s, target, c | set(custom), say('gui.controls.preset.done' if moved else 'gui.controls.preset.same'))


def _written(ctx, result, kept):
    """The view after Save or Restore: the files read again; a refused one keeps the pending layout when it can."""
    message = say('gui.controls.saved', backup=result.backup or say('gui.controls.no_backup')) if result.ok else \
        say(f'gui.controls.not_saved.{result.reason}', detail=result.detail)
    s = cc.read_state(*files(ctx))
    if not result.ok and kept and kept[0] == fingerprint(s):
        return view(ctx, s, kept[1], kept[2], message, False)
    return view(ctx, s, dict(s.inputs), set(), message, result.ok)


def save(ctx, token, desired, converted=()):
    s, d, c, note = current(ctx, token, desired, converted)
    if note:
        return view(ctx, s, d, c, note, False)
    return _written(ctx, cc.save(*files(ctx), s, d, tuple(c)), (token, d, c))


def restore(ctx, token):
    s = cc.read_state(*files(ctx))
    if token != fingerprint(s):
        return view(ctx, s, dict(s.inputs), set(), say('gui.controls.not_saved.changed'), False)
    return _written(ctx, cc.restore(*files(ctx), s), None)


def _icons_args(ctx):
    data_dir, store, _ = files(ctx)
    return data_dir, store, ctx.repo, ctx.spec, ctx.app, ctx.data_dir.parent, ctx.builder_dir


def icons_view(info):
    names = ', '.join(name for _, name in info.font_mods)
    if info.offered:
        line = say(f'gui.controls.icons.{info.status}')
        line += ' ' + say('gui.controls.icons.shared', inputs=', '.join(info.shared)) if info.shared else ''
    else:
        line = say(f'gui.controls.icons.not.{info.reason}')
    return {'offered': info.offered, 'reason': info.reason, 'line': line, 'outdated': info.status == 'outdated',
            'button': say('gui.controls.icons.update') if info.reason != 'not_offered' else '',
            'env': info.env, 'download': say(f'gui.controls.icons.download.{info.env}') if info.env != 'ready' else '',
            'font_mods': [m for m, _ in info.font_mods],
            'switch_off': say('gui.controls.icons.switch_off', mods=names) if info.font_mods else '',
            'kept': say('gui.controls.icons.kept', mods=names) if info.font_mods else '',
            'making': say('gui.controls.icons.making')}


def icons_info(ctx):
    return icons_view(ci.icons_info(*_icons_args(ctx)))


def icons(ctx, install_env=False, switch_off=()):
    """Makes the icons mod (the venv first when install_env), installs it in the mod library and applies the mods, as
    the old page did through its Mods part. Writes only the game's folders of this home: refused when the load folder
    (mod_folder.mods_folder, from $HOME) is elsewhere."""
    info = ci.icons_info(*_icons_args(ctx))
    if not info.offered:
        return {'ok': False, 'message': icons_view(info)['line'], 'icons': icons_view(info)}
    if info.env != 'ready' and not install_env:
        return {'ok': False, 'message': icons_view(info)['download'], 'icons': icons_view(info)}
    if set(m for m, _ in info.font_mods) - set(switch_off):
        return {'ok': False, 'message': icons_view(info)['kept'], 'icons': icons_view(info)}
    load = cm._library(ctx.repo, ctx.spec, cm.LATEST[ctx.spec.id], ctx.data_dir.parent).load
    if not Path(load).is_relative_to(ctx.home):
        raise TabError(t(f"the game's mods folder {load} is not under {ctx.home} (set HOME as MNG_HOME)",
                         f'游戏的 mod 文件夹 {load} 不在 {ctx.home} 下（HOME 要和 MNG_HOME 一样）'))
    built = ci.make_icons(*_icons_args(ctx), bool(install_env), threading.Event())
    if built.ok:
        done = cm.install_icons(ctx.repo, ctx.spec, ctx.app, ctx.data_dir.parent, built.work, built.made_from,
                                tuple(switch_off))
        ok, reason, detail = done.ok, done.reason, done.detail
    else:
        ok, reason, detail = False, built.reason, built.detail
    message = say('gui.controls.icons.done') if ok else say('gui.controls.icons.failed', reason=reason, detail=detail)
    return {'ok': ok, 'message': message, 'icons': icons_info(ctx)}


def key_codes(repo):
    """{virtual key code: keyboard.txt name} from the runtime's own table (keyboard_map.h keymap_inputs), keys only."""
    found = {int(code): name for code, name in KEYMAP_INPUT.findall((Path(repo) / KEYMAP).read_text())}
    return {code: name for code, name in found.items() if code < 128}


def layout(ctx):
    """The game's keyboard.txt as the old page found it: the data folder's, then the app's, else the defaults."""
    for path in (ctx.data_dir / 'keyboard.txt', ctx.app / 'Contents/Resources/keyboard.txt'):
        if path.is_file():
            return kb.read_layout(path), path
    return kb.default_layout(ctx.data_dir / 'keyboard.txt'), None


def keyboard(ctx):
    """The layout (each action and its keys) and the key test's table: key code -> its name, label and actions."""
    found, path = layout(ctx)
    note = say('gui.controls.test.unread', detail=found.problems[0]) if found.problems else \
        '' if path else say('gui.controls.test.defaults')
    keys = {str(code): {'name': name, 'label': kb.INPUTS.get(name, name),
                        'actions': [word(kb.TITLES.get(a, a)) for a in kb.owner(found, name)]}
            for code, name in key_codes(ctx.repo).items()}
    texts = {k.rsplit('.', 1)[-1]: say(f'gui.controls.test.{k}') for k in
             ('title', 'note', 'start', 'stop', 'stopped', 'running', 'none')}
    return {'source': str(path or ''), 'note': note, 'where': KEYBOARD_WHERE, 'texts': texts,
            'actions': [{'id': a, 'title': word(kb.TITLES.get(a, a)),
                         'keys': [kb.INPUTS.get(n, n) for n in found.bindings.get(a, ())]} for a in kb.ACTIONS],
            'keys': keys,
            'formats': {k: say(f'gui.controls.test.{k}', **{f: '{' + f + '}' for f in fields})
                        for k, fields in (('key', ('key',)), ('modifiers', ('modifiers', 'key')), ('char', ('char',)),
                                          ('code', ('code',)), ('actions', ('key', 'actions')), ('unbound', ('key',)))}}


ACTIONS = {'state': state, 'answer': answer, 'swap': swap, 'xbox_letters': xbox_letters, 'save': save,
           'restore': restore, 'icons_info': icons_info, 'icons': icons, 'keyboard': keyboard}
