"""The window's Play bar (E13 S13.3): ▶ Play in the title row, in sight whichever tab is shown, with one row under it
for why it cannot play now or how the last Play went. A view: controller.start keeps the state and plays.

It also owns the Settings part's state jobs, so the bar and the Settings tab never ask at once. Every read, choice and
Play goes through it and is tagged with the selection (the game and its paths) it was asked for. An answer counts only
while that selection is still the chosen one: a late answer for an earlier game or app neither enables Play nor shows
its app, and the latest selection is read once the running job has ended. A change of selection disables Play at once
and asks for one read; refresh() asks nothing more until the selection changes again, after a build (stale), or when a
tab asks (request). The row under the bar holds at most two lines, cut with "…"; the Settings tab's status keeps the
whole text. Enabled as Play always was: a game, its state without a reason, the controller idle (E12's holds) and no
Settings job running. One click, one Play.

E14 S14.1: Play only when the game is what the Mods tab shows. The bar asks Mods for nothing: it takes the Mods tab's
reads and the Mods part's answers (heard), each counted only while controller.mods.answered names the chosen game, game
file and app (mods_key); until such a state arrives (after a change of selection, a successful Apply or a successful
build) the button is disabled, and so is every action while a Mods job runs (a quick read too; the line says Mods is
applying or reading). From that state, the first match wins: a failed read or change: disabled, its error;
no_game_file: disabled, its reason; no_app: Build; code_pending or the texture pack's heap_pending: Rebuild; E17:
the Settings state's rebuild (native 4K chosen and the app has none, or a native 4K app with the pack's heaps):
Rebuild; not_applied: Apply mods; not_supported, not_latest (its reason in the line) or nothing pending: Play. Build
and Rebuild open the Build tab and run its checks (build: the window's callback); a build starts there, never here.
Apply mods is the Mods tab's Apply (apply_mods) and never launches; a failed one keeps its reason in the line until the
next Apply or another selection. One click, one job."""
import tkinter as tk

from tools.builder.controller_mods import mods_key
from tools.builder.gui_style import BAD, GOOD, MUTED, PAD, WRAP, muted
from tools.builder.gui_widgets import FlatButton
from tools.builder.session import Finished, ModsApplied, ModsStateReady, PlayDone, ProcessError, StartStateReady
from tools.builder.strings import text

LINES = 2
LABELS = {'play': 'gui.settings.play', 'wait': 'gui.settings.play', 'build': 'gui.play.build',
          'rebuild': 'gui.play.rebuild', 'apply': 'gui.play.apply'}


def fit(font, line, width=WRAP, lines=LINES):
    """line (newlines folded) as Tk wraps it at width, word by word: whole when it takes at most `lines` lines, else
    those lines with the last one cut and ended with "…"."""
    rows, row, words = [], '', ' · '.join(p for p in line.split('\n') if p).split(' ')
    for word in (piece for w in words for piece in _pieces(font, w, width)):
        candidate = f'{row} {word}' if row else word
        if row and font.measure(candidate) > width:
            rows.append(row)
            row = word
        else:
            row = candidate
    rows.append(row)
    if len(rows) <= lines:
        return ' '.join(rows)
    last = rows[lines - 1]
    while last and font.measure(last + '…') > width:
        last = last[:-1]
    return ' '.join(rows[:lines - 1] + [last.rstrip() + '…'])


def _pieces(font, word, width):
    """A word wider than the line, in the pieces Tk breaks it into (by character)."""
    while font.measure(word) > width:
        cut = len(word) - 1
        while cut > 1 and font.measure(word[:cut]) > width:
            cut -= 1
        yield word[:cut]
        word = word[cut:]
    yield word


