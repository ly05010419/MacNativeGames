"""The job layer of the drag-and-drop builder app (tools/builder_app): everything that touches files and processes, behind
a command line that the window runs and reads line by line.

  python -m tools.builder_jobs classify <path>...     one JSON object: what each dropped file is, and the plan for a build
  python -m tools.builder_jobs checks [--work <dir>]  one JSON object: what this Mac still needs, with the command that fixes it
  python -m tools.builder_jobs apps [--apps-dir <dir>] one JSON object: the game apps on this Mac, each with the update it needs
  python -m tools.builder_jobs build <request.json>   one JSON event per line (events.py) until the build ends

The rules are the repository's: an installed app is updated in place with the work folder it records, by the fastest of
the three updates that fits (update_runtime.py --plan: express, fast, else build_app.py); a new app goes where
build_app.default_work says (one folder rule for the command line and the window). The game is told from the
files (build_app.py's own readers), never from a name the player types. Nothing is copied: keys and game files are
read where they are. The window shows what these commands print and holds no rule of its own."""
