# The drag-and-drop builder app

`MacNativeGames Builder.app` turns a player's own game files into a game app in one window: drop the files, press
**Build and Play**, watch the progress, and the game starts when the build is done. It runs the same `build_app.py`
as the command line; it adds no build step of its own. Mac apps of every game `build_app.py` builds (Tears of the
Kingdom, Breath of the Wild, Ocarina of Time, Donkey Kong Bananza, Pokémon Pokopia) for now; iPhone and iPad come next.

The older Tk window (`tools/builder`, [builder.md](builder.md)) stays for its Mods, Controls and Saves tabs.

## Make it

With the Xcode command line tools (`xcode-select --install`) and this checkout:

```sh
python3 tools/builder_app/make_app.py            # -> ~/Applications/MacNativeGames Builder.app
```

It builds the window with `swift build`, puts a Python 3.14 inside the app (python-build-standalone 20261021, checked
against its pinned SHA-256, plus `cryptography` at the build's own pin), records this checkout's path in its
`Info.plist` (`MNGRepo`) and signs it ad hoc. About 40 seconds and 93 MB. The app runs the checkout it was made from:
after `git pull`, the next build uses the new code; make the app again only when `tools/builder_app` changed.

## What the player does

The window is three numbered steps, top to bottom; a step is dimmed until the one above it is done.

1. **Choose the game**: drop the game files (or the folder that holds them) on the window, or **Choose Game…**. What
   each file is comes from the file itself: a package's ticket, a Switch 2 dump's `main.npdm`, a `.nup`, or a
   `[title ID]` in the name. Of several updates, the newest one `build_app.py` has verified values for is used; the
   others are listed as left out. Paths are remembered, nothing is copied.
2. **Choose your keys**: the `prod.keys` dumped from the player's own Switch, chosen once for every game. A game card is
   opened with them right away, which tells its game and proves the keys can read it and are new enough, before a
   30-minute build. A Switch 2 game needs none and says so.
3. **Build and play**: what this Mac still needs (developer tools, Homebrew's `opus` and `libvpx`, disk space), each
   with its fix (a button for the developer tools' installer and the Homebrew packages, otherwise the command to
   copy); then the one button. While it builds: the bar, the stage and what it is doing, the tool running now with its
   own counter (for example 16590 / 25787 shaders), the time used and, from the second build of a game on this Mac, the
   time left. **Cancel** stops the build (SIGINT, SIGKILL 10 s later); building again goes on where it stopped, since
   `build_app.py` reuses its work folder. At the end the game starts. A failure shows its error, the end of the failing
   tool's output and **Open Log**.

Texts are Chinese when the Mac's first language is Chinese (the jobs too: `MNG_LANG`), else English.

Apps go to `~/Applications` and work folders to `~/Library/Application Support/MacNativeGames/Builder/work/<game>`
(**Folders** changes both). An app that is already there is built again with the work folder it records.

## How it is made

| Part | Where | Rule |
|---|---|---|
| Window | `tools/builder_app/Sources/MNGBuilder` (SwiftUI) | Shows what the jobs print; holds no rule. Texts in `Strings.swift`, Chinese when the Mac's first language is |
| Jobs | `tools/builder_jobs` (Python) | `classify`, `checks` print one JSON object; `build` prints one event per line (`events.py`) |
| What a file is | `classify.py`, `identify.py` | `build_app.py`'s own readers (`ticket_title_ids`, `npdm_program_id`, `title_kind`, `VERSIONS`), imported, never restated |
| Progress | `history.py` | Each step is a log `build_app.py` starts in `<work>/logs`; the bar is last build's step times on this Mac (no guessed durations); a first build follows the four stages and shows no time left |
| Build | `build_job.py` | `build_app.py` in its own process group; exactly one `done` event; every line also in `~/Library/Logs/MacNativeGames/builder-app-<game>-<time>.log` |

Tests: `python -m unittest tests.test_builder_app_jobs` (file recognition, the plan, the progress record, and a
whole build against a stand-in `build_app.py`: events, a failure and a cancel).

Ideas taken from NX2Apple (MIT) and S2Arm (Apache-2.0), code written here: drop-anything recognition by title ID, the
key check before a build, a JSON event stream with the bar from this Mac's own past builds, one main button.
