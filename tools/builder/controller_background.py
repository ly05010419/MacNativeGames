"""The controller's part for the background picture (S4.3; moved out of controller.py in E17 S17.1 to keep it under 300
lines): 'saved' (screen size), 'save' (chosen file, screen size), 'focus' (screen size, left|center|right) or 'remove'
the picture next to the settings, on load_jobs (the disk and sips may be slow); BackgroundChanged, or
ProcessError('background'), when done. One at a time: two would race on the files."""
from tools.builder import background
from tools.builder.build_run import collect
from tools.builder.session import BackgroundChanged, ProcessError


class BackgroundPart:
    def __init__(self, owner):
        self.owner, self._job = owner, None

    @property
    def busy(self):
        return self._job is not None

    def change(self, action, *args):
        self.owner._open()
        if self._job is not None:
            raise RuntimeError('a background change is still running')
        fn = {'saved': background.saved, 'save': background.save, 'remove': background.remove,
              'focus': background.focus}[action]
        self._job = self.owner.load_jobs.submit(fn, self.owner.runner, self.owner.store.path.parent, *args)

    def tick(self):
        if self._job is None or not self._job.done():
            return
        job, self._job = self._job, None                # removed before its result is read (R5)
        value, error = collect(job)
        self.owner._emit(ProcessError('background', repr(error)) if error is not None else BackgroundChanged(value))
