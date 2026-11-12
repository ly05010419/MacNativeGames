"""The Settings tab of MNG Launcher: the game's start options (tools/builder/start_options.py, unchanged), the GPU check
for frame interpolation, and Play. The same files as the game's settings panel (key 1) and the old window.

Actions: state() -> the options with their labels, the choices and the notes; choose(option, choice) -> the new state;
probe() -> {'gpu': 'yes'|'no'|'unknown'} (15-21 s, MetalFX frame interpolation of Metal 4); play(gpu) -> PlayResult."""
from tools.builder import game_state, start_options
from tools.builder.ports import CommandRunner, RealClock
from tools.builder_jobs.text import t

# What the old window showed (its gui.settings.* texts, removed with the Tk window 2026-11-12), and in Chinese.
OPTIONS = {
    'picture': (t('Picture at start', '开局画面'),
                t('Automatic starts at 1920x1080 without MetalFX; MetalFX 2K sharpens it to 1440p; Native 2K renders 2K.',
                  '自动：1920x1080，不用 MetalFX；MetalFX 2K：放大到 1440p；原生 2K：直接渲染 2K。')),
    'interpolation': (t('Frame interpolation', '补帧'), t("MetalFX adds frames between the game's own.", 'MetalFX 在游戏自己的帧之间插帧。')),
    'smoothing': (t('Edge smoothing', '边缘平滑'), t('MetalFX smooths the stair-stepped edges of the 3D scene.', 'MetalFX 抹平 3D 画面的锯齿。')),
    'ground': (t('Sharper ground at a distance', '远处地面更清晰'),
               t('Sharper ground, walls and foliage far away (16x anisotropic filtering).', '远处的地面、墙和植物更清晰（16 倍各向异性过滤）。')),
    'aspect': (t('Picture shape', '画面比例'), t('21:9, or the shape of the window, instead of 16:9.', '21:9 或窗口的形状，代替 16:9。')),
}
CHOICES = {'auto': t('Automatic', '自动'), 'metalfx': 'MetalFX 2K', 'native-2k': t('Native 2K', '原生 2K'),
           'interp30': '30+30', 'interp': t('Half the refresh rate', '屏幕刷新率的一半'), 'interpmax': t('Uncapped', '不限'),
           'on': t('On', '开'), '16': '16x', '21:9': '21:9'}
REASONS = {'no_game_file': t('No app chosen.', '没有选 App。'),
           'no_app': t('No built app here: build it first.', '这里没有打好的 App：先打包。'),
           'unknown_version': t('These settings are for Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0.',
                                '这些设置只对王国之泪 1.4.3 和旷野之息 1.9.0 有效。'),
           'running': t('The game is running: close it first (or use its settings panel, key 1).',
                        '游戏开着：先关掉游戏（或在游戏里按 1 打开设置面板改）。'),
           'open_failed': t('The game could not be started.', '游戏没能启动。')}
WHERE = {start_options.FILE: t("Kept in the game's settings panel (key 1): changing it in either place changes both.",
                                '和游戏里的设置面板（按 1）是同一份：在哪边改都一样。'),
         start_options.PLAY: t('Only when the game is started with Play here (the settings panel does not have it).',
                               '只在这里点“游玩”启动时生效（游戏的设置面板里没有这一项）。')}


def paths(ctx):
    return start_options.StartPaths(ctx.builder_dir / 'start-options.json', ctx.data_dir.parent)


def note(text):
    """A note of read_state in words: 'running', or the model's own sentence."""
    return REASONS.get(text, text)


def view(state):
    return {'name': state.name, 'version': state.version_label, 'reason': REASONS.get(state.reason, state.reason),
            'options': [{'id': o.id, 'label': OPTIONS[o.id][0], 'description': OPTIONS[o.id][1], 'where': WHERE[o.how],
                         'choices': [{'id': c, 'label': CHOICES.get(c, c)} for c in o.choices],
                         'chosen': state.choices.get(o.id, 'auto')} for o in state.options],
            'notes': [note(n) for n in state.notes]}


def state(ctx):
    return view(start_options.read_state(ctx.spec, ctx.app, paths(ctx)))


def choose(ctx, option, choice):
    return view(start_options.choose(ctx.spec, ctx.app, paths(ctx), option, choice))


def probe(ctx):
    return {'gpu': game_state.probe_metal4fx(CommandRunner(RealClock()))}


def play(ctx, gpu='unknown'):
    result = start_options.play(CommandRunner(RealClock()), ctx.spec, ctx.app, paths(ctx), gpu)
    return {'ok': result.ok, 'reason': REASONS.get(result.reason, result.reason), 'detail': result.detail,
            'held': [OPTIONS[h][0] for h in result.held]}


ACTIONS = {'state': state, 'choose': choose, 'probe': probe, 'play': play}
