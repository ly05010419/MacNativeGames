# How the builder is made

`tools/builder` is the window (and its text mode) that checks the Mac and the player's files, runs `build_app.py`,
starts the game with its settings, and manages the player's mods and controls, for Tears of the Kingdom and Breath of
the Wild. What a player sees is in [install.md](install.md#build-with-a-window). This page is for the people and the AI
assistants who change its code: how it is laid out and the rules every change keeps.

## What it is not

- **It ships nothing of the game, and no mod.** Every mod is one the player adds; the builder checks it, orders it and
  installs it. It has no built-in cheats or gameplay changes: the game's logic stays as it is
  ([CONTRIBUTING](../CONTRIBUTING.md)), and a player who wants a cheat adds it as a mod.
- **It does not do the repository's work a second way.** A build is `build_app.py`, run with the paths the checks
  approved. Mods are validated and installed by `games/totk/scripts`' `make_mod_manifest.py` and `mod_folder.py`,
  imported and called in process (`mod_tools.py`), so the builder accepts exactly what the game's installer accepts.
- **It stores paths and facts, never contents.** Keys, game files and saves are read where they are and never copied.

## Layers

The builder is MVC with the effects at the edges.

| Layer | Modules | Rule |
|---|---|---|
| Model | `games`, `manifests`, `inputs`, `checks*`, `paths`, `command`, `progress`, `session`, `process`, `config`, `game_state`, `start_options`, `mod_*`, `controls_file`, `icons_*`, `updates`, `background` | No Tk. Pure where it can be; effects only through the ports it is given |
| Controller | `controller` (with `build_run`, `build_request`), `controller_start`, `controller_mods`, `controller_controls`, `controller_icons` | Blocking work runs as jobs on one worker thread (`jobs.py`); the view calls `tick()`, which never waits; results come back as events |
| View | `gui*.py` (Tk), `cli.py` (text) | Draws the state and events it receives and calls the controller. No rule lives in a view |
| Composition root | `app.py` | The one place that creates the real adapters (`ports.py`: commands, clock, files) and wires them; tests pass fakes with the same methods |
| Texts | `strings.py`, `strings_tabs.py` | Every visible text, in English, by id. A missing id is a bug that a test finds |

Each tab has its own part of the controller and its own job at a time: a second request while one runs is refused, not
queued. Work that may take long (an archive, a download, a build) runs in its own process group and can be cancelled
(`cancellable.py`, `process.py`).

## Rules for every change

1. **A contract per module.** The module's docstring says what it promises, what it refuses and what it leaves to
   others. Change the docstring with the code; a reader (or an assistant) should not need anything else to use the
   module. Tags such as `S5.4 C2` or `E6` name the step and rule that introduced a behaviour.
2. **At most 300 lines per file, tests included.** It is what can be read whole in one pass: a file read in parts ends
   with a function or a class written twice. Split by responsibility before growing (`gui_controls_info.py` came out
   of `gui_controls.py`, `mod_applied.py` out of the Mods controller). Read the whole file before editing it, and
   search for what you are about to write.
3. **One writer per file.** `library.json` is written only by the Mods part's job, `settings.json` only by
   `config.save` (atomically). Every Apply of the mods goes through `mod_applied.apply_and_record`, and a test reads
   the source to make sure nothing calls `mod_library.apply` directly.
4. **Keep what you do not know.** `config.load` and `mod_library.open_library` keep keys they do not know, so data
   written by a newer or another builder survives an older one.
5. **Narrow exceptions.** Catch only the errors the contract names, where they can happen. A broad `except` stays only
   at a few boundaries, each with a comment that says why: a crashing check becomes a blocking result
   (`checks.run_checks`), the worker keeps a job's error for whoever reads its result (`jobs.py`), a job's error is
   published once instead of raised at every tick (`build_run.py`), and the views stay alive after an error (`gui.py`,
   `cli.py`). A cleanup that catches `BaseException` undoes its own work and raises again.
6. **Processes.** An argument list, never a shell, in a process group of its own. A cancel or a timeout tries to stop
   the whole group: a build gets SIGINT, then SIGKILL; an archive or the icons' script gets SIGKILL and is waited for.
   Nothing claims a cancel worked: the window says how the process ended and whether that matches the signals sent.
7. **The view's thread never waits.** Checks, builds, reading the library, archives and downloads are jobs; a view
   asks the controller and draws the answer when it comes. The few effects a view does itself (opening the app or
   Finder, the clipboard) are injected, so tests replace them.
8. **Mirror, and test the mirror.** Where the builder restates something of the repository (the versions of
   `build_app.py`, the controller map's parser, the game's start options), a drift test reads the original source and
   fails when the two part.

## Where its data lives

| Path | What |
|---|---|
| `~/Library/Application Support/MacNativeGames/Builder/` | `settings.json` (paths, the last build), the chosen picture, the Controls tab's state, the venv for the button icons |
| `~/Library/Application Support/MacNativeGames/<TOTK or BotW>/mod-library/<version>/` | The player's mods, cloned when added, and `library.json` (order, switches, checks, what was applied) |
| `~/Library/Application Support/MacNativeGames/<TOTK or BotW>/` | `controls.txt` and `controller-type.txt`, which the game reads |
| `~/Library/Logs/MacNativeGames/builder-<game>-<date>-<time>.log` | Each build's output |

## Tests

```sh
python3.14 -m unittest discover -s tests -p 'test_builder_*.py'
```

Python 3.14 is needed (`compression.zstd`); the window's tests need Tk. The whole suite is
`python -m unittest discover -s tests`, as in CONTRIBUTING.

- **Real files in a temporary folder.** Tests that touch the library or the game's folders point `HOME` into a
  temporary folder and first assert that the paths resolve inside it, so no test can reach a player's data.
- **Fakes at the ports.** `tests/builder_fakes.py` (the ports), `builder_fake_process.py`, `builder_fake_build.py`
  and `builder_fake_controller.py` (a view's tests drive a real Tk view against a fake controller);
  `builder_tk_base.py` gives a withdrawn Tk root and helpers to read what the view shows.
- **Integration first.** A model that wraps the repository's own scripts is tested against them (the real installer, a
  harness compiled from `runtime/controller_map.h`), not against a copy of their behaviour.
- **A test must be able to fail.** When a test guards a rule, break the rule once and see the test fail, then put it
  back and see it pass.

## Adding something

- **A game or a version:** start from `games.py`, which mirrors `build_app.py` (its drift test says what does not
  match). The Settings and Mods tabs name the version they serve (`start_options.py`, `controller_mods.LATEST`,
  `mod_library.TITLE_IDS`); a version they do not know is said in words, never guessed.
- **A tab:** a model module with its contract, a controller part with its own job, a `gui_<tab>.py` view that only
  draws, its texts in `strings_tabs.py`, and the view registered in `gui.py`.
- **A mod layout or a mod check:** `mod_find.py` finds the mod's folders, `mod_tools.py` validates it with the
  repository's scripts, and every refusal has a text that says why in words.