class PlayBar:
    def __init__(self, header, below, controller, fonts, build, apply_mods):
        self.controller, self.fonts, self.build, self.apply_mods = controller, fonts, build, apply_mods
        self.state, self.status, self._key, self._asked, self._want = None, ('', MUTED), None, None, False
        self._job, self._failed = None, False           # one of ours running ('read', 'choose', 'play'); a read failed
        self.mods, self._mods_key, self._mods_error, self._applied = None, None, None, None    # E14 S14.1
        self._action = 'wait'                           # what the button does: a key of LABELS
        self.button = FlatButton(header, text('gui.settings.play'), self.press, kind='primary', font=fonts['action'],
                                 size=(170, 44), state='disabled')
        self.button.pack(side='right')
        self.line = muted(tk.Label(below, text='', anchor='w', justify='left', wraplength=WRAP), fonts['small'])

    @classmethod
    def under_title(cls, form, controller, fonts, build, apply_mods):
        """The window's title row with the bar at its right, and the bar's line under it (in sight in every tab)."""
        header, below = tk.Frame(form), tk.Frame(form)
        header.pack(fill='x', padx=PAD, pady=(22, 0))
        below.pack(fill='x', padx=PAD, pady=(4, 6))
        tk.Label(header, text=text('gui.title'), font=fonts['title']).pack(side='left', anchor='w')
        return cls(header, below, controller, fonts, build, apply_mods)

    def _selection(self):
        game, fields = self.controller.selection()
        return game, tuple(sorted(fields.items()))

    # the Settings part's jobs, one coordination
    def request(self):
        """A tab wants the chosen game's state (the Settings tab on opening); a read of this selection already running
        serves it."""
        if not (self._job == 'read' and self._asked == self._selection()):
            self._want = True
            self._read()

    def stale(self):
        """The app may have changed (a build ended): read again at the next refresh."""
        self._key = None

    def _read(self):
        start = self.controller.start
        if self._want and not start.busy and self._selection()[0] is not None:
            self._want, self._asked, self._job = False, self._selection(), 'read'
            start.state()

    def choose(self, option, choice):
        self.controller.start.choose(option, choice)
        self._asked, self._job = self._selection(), 'choose'

    def press(self):
        """The one button: Play, or what has to come before it (S14.1)."""
        if self._action == 'play':
            self.play()
        elif self._action in ('apply', 'build', 'rebuild') and self._free():
            self.button.configure(state='disabled')     # until the next refresh says what comes next
            if self._action == 'apply':
                self._applied = None
                self.apply_mods()
            else:
                self.build()

    def play(self):
        if not self._can_play():
            return
        self.button.configure(state='disabled')
        self.status = (text('gui.settings.playing'), MUTED)
        self.controller.start.play()
        self._asked, self._job = self._selection(), 'play'
        self._show(self.status)

    def on_event(self, event):
        """Takes the Settings part's events (the Settings panel hands them on). A job's answer (a state, Play's outcome,
        an error) is fresh when the selection it was asked for is still the chosen one: True, and the tab shows it;
        False for a late one, which neither the bar nor the tab shows. A read that failed is said here; it is not
        retried until something asks again (a tab, another selection, a build)."""
        fresh, job = True, None
        if isinstance(event, (StartStateReady, PlayDone)) or (isinstance(event, ProcessError)
                                                              and event.where in ('settings', 'play')):
            job, self._job = self._job, None
            fresh = self._asked == self._selection()
        if not fresh:
            self._want = self._want or isinstance(event, StartStateReady)   # an earlier selection's: read the new one
        elif isinstance(event, StartStateReady):
            self.state, self._key = event.state, self._asked
            if self._failed:                            # the read that failed has been answered now
                self.status, self._failed = ('', MUTED), False
        elif isinstance(event, ProcessError) and event.where == 'settings' and job == 'read':
            self.state, self._key = None, self._asked
            self.status, self._failed = (text('gui.settings.error', error=event.error), BAD), True
        elif isinstance(event, PlayDone):
            r = event.result
            lines = [text('gui.settings.played') if r.ok else text(f'gui.settings.play_failed.{r.reason}',
                                                                   detail=r.detail)]
            self.status = ('\n'.join(lines + [text(f'gui.settings.held.{h}') for h in r.held]),
                           GOOD if r.ok and not r.held else BAD)
        elif isinstance(event, ProcessError) and event.where == 'play':
            self.status = (text('gui.settings.error', error=event.error), BAD)
        self._read()
        return fresh

    def heard(self, event):
        """Every window event (the window hands them all on); the bar keeps the Mods part's answers that are about the
        chosen game and app, and a build's end."""
        if isinstance(event, Finished):
            self.stale()
            if event.outcome.name == 'SUCCESS':
                self.mods = None                        # a new app: the Mods tab reads it again; wait for that
            return
        if not (isinstance(event, (ModsStateReady, ModsApplied))
                or (isinstance(event, ProcessError) and event.where == 'mods')):
            return
        kind, key = self.controller.mods.answered or (None, None)
        if key != mods_key(self.controller.selection()):
            return                                      # an earlier game's or app's: never shown
        self._mods_key = key
        if isinstance(event, ModsStateReady):
            self.mods, self._mods_error = event.state, None
        elif isinstance(event, ModsApplied) and event.result.ok:
            self.mods, self._applied = None, None       # the Mods tab reads again after Apply: wait for that state
        elif isinstance(event, ModsApplied):
            r = event.result
            self._applied = (text(f'gui.mods.applied.{r.reason or "ok"}', removed=r.removed_backups), BAD)
        elif kind == 'apply':
            self._applied = (text('gui.error', error=event.error), BAD)
        else:                                           # a read or a change failed: what the game holds is unknown
            self.mods, self._mods_error = None, (text('gui.error', error=event.error), BAD)

    # the bar
    def _need(self):
        """(what the button does, the line under it) from the Mods state: S14.1's table, the first match wins."""
        m = self.mods
        if m is None:
            return 'wait', self._mods_error or (text('gui.play.reading'), MUTED)
        reason = m.reason and (text(f'gui.mods.reason.{m.reason}', name=m.name, version=m.version_label), MUTED)
        if m.reason in ('not_supported', 'not_latest'):
            return 'play', reason if m.reason == 'not_latest' else None
        if m.reason:
            return 'build' if m.reason == 'no_app' else 'wait', reason
        t = m.textures
        if m.code_pending or (t is not None and t.heap_pending):
            line = 'gui.mods.code_pending' if m.code_pending else \
                'gui.textures.build_on' if t.enabled else 'gui.play.why.heap_off'
            return 'rebuild', (text(line), BAD)
        if self.state is not None and self.state.rebuild and self._key == self._selection():    # E17: the picture
            return 'rebuild', (text(f'gui.play.why.{self.state.rebuild}'), BAD)
        if m.not_applied:
            return 'apply', self._applied or (text('gui.play.why.apply'), BAD)
        return 'play', None

    def _free(self):
        return self.controller.idle and not self.controller.start.busy and not self.controller.mods.busy

    def _can_play(self):
        return (self._action == 'play' and self.state is not None and not self.state.reason
                and self._key == self._selection() and self._free())

    def refresh(self, idle):
        key = self._selection()
        if key != self._key:                            # another game or paths: nothing shown is valid any more
            self._key, self.state, self.status, self._failed = key, None, ('', MUTED), False
            self._want = key[0] is not None
        if mods_key(self.controller.selection()) != self._mods_key:     # another game file or app: wait for its state
            self._mods_key = mods_key(self.controller.selection())
            self.mods, self._mods_error, self._applied = None, None, None
        self._read()
        self._action, note = self._need() if key[0] is not None else ('play', None)
        self.button.configure(text=text(LABELS[self._action]))
        if self._action != 'play':
            self.button.configure(state='normal' if self._action != 'wait' and idle and self._free() else 'disabled')
            self._show(note if idle else self._busy())
            return
        can = idle and self._can_play()
        self.button.configure(state='normal' if can else 'disabled')
        if can or self.controller.start.busy or (self.state is None and key[0] is not None and idle):
            self._show(self.status if self.status[0] or note is None else note)    # the last Play's outcome, or why
        else:
            busy = not idle or self.controller.mods.busy        # a quick Mods read does not hold, but Play waits
            self._show(self._busy() if busy else (text('gui.settings.why_not.app'), MUTED))

    def _busy(self):
        """Why nothing can start now: Mods applying, Mods reading or changing (a full read holds too), or a build."""
        mods = self.controller.mods
        return (text('gui.mods.applying' if mods.applying else 'gui.play.reading' if mods.busy
                     else 'gui.settings.why_not.build'), MUTED)

    def _show(self, shown):
        line, colour = shown
        self.line.configure(text=fit(self.fonts['small'], line) if line else '', foreground=colour)
        if line and not self.line.winfo_manager():
            self.line.pack(fill='x', anchor='w')
        elif not line and self.line.winfo_manager():
            self.line.pack_forget()
