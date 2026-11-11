"""The events a job prints, one JSON object per line, each with "event" (its kind) and "t" (seconds since 1970).

  plan        game, app, work, log, measured (the bar comes from this Mac's last build), command
  stage       index, total, name, text           build_app.py's 'i/4 <name>: <text>' lines
  step        name, index                         a tool started (its log in the work folder)
  progress    fraction (0..1), left (seconds or null), elapsed, measured, step_done/step_total (the running
              step's own 'N/M' counter, or null)
  log         text                                any other line build_app.py printed
  cancelling                                      a cancel was received and sent to the build
  done        ok, and app/elapsed, or cancelled/message/detail/step_log/log
The window (tools/builder_app, BuildEvent.swift) decodes exactly these; a new field is optional there."""
import json
import threading
import time


class Emitter:
    def __init__(self, stream):
        self.stream, self.lock = stream, threading.Lock()

    def __call__(self, event, **fields):
        line = json.dumps({'event': event, 't': round(time.time(), 3), **fields}, ensure_ascii=False)
        with self.lock:
            self.stream.write(line + '\n')
            self.stream.flush()
