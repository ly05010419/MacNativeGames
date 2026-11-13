# The models behind MNG Launcher's pages

`tools/builder` is the model library behind MNG Launcher's Settings, Mods, Controls, Storage and Saves pages, for Tears
of the Kingdom and Breath of the Wild: the game's start options, the player's mods, the controller layout and button
icons, the caches and the saves. It has no window, no text mode and no command line of its own. The window is
`tools/builder_app` and the pages call these models through `tools/builder_jobs/tab_<page>.py`, one JSON answer per call;
how the window is made is in [builder-app.md](builder-app.md). This page is for the people and the AI assistants who
change the models: how they are laid out and the rules every change keeps.

## What it is not

- **It ships nothing of the game, and no mod.** Every mod is one the player adds; the models check it, order it and
  install it. There are no built-in cheats or gameplay changes: the game's logic stays as it is
  ([CONTRIBUTING](../CONTRIBUTING.md)), and a player who wants a cheat adds it as a mod.
- **It does not do the repository's work a second way.** A build is `build_app.py` (MNG Launcher's build job). Mods are
  validated and installed by `games/totk/scripts`' `make_mod_manifest.py` and `mod_folder.py`, imported and called in
  process (`mod_tools.py`), so the pages accept exactly what the game's installer accepts.
- **It stores paths and facts, never contents.** Keys, game files and saves are read where they are and never copied
  (a save backup is the copy the player asked for).

## Layout

| Part | Modules | Rule |
|---|---|---|
| Models | `games`, `game_state`, `start_options`, `mod_*`, `controls_file`, `keyboard_layout`, `icons_*`, `storage`, `saves`, `progress` | No window. Pure where they can be; effects only through the ports they are given |
| Page calls | `controller_mods`, `controller_controls`, `controller_icons` | One function per thing a page does (read the state, change, Apply, save, restore, make the icons); each returns a value, nothing runs in the background |
| Ports | `ports.py` (a clock and a command runner), `cancellable.py` (a process the player can stop) | The page creates the real ones; tests pass fakes with the same methods |
| Texts | `strings.py` (the Controls page's English, by id), `gui_controls_table.py` (its names of inputs and buttons) | The other pages keep their texts in their own `tab_*.py`, English and Chinese side by side |

`progress.py` reads `build_app.py`'s output into events for MNG Launcher's build job. `icons_generate.py` is run as a
script by `icons_mod.py`, and the button icons scripts (`games/totk/scripts/make_button_icons.py`,
`make_botw_button_icons.py`) import `keyboard_layout`, `controls_file` and `icons_plan`.

Each page call is short and returns its answer. Work that may take long (an archive, a download, the icons) runs in its
own process group and can be cancelled (`cancellable.py`).

## Rules for every change

1. **A contract per module.** The module's docstring says what it promises, what it refuses and what it leaves to
   others. Change the docstring with the code; a reader (or an assistant) should not need anything else to use the
   module. Tags such as `S5.4 C2` or `E6` name the step and rule that introduced a behaviour.
2. **At most 300 lines per file, tests included.** It is what can be read whole in one pass: a file read in parts ends
   with a function or a class written twice. Split by responsibility before growing (`mod_applied.py` came out of
   `mod_library.py`, `controller_icons.py` out of the Controls calls). Read the whole file before editing it, and search
   for what you are about to write.
3. **One writer per file.** `library.json` is written by one Mods call at a time: the Mods page holds
   `Builder/mods-tab.lock` and refuses a second writer in words. Every Apply of the mods goes through
   `mod_applied.apply_and_record`, and a test reads the sources (`tools/builder` and `tools/builder_jobs`) to make sure
   nothing calls `mod_library.apply` directly. `controls.txt`, `controls-tab.json` and `start-options.json` are written
   atomically by their one model.
4. **Keep what you do not know.** `mod_library.open_library` keeps keys it does not know, so data written by a newer
   or another version survives an older one; a new store does the same.
5. **Narrow exceptions.** Catch only the errors the contract names, where they can happen. A broad `except` stays only
   at a boundary, with a comment that says why (a page's bridge turns a model's error into the answer's words). A
   cleanup that catches `BaseException` undoes its own work and raises again.
6. **Processes.** An argument list, never a shell, in a process group of its own. A cancel or a timeout tries to stop
   the whole group: an archive or the icons' script gets SIGKILL and is waited for. Nothing claims a cancel worked.
7. **Mirror, and test the mirror.** Where the models restate something of the repository (the versions of
   `build_app.py`, the controller map's parser and names, the keyboard map, the game's start options), a drift test
   reads the original source and fails when the two part.

## Where its data lives

| Path | What |
|---|---|
| `~/Library/Application Support/MacNativeGames/Builder/` | `start-options.json` (what only Play passes), `controls-tab.json` (the controller answered with type auto), `icons-venv/` (fontTools for the button icons), `mods-tab.lock`, `history/` (the build job's step times) |
| `~/Library/Application Support/MacNativeGames/<TOTK or BotW>/mod-library/<version>/` | The player's mods, cloned when added, and `library.json` (order, switches, checks, what was applied, the code mods in each app) |
| `~/Library/Application Support/MacNativeGames/<TOTK or BotW>/` | `controls.txt`, `controller-type.txt`, `keyboard.txt` and the game's settings, which the game reads |
| `~/Documents/MacNativeGames-Backups/` | The Saves page's backups (another folder when the player picks one) |

## Tests

```sh
python3.14 -m unittest discover -s tests -p 'test_builder_*.py'
python3.14 -m unittest discover -s tests -p 'test_launcher_*.py'
```

`test_builder_*` test the models, `test_launcher_tab*` the pages over them. Python 3.14 is needed
(`compression.zstd`). The whole suite is `python -m unittest discover -s tests`, as in CONTRIBUTING.

- **Real files in a temporary folder.** Tests that touch the library or the game's folders point `HOME` (and a page's
  home, as `MNG_HOME` does) into a temporary folder and first assert that the paths resolve inside it, so no test can
  reach a player's data.
- **Integration first.** A model that wraps the repository's own scripts is tested against them (the real installer, a
  harness compiled from `runtime/controller_map.h`), not against a copy of their behaviour.
- **A test must be able to fail.** When a test guards a rule, break the rule once and see the test fail, then put it
  back and see it pass.

## Adding something

- **A game or a version:** start from `games.py`, which mirrors `build_app.py` (its drift test says what does not
  match). The Settings and Mods pages name the version they serve (`start_options.py`, `controller_mods.LATEST`,
  `mod_library.TITLE_IDS`); a version they do not know is said in words, never guessed.
- **A page:** a model module with its contract here, a `tools/builder_jobs/tab_<page>.py` that calls it and says the
  result in English and Chinese, its view in `tools/builder_app`, and tests for both.
- **A mod layout or a mod check:** `mod_find.py` finds the mod's folders, `mod_tools.py` validates it with the
  repository's scripts, and every refusal has a text that says why in words (the Mods page's test checks each one).
