# The drag-and-drop builder app

`MNG Launcher.app` (the drag-and-drop builder) turns a player's own game files into a game app in one window: drop the files, press
**Build and Play**, watch the progress, and the game starts when the build is done. It runs the same `build_app.py`
and `update_runtime.py` as the command line, with the same folder rule; it adds no build step of its own. Mac apps of every game `build_app.py` builds (Tears of the
Kingdom, Breath of the Wild, Ocarina of Time, Donkey Kong Bananza, Pokémon Pokopia) for now; iPhone and iPad come next.

Its Settings, Mods, Controls, Storage and Saves pages (opened under a game's card) took over the older Tk window's tabs, which is gone; the rules
behind them stay in `tools/builder`, the models the pages call through `tools/builder_jobs/tab_*.py`
([builder.md](builder.md)).

## Download builds

The recommended download on GitHub Releases is `MNG-Launcher-<version>-macOS-arm64.dmg`.
Open it, drag `MNG Launcher.app` to Applications, eject the image, then open the installed app.
The ZIP is an alternative. Official downloads are Developer ID signed and Apple notarized.
Players do not re-sign the downloaded Launcher, configure signing certificates or submit it for notarization.

A portable build includes Python and its matching source snapshot; players do not compile the Launcher or
need a checkout. At first use, the source is verified and extracted to
`~/Library/Application Support/MacNativeGames/Builder/sources/<archive-sha256>`. Old source versions remain
available to games that recorded them. Building a game needs the player's own game files and keys.

**No developer tools** (2026-11-17): the source snapshot carries `.prebuilt/`, the programs a Mac build would
otherwise compile (each game version's runtime, SPIRV-Cross, the warm-up tool, the update unpacker), compiled by
the release from the same commit (`tools/prebuilt.py`, with a manifest of the commit and each file's SHA-256). A
build takes a program only when the folder belongs to the source's commit and the file matches; anything else is
compiled as before, so a checkout, or a source with local changes, still needs the command line tools. The
Launcher's first build page then shows "developer tools: not needed" and runs no `xcrun`, `git` or `make`, which
on a Mac without them open Apple's install dialog. Ocarina of Time still needs the full Xcode with its Metal
toolchain: it compiles the shaders translated from the player's own game; the build page says so and links Xcode.
An iPhone / iPad build needs Xcode too.

The Build page is three pages: 1 this Mac (what is missing, with its fix), 2 the game files and keys, 3 where the
app goes and the build. Next opens when the page is done.

### Code updates without a new Launcher (1.0.2 and newer)

Before 1.0.2 a Launcher only ever ran the source it shipped with: an express update rebuilt the same code, and a fix
reached players only through a new Launcher download (2026-11-16: a 1.0.0 player kept the Ocarina of Time grass bug fixed
two days earlier). From 1.0.2 the Launcher also runs newer code from signed source releases:

- **Published by hand.** The maintainer picks a pushed commit and publishes it as a GitHub release tagged `source-*`
  with three files: `mng-source.json` (commit, commit time, archive name, SHA-256 and size, the oldest Launcher it works
  with), `mng-source.json.sig` (its Ed25519 signature) and `mng-source-<commit>.tar.gz` (`git archive` of the commit).
  The private key stays in the maintainer's keychain; nothing is published automatically.
- **Checked by the signed app.** `tools/builder_app/source_updates.py` and `bundled_job.py` ship inside the signed
  Launcher, never inside a source archive. A release is used only when the signature verifies with the public key in the
  Launcher's own `Info.plist` (`MNGSourcePublicKey`, from `tools/builder_app/source_signing.pub`), the archive has the
  size and SHA-256 the manifest names, its commit is newer than the Launcher's own source, and it asks for no newer
  Launcher (otherwise the update's log says which Launcher to download).
- **When.** The home page reads the release list at most once in ten minutes and downloads new code in the background;
  pressing any update (express, fast or full) first makes sure the newest code is here, and its log names the code it
  builds with (`Code: <commit>`). No network or a bad release leaves the code already on the Mac.
- **Where.** Downloaded code goes to the same `sources/<archive-sha256>` folders with its manifest beside it; the game's
  overlay then shows that commit as its build.
- **Privacy.** GitHub is asked with the User-Agent `MNG-Launcher` and nothing about the player.

`MNG_SOURCE_UPDATES=0` in the Launcher's environment keeps it on its own source.

### The Launcher updates itself (the button at the top right)

Code updates change what a build is made from, not the window: a new Launcher still had to be downloaded and dragged into
Applications, and the only sign of one was the version label turning blue (its click downloads the disk image). A
Launcher with the update button does it in one click:

- **The button.** At the home page's top right, only while a newer `launcher-v*` release is out (the same read of the
  release list as above, no other request). It is off while a build or a game's update runs: those use the Python
  inside the app.
- **What a click does.** `tools/builder_app/launcher_update.py`, shipped inside the signed app beside
  `source_updates.py`, downloads the release's `MNG-Launcher-<version>-macOS-arm64.zip` from GitHub, unpacks it and
  takes the app only when all of this holds: the archive is the size the release list names and holds the app alone (no
  name or link that would land outside it); the app says this bundle ID and that version, higher than the running one;
  its code signature is whole and satisfies the designated requirement of the Launcher that is running (the same
  Developer ID team and bundle ID); and macOS accepts it as notarized. The window shows the download, the check, then
  quits.
- **The swap.** A small shell script outside the app waits for the Launcher to quit, moves the old app aside, moves the
  new one in and opens it; when the new one cannot be put in place the old one comes back and says so at its next
  start. The design is Sparkle's (verify before replacing, replace after the app has quit, from outside the bundle).
- **When it cannot.** A refused download, a folder that cannot be written (a disk image, another account's
  Applications), or macOS running the Launcher from a translocated copy: the window says why under the button and
  offers the plain download. Nothing was changed.
- **A first time by hand.** A Launcher from before the button (1.0.6 and older) has no updater: it is replaced once the
  old way.

For tests, `MNG_LAUNCHER_UPDATE_TEST` in the Launcher's environment shows the button without a newer release and lets
the same version be installed again: `1` reads the Launcher's own release list, an address
(`http://127.0.0.1:8000/releases.json`) another one; `MNG_LAUNCHER_UPDATE_CLICK=1` with it presses the button two seconds
after the start. No check of the download changes: a Developer ID Launcher still takes only a notarized app of its own
team. `tests/test_launcher_self_update.py` runs the whole update and every refusal with small apps signed ad hoc, and
with `MNG_TEST_LAUNCHER_RELEASES=<folder of <version>/release/>` with two real signed releases.

## Build a portable copy locally

Local compilation is supported independently of the maintainer's signing credentials and release tools.
It automatically uses an ad-hoc signature and needs no Developer ID certificate or developer membership.
Choose this route only for a source build; do not replace an official downloaded Launcher as an installation step.
Use an Apple silicon Mac with the command line tools. Commit the source you want to bundle first, then run:

```sh
python3 tools/builder_app/make_app.py --portable --version 1.0.0 \
  --scratch /path/outside-checkout/swift \
  -o '/path/outside-checkout/MNG Launcher.app' \
  --zip /path/outside-checkout/MNG-Launcher-1.0.0-macOS-arm64.zip
```

This makes an **ad-hoc signed local build**, not an Apple-notarized official download. Outputs must be outside
the checkout. The builder refuses dirty tracked source, missing required dependencies and local instruction/key
files; untracked and ignored files are never included. ZIP creation preserves symlinks and ordinary resources,
while omitting extended attributes so Archive Utility cannot add AppleDouble (`._`) files.

Run its regression tests with:

```sh
python3 -m pip install -r games/totk/requirements.txt
python3 -m unittest discover -s tests -p 'test_launcher_*.py' -v
```

The **Validate Launcher preview (not notarized)** Actions workflow runs these tests, builds the portable app and
stores its ZIP as a short-lived Actions artifact for testing. It does not publish a GitHub Release.
Official signing, notarization and release automation are maintained separately from this repository.

## Make a development app

With the Xcode command line tools (`xcode-select --install`) and this checkout:

```sh
python3 tools/builder_app/make_app.py            # -> ~/Applications/MNG Launcher.app
```

It builds the window with `swift build`, puts a Python 3.14 inside the app (python-build-standalone 20261021, checked
against its pinned SHA-256, plus `cryptography` at the build's own pin), records this checkout's path in its
`Info.plist` (`MNGRepo`) and signs it ad hoc. About 40 seconds and 93 MB. The app runs the checkout it was made from:
after `git pull`, the next build uses the new code; make the app again only when `tools/builder_app` changed.

## What the player does

The window opens on **the games**, one card each, with no bar on top. A game built already shows its own app's picture
(made by its build from the player's files; the Launcher ships no game art); a game
not built yet shows a plus that opens the build steps with that game chosen. The built games are every app of these
games macOS knows (LaunchServices) and that is still there, plus those in the apps folder, with the update each needs
(`update_runtime.py --plan`). They are asked for by bundle ID (LaunchServices, about 0.05 s) and planned at the same
time, so a look takes well under a second; the window shows the last look (`Builder/installed.json`) at once and brings
it up to date, and only the very first look has the cards waiting. A card opens its actions:
a big green Start game at the top right (Tears of the Kingdom and Breath of the Wild start through the
start options (tab_settings.play), with the choices only it passes; the others open as apps), one update button named after the
fastest update that fits (**Express update**, **Fast update**, or **Full build**,
which opens the build steps with that game chosen), and for Tears
of the Kingdom and Breath of the Wild their pages (Settings, Mods, Controls, Storage, Saves), which open under those
buttons (pressed again, they close). An
update running, or one another program started, shows its progress there. **Update** needs no
game files or keys: it runs the express update, or the fast one (`--seed`) when the startup shaders must be made again,
in place, with the work folder the app records. When only a full build fits, it says why and asks for the game files.

The build steps (a card's plus; or files given to the app, `open -a` or the Dock) are three numbered steps, top to
bottom; a step is dimmed until the one above it is done.

1. **Choose the game**: drop the game files (or the folder that holds them) on the window, or **Choose Game…**. What
   each file is comes from the file itself: a package's ticket, a Switch 2 dump's `main.npdm`, a `.nup`, or a
   `[title ID]` in the name. Of several updates, the newest one `build_app.py` has verified values for is used; the
   others are listed as left out. Paths are remembered, nothing is copied.
2. **Choose your keys**: the `prod.keys` dumped from the player's own Switch, chosen once for every game. A game card is
   opened with them right away, which tells its game and proves the keys can read it and are new enough, before a
   30-minute build. A Switch 2 game needs none and says so.
3. **Build and play**: what this Mac still needs (developer tools and disk space), each
   with its fix (a button for the developer tools' installer, otherwise the command to
   copy); then the one button. While it builds: the bar, the stage and what it is doing, the tool running now with its
   own counter (for example 16590 / 25787 shaders), the time used and, from the second build of a game on this Mac, the
   time left. **Cancel** stops the build (SIGINT, SIGKILL 10 s later); building again goes on where it stopped, since
   `build_app.py` reuses its work folder. At the end the game starts. A failure shows its error, the end of the failing
   tool's output and **Open Log**.

Texts are Chinese when the Mac's first language is Chinese (the jobs too: `MNG_LANG`), else English.

**Opened with files** (`open -a "MNG Launcher" "<base>" "<update>" "<prod.keys>"`, or files dropped on its icon): a
`.keys` file goes to step 2, the rest to step 1. An AI helping a player starts it this way (install-zelda skill).

**Folders, one rule with the command line** (user 2026-11-12: "目录规范要和 launcher 统一"): apps go to `~/Applications`
(**Folders** changes it); a game already installed anywhere is built into that app. The work folder is the one the app
records, else `MacNativeGames-work/<game>` beside the apps folder (`build_app.default_work`, also the default of
`build_app.py` without `--work`); **Folders** can name another root, **Automatic** goes back. With files dropped for an
installed game of the same version (known from the update's `[vNNN]`), the build is the express or fast update when that
fits; another or an unknown version is the full build into the same app and work folder, which reuses what it can.

## Without the window: `tools/mng.py`

The same engine in a terminal, for a player or the AI helping them (user 2026-11-12: "纯用ai打包 也和launcher 能统一使用方法"):
`status`, `check`, `build`, `update`, `page <tab> <action>`, `open` (`python3 tools/mng.py --help`). It runs
`tools/builder_jobs` exactly as the window does and prints the job's events as lines. Any `python3` starts it; it runs
with MNG Launcher's own Python (or a Python 3.14).

**One job at a time on a Mac, whoever starts it** (`tools/builder_jobs/shared.py`): a build or update holds
`Builder/jobs/build.lock` and writes its events to `Builder/jobs/current.ndjson` and its record (pid, game, app, kind,
who started it) to `current.json`. A second one is refused with who holds the slot. A Launcher window that did not start
the running job follows its events and shows it as its own ("Started from …"), and its Cancel sends that job SIGINT.

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
