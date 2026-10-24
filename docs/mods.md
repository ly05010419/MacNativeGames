# Player mods

[English](#english) | [中文](#chinese)

<a id="english"></a>
## English

A mod reaches the game in one of two ways:

- **RomFS content** is loaded while the game runs. Whether a mod is used is decided by one file:
  `manifest.tsv`. The app does not scan folders for mods.
- **Code patches for the main module** (`exefs/main.pchtxt`, `exefs/*.ips`, `exefs/*.ips32`) are merged into
  the app **at build time** with `--mod`. See [Code patches](#code) below.

Nothing under `exefs/` is loaded while the game runs: the app runs the three code modules it was built from,
rewritten for Apple silicon and bound to their SHA-256. The build is the only place code can change.

The launcher points `TOTK_ROMFS_MODS` at

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

where `<version>` is the game version the app was built from: `v0` (1.0.0), `v112` (1.1.2), `v121`
(1.2.1) or `v143` (1.4.3).

### What `manifest.tsv` is

Only the files listed in `manifest.tsv` are served. Each line has four tab-separated fields:

```text
<RomFS path>	<size>	<SHA-256 of the mod file>	<SHA-256 of the original, or ->
```

At startup the loader checks each listed file's size and SHA-256, and checks the original's SHA-256
against the game image. `-` in the last field adds a file the game's RomFS does not have (used by mods
that add assets). Paths are case-sensitive and keep the RomFS spelling.

A folder without `manifest.tsv` means "no mod". Files copied into the folder but not listed are not
used. In detail:

- A **new path** that is not listed is treated as absent: the game cannot see the file.
- A **replacement whose size or hash changed** without regenerating the manifest stops the start with
  `RomFS overlay file missing or wrong size` or `RomFS overlay integrity mismatch`.
- A path that exists in the game but is not listed is read from the game, so the copied file has no
  effect.

### Install a mod

A build can perform steps 2 and 3 with `--mod <mod folder> --install-mod-romfs`. That flag replaces the version
folder whole (what it held is kept next to it as a backup - see [the mod's RomFS files](#data)); without it the build
leaves the folder alone
and prints the two commands that fill it by hand. The steps below are the manual route either way.

1. Quit the game completely.
2. Copy the mod's RomFS paths **directly** into the version folder. If the mod ships a `romfs/` folder,
   copy its contents, not the folder itself:

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. Generate the manifest with the built app. Run this from the repository root:

   ```bash
   python3.14 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   The script reads `Contents/Resources/totk/extracted/<version>/romfs` from the app, hashes each file
   and its original, leaves out files that are identical to the original, and writes `manifest.tsv`.
   It prints a line such as `...: 12 files (10 replaced, 2 added)`.

   `--check`, `--exclude` and `--version` belong to this script, not to `build_app.py` (which only takes
   `--mod` and `--install-mod-romfs` and runs the script itself, `--check` included, whatever flags were passed):
   `--check` reports what the manifest would hold and writes nothing, `--exclude` leaves a path to the app's own
   version of it.

4. Start the game and check the log:

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   It should say `.../mods/v121 checked, N files`; additions also print `M files added to the RomFS`.

### Switching versions

The game reads the installed folder as it is; it does not follow the folder that was downloaded or unpacked.
If the new version removes files, set the old version folder aside (or delete it after a backup) before
copying, or the removed files stay active. Generate the manifest again after every change.

<a id="code"></a>
### Code patches (build time)

`--mod <mod folder>` merges a mod's main-module code patches into the app while it is built. The app has no module
loader: at start it maps the three code modules it was built from (main, sdk and subsdk0), rewritten for Apple
silicon and bound by SHA-256. The patches are applied to the verified retail main of the version being built before
that rewrite, so the app still matches the hash it is bound to; RomFS (data) mods are unaffected and are still
loaded at start.

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" --mod "/path/to/Mod"
```

Several mods with code patches are combined in a mod manager first: TKMM's merged output is one folder with one
`exefs/` and one `romfs/` (a manager may also pack several mods' records into one `.ips`), and `--mod` takes that
folder as it is. Two patch files that change the same bytes are refused with both names, so a merge that overwrote
one mod's change with another's stops the build instead of shipping silently.

The folder holds the mod's files in the usual layout - `exefs/` for code, `romfs/` for data - and either half is
enough: without `exefs/` there is no code to merge, and without `romfs/` the version folder is never touched. An
`exefs/` that holds nothing to merge into main (`subsdk9`, a `main.npdm`) is a note, not a failure: the build
carries on, its code half is in the app, and its data half takes effect once the folder is installed
(`--install-mod-romfs`, or by hand). With `--update` the patch is merged into the updated module, which is the one
the app runs.

<a id="data"></a>
### The mod's RomFS files: `--install-mod-romfs`

A build does not write to the version folder (`~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`)
unless this flag is given: the folder may hold another mod's files, or files copied by hand. With the flag, the
build replaces it with the mod's `romfs/` and writes its `manifest.tsv`:

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" \
  --mod "/path/to/Mod" --install-mod-romfs
```

Then, at the very end, the build:

- **replaces** `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>` with a copy of the mod's
  `romfs/`, so after the build the folder holds exactly this mod's RomFS and nothing else. What it held
  before - another mod's files, files copied by hand - is not deleted: it is moved next to it as
  `<version>.backup-<date>-<time>` (the game never loads that folder; delete it yourself once you no longer
  need it). A mod whose files the game would all take from the app instead replaces nothing, and then the
  folder is left alone: see below;
- writes `manifest.tsv` with `scripts/make_mod_manifest.py` against the app just built, so a path the app
  replaces itself is left out (the heap table included: the 2K overlay carries the mod's heap sizes; the UI
  archive is merged instead, which leaves the mod's own copy next to it as `<name>.mod-original` - the game
  ignores it), and a misspelt path stops the build at its last step - the app is already written by then - with
  the reason instead of stopping the game later.

The folder is not touched until a complete replacement exists: the build copies the mod's `romfs/` into a folder
next to it, writes the manifest there, and moves the new copy into place only when it is complete (the old folder
becomes the backup). Before any of it, the manifest tool runs with `--check` on the mod's own folder, so a
misspelt path stops the build while the version folder still holds what it held; and a mod whose files the game
would all take from the app instead (every one identical to the original, or one the app replaces itself) replaces
nothing the game loads - the build says so and leaves the folder alone rather than swapping in a folder with no
manifest. The build warns when it starts and again right before the copy:

```text
--install-mod-romfs: the player's mod folder for the version this build makes will be REPLACED by The Second Quest
1.4.3/romfs (and its manifest.tsv) when the build ends; what it holds now is moved next to it as
<folder>.backup-<date>-<time>, not deleted. Without the flag the build leaves the folder alone.
...
.../mods/v143: replaced by The Second Quest 1.4.3/romfs and its manifest.tsv (--install-mod-romfs); what it held before is kept in .../mods/v143.backup-20261021-213000
4/4 app: .../mods/v143 holds 5250 files from The Second Quest 1.4.3/romfs now (what it held before: .../mods/v143.backup-20261021-213000; delete that folder once you no longer need it)
   left out (the app replaces it, and the 2K overlay already carries these heap sizes): System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
   .../mods/v143/manifest.tsv: 5229 files (4200 replaced, 1029 added)
```

Without the flag the build leaves the folder untouched and prints the two commands that perform the copy and the
manifest by hand.

Nothing changes at run time: the game loads this folder and its manifest as before. The iPhone / iPad build has
no player mod folder yet, so there `--mod` merges the code patches only (and `--install-mod-romfs` is refused
there: there is no folder to fill).

What the build does, in order:

1. Extracts and verifies the retail ExeFS, as always. The patch is only ever applied to the verified
   retail module of the version being built, never to an already rewritten one.
2. Applies the mod's patches. Offsets are **main addresses** - the address space the game's code lives
   in, the same numbers the app's own patch tables use:

   - `.pchtxt`: the community format, read the same way as the runtime's own `TOTK_PCHTXT` reader (section
     switches and `@flag` lines included, see below). The first line must be
     `@nsobid-<build id>`: the 16 bytes the rewriter's tables use (32 hex digits) or the 20 bytes the
     runtime compares (40 hex digits; the runtime's own `TOTK_PCHTXT` experiment takes only that longer
     form). Patch lines `<hex main address> <hex bytes>` follow, and only lines after `@enabled` count:
     `@disabled` switches the following section off again (a file that keeps an edit around without applying
     it), `@enabled` switches it back on, and `@stop` ends the file. A `@flag offset_shift 0x100` line must
     come before the first patch line - it says the addresses leave out the 0x100-byte NSO header an IPS form
     counts, which is how they are applied; a file without it, or with another shift, is refused by file and
     line rather than patched 0x100 bytes off. `@flag print_values` (a display option of the patching tools,
     in every community pchtxt we have seen) is accepted; any other flag is refused. The `@nsobid-` line identifies
     the target build, so the file name does not matter.
   - `.ips` / `.ips32`: applied as emulators apply them, whose offsets count the 0x100-byte NSO header,
     so the build subtracts 0x100. A record below 0x100, a truncated file or a missing `EOF` is an error.
     The file names neither its module nor its build, so it is read as a main patch only when it is named
     `main`, or after main's build id (`exefs/<build id>.ips`, the Atmosphere and mod-manager
     convention). Anything else is left out with a note: a patch for `sdk` or `subsdk9`, or one named
     after another build of main. An `.ips` named `main` has no id to check; naming it after the build
     id, or shipping a `.pchtxt`, provides that check.

3. Checks the result before building on it. The patch is refused, with the address and the reason, when
   it lands outside main's text, rodata or data (the padding after text is reserved), when two patch
   files change the same bytes, when it changes nothing, or when it touches a site the app patches
   itself - the 60 FPS and real-time frame-step sites, render/output resolution, ultrawide, cloth, and
   the rewriter's registered cuts. Those features are set by the app (`TOTK_FPS60`, `TOTK_REALTIME`,
   `TOTK_RENDER_RES`, `TOTK_OUTPUT_RES`, `TOTK_ASPECT`): the runtime re-checks the original instruction
   at each of its sites and stops the game if it changed.
4. Rewrites the patched module for Apple silicon and binds the app to its SHA-256. The report in `--work`
   (`analysis/phase12-validation.json` for 1.0.0, `analysis/<version>-port/phase12-validation.json`
   otherwise, under `mod_exefs`) records the mod folder, every patch file's SHA-256, the addresses it
   changed and the merged module's hash.

The iPhone / iPad build merges the same way: `--mod` with `--platform ios`.

What else a mod's `exefs/` may hold:

| In the mod's `exefs/` | Result |
| --- | --- |
| `*.pchtxt`, `*.ips`, `*.ips32` for main | merged into the build; an `.ips` must be named `main` or after main's build id, while a `.pchtxt` names its build in `@nsobid-` and may be named anything |
| `main.npdm` | ignored with a note: the native loader never reads a module's metadata |
| `subsdk9` and other extra code modules | **not loaded**, with a note in the build output and in the report: the app loads the three modules it was built from (main, sdk, subsdk0), so the module's own code never runs |
| a whole replacement `main` | refused: a patch is required instead, so the build can check it against this version |
| nothing for main (`subsdk9` alone, a `main.npdm` alone, even an empty `exefs/`) | noted, not refused: nothing is merged into main and the build carries on - its code is not in the app, and its data applies once the folder is installed |

An extra module is how a mod built with a hooking framework (exlaunch and the like) changes code while
the game runs: its trampolines never install here. Such a mod - The Second Quest is one - keeps its main
patches and its whole RomFS, but not the part of its code changes that lives in that module. Making one
work means the app growing a module loader (rewriting the module for Apple silicon and answering its
service calls), which this version does not do.

Changing the mod's code after a build means building again (the mod's bytes are part of the app), and a
build with `--mod --install-mod-romfs` also replaces its data folder. Changing only RomFS files needs no build:
copy them into the version folder and write the manifest again (steps 2 and 3 above). A merge that already
happened is reused: the rewrite is skipped while the recorded hash matches, and the record names the
rewriter's own SHA-256 too, so a changed `patch_nso.py` makes it rewrite instead of reusing the old image.
The 2K output overlay records the heap table it was built on: a build without `--mod` after a build with it
rebuilds the overlay from the retail table instead of keeping the mod's heap sizes.

### Breath of the Wild

The Breath of the Wild app reads its mods from

```text
~/Library/Application Support/MacNativeGames/BotW/mods/<version>
```

where `<version>` is `botw160` (1.6.0) or `botw190` (1.9.0 with its add-on content).

The RomFS half above applies: copy the mod's RomFS contents into that folder and generate the manifest with the app,
naming the folder `botw160` or `botw190`, or passing `--version`:

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw190"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Breath of the Wild.app"
```

Check `grep "mods/botw190 checked" ~/Library/Logs/MacNativeGames/botw.log | tail -1` after the start (`botw160` for 1.6.0).

- A mod that makes a resource larger than the original also has to ship the matching resource size table,
  `System/Resource/ResourceSizeTable.product.srsizetable`, built for the same version. Without it the game reserves the original
  size and stops at start (for example a translation whose `Pack/Bootup_<lang>.pack` grew).
- The game's language follows the app's preferred language, which also picks the language folder a mod must use (`USen` for
  `en-US`). To give only this game another language:
  `defaults write org.macnativegames.botw AppleLanguages -array en-US`.

<a id="prompts"></a>
### Button prompts

Two tools redraw the game's button prompts, so that each prompt shows the controller button you press for that Switch
button. Both read the art from your own app and write a RomFS mod with its `manifest.tsv` into the folder you name.
Nothing of the game is in the project.

#### Automatic icons (Breath of the Wild)

A Breath of the Wild 1.6.0 or 1.9.0 app draws the prompts itself, with no tool to run. At each start the launcher picks the icons:

| Key 6 (`controller-type.txt`) | Icons |
| --- | --- |
| Xbox | Xbox, by position |
| PlayStation | PlayStation, by position |
| Switch | the game's own Switch icons, traced sharp at 2x or more (at 1x the game's own, with no set made) |
| auto | the controller connected at start; with none, the last one the game saw (`controller-detected.txt`). Xbox and other controllers: Xbox; DualSense and DualShock: PlayStation; Nintendo controllers and no controller yet: Switch. `TOTK_FACE_BUTTONS=label` draws them by label |

The icons follow your `controls.txt` too: each prompt shows the button you press on that controller, by its own name for
each position (by position, `south` is Xbox A or Cross). The launcher draws them at the size of the main display, from 1x to 4x.
The scale is the height in pixels of the largest 16:9 picture on the display, divided by 1080 and rounded up. That gives 1x
for 1080p, 2x for a MacBook Pro's 3456x2234 and 3x for 5K. The first start with a new type, controller, display size or `controls.txt` makes the set
once from the app's own RomFS (about 8 seconds on an M1 Pro, before the window opens) and keeps it in
`button-icons/<version>/` next to `controls.txt`, at most four sets (about 37 MB each at 1x, 44 MB at 2x). Later starts reuse it. The game reads
the icons at start only: after key 6 or a new controller during play, the log says
`button icons: xbox-position now, playstation-position from the next start`, and the new icons show at the next start.
After a move to another display, the icons of its size also show from the next start.

- A mod in `mods/<version>` that replaces a file of the set wins: the whole set is left out, and the log says so. The set
  holds `Pack/Bootup.pack` and the `Font/Font_*.sbfarc`, and at 2x or more also
  `System/Resource/ResourceSizeTable.product.srsizetable`. So a mod with its own icons (for example one made with
  `botw_prompts.py` below) turns the automatic icons off. The game then shows that mod's icons or its own. Delete the mod's
  icon files to use the automatic icons. A mod whose only file of the set is the size table (for example an HD texture pack) keeps them: at 2x
  or more the launcher makes the set's size table from the mod's table (`botw_prompts.py --mods`), with each icon entry at the
  larger of the two sizes, and that table is loaded in place of the mod's. The set is made again when the mod's table changes.
- If the set cannot be made (the work folder's Python is gone, for example), the game starts with the Switch icons and
  the log names the cause. The tools run with the Python of the work folder the app was built from (fontTools comes with
  `games/totk/requirements.txt`).
- 1.6.0 is tested in game (Xbox, 2x): the prompts and the glyphs in text show the Xbox buttons. Where a version's files differ from 1.9.0, that texture or font keeps the game's own
  icon, and the log notes it. At 2x or more, the tool needs a size table entry for each file that grows. Without one, it
  draws the whole set at 1x.
- `build_app.py --no-button-icons` builds the app without them. `--button-icons` adds them to a Tears of the Kingdom app.
  Its launcher picks and makes them the same way, always at 1x, with `make_button_icons.py` (not tested in game yet).

The tools below make the same icons by hand, into the mods folder.

- Tears of the Kingdom: `games/totk/scripts/make_button_icons.py` redraws the icon font. The `xbox` and `playstation`
  styles follow your setup with `--face-buttons position|label` and `--controls <controls.txt>` (`--controls` needs
  `--face-buttons`). Without the two options the output is the same as before: the default layout by position. The `xbox`
  and `playstation` styles draw the stick presses as LS and RS (L3 and R3); the prompts drawn as fixed images keep their
  Switch icons.
- Breath of the Wild 1.6.0 and 1.9.0: `games/botw/scripts/botw_prompts.py` redraws eight key textures in the Xbox (or PlayStation) style
  and the controller pictures of the controller tips page (see below).

Before they write, the tools print one line for each Switch button they change, for example `Switch A <- Xbox X`. `make_button_icons.py` prints these lines when you pass `--face-buttons` or `--controls`. Check these lines.

The prompts depend on the face mode: the game's `TOTK_FACE_BUTTONS`, or the controller type picked with key 6. Pass the
same mode as `--face-buttons`:

| The game's setting | `--face-buttons` |
| --- | --- |
| key 6 on Xbox or PlayStation | `position` |
| key 6 on Switch | `label` |
| key 6 on auto, with `TOTK_FACE_BUTTONS=label` | `label` |
| key 6 on auto, without `TOTK_FACE_BUTTONS` | `position` (`label` for a Nintendo controller) |

The shortcut reminder shows the current type. With the wrong mode, every face prompt shows the wrong letter: Xbox A and B
trade places, and so do X and Y. Pass the `controls.txt` the game reads, from its data folder
(`~/Library/Application Support/MacNativeGames/BotW/controls.txt`, or `.../TOTK/controls.txt`). After you change that
file or the mode, run the tool again with `--replace`.

#### Breath of the Wild prompts

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw190"
python3.14 games/botw/scripts/botw_prompts.py "$MODDIR" --app "/path/to/Breath of the Wild.app" \
  --face-buttons position --controls "$HOME/Library/Application Support/MacNativeGames/BotW/controls.txt"
```

Use `botw160` for 1.6.0. The tool needs Python 3.14 and fontTools (`python3.14 -m pip install fonttools`). It changes
eight 48x48 key textures in `Pack/Bootup.pack` > `Layout/Common.sblarc`, and six controller pictures (next section):

- A, B, X and Y become a disc with the letter you press, on the place of the game's disc.
- L and ZL become the Xbox LB, LT, RB or RT key, in the box of the game's key.
- The left stick press (two textures) keeps the game's stick with its small L, traced sharp at `--scale 2` or more (with a
  `controls.txt` remap, the stick you press).
- The new letters come from the Switch system font that your app already holds, made bolder to match the game's letters.

The icons inside text (the controller tips, the pick-up window, camera mode, and R, ZR, + and − in messages) are glyphs of the
button font, `External_00.bffnt` in every `Font/Font_*.sbfarc` (a BC4 glyph sheet). Each button has one drawing (the drawings
of `make_button_icons.py`), and the tool makes the texture and the glyph from it, so the two match. In the font, A,
B, X and Y show the letter you press. L, R, ZL and ZR become LB, RB, LT and RT, and + and − become the Xbox Menu and View buttons. Every stick
keeps the game's drawing (a stick with a small L or R), in both styles: the two stick press glyphs and the glyphs of a stick being
moved (with arrows that show the way). At `--scale 2` or more they are traced sharp, as with `--style switch`.
The d-pad glyphs and the d-pad of the shortcut icons on the screen (four dots on the Switch) become a d-pad cross.
A 4x4 BC4 block can hold pixels of a changed glyph and of its neighbour. The tool encodes such a block again, so a grey
neighbour pixel there can move to the nearest of the block's eight levels. At `--scale 1`, in the seven 1.9.0 fonts no neighbour pixel changes. At 2x or more the whole sheet is encoded again, so an unchanged glyph can also shift a grey pixel by a level.

#### The controller pictures of the controller tips page

The layout of the controller tips page (`PaAllControllerTipsNN_00`) has only one controller artwork: the Switch in handheld
mode. It does not change with the controller you use. It is two pictures, a front view (`Nt_NNillust_00`, 576x288) and the top edge (`Nt_NNillust_01`, 576x100), and labelled lines end on
their buttons. The hint boxes in play (`PaMessageTipsDrcImageNN_00`) use four 228x228 pieces of the same artwork
(`Nt_GamePadIllustSnn_00` to `_03`): the left and right shoulder views, and the right and left front of the handheld.
The tool draws an Xbox controller in these six textures, or a DualSense with `--style playstation`. They hold two channels, grey and alpha. The
pictures come from our own drawing code and the system font, not from the game's art. The tool takes only the places of the
buttons from the game, so the lines still end on a button of the same kind:

- The left stick, the d-pad (a cross where the Switch has four round buttons), the right stick and the four face buttons are
  in the places of the Switch's. View and Menu are in the places of − and +. LT, LB, RT and RB are in the places of ZL, L,
  ZR and R. The body around them has the controller's own outline: two grips below the d-pad and the right stick and the
  bumpers above the top corners, not the Switch's flat shape.
- The four face buttons keep the controller's own layout: Y at the top, X on the left, B on the right and A at the bottom
  (Triangle, Square, Circle and Cross with `--style playstation`). A `controls.txt` changes only which action a button does,
  so it changes the prompts in the text, not the diamond. The ring and dot of a hint box (panes `N_A_00` to `N_Y_00` of
  `PaMessageTipsDrcImageNN_00`) move to the diamond button you press for that Switch button (a button no face button drives keeps
  its ring, and the tool prints a note). Each shoulder button shows the button you press for the Switch
  button of that place, the same chain as for the prompts; one that nothing drives stays blank. A button driven by a stick
  press stays blank too: the pictures carry no LS, RS, L3 or R3.
- The Pro Controller picture (`amiiboProConNN_00`) is only in the amiibo scan hint, and the Wii U GamePad pictures
  (`Nt_GamePadIllustS_*`, `amiiboDRCNN_00`) are for the Wii U layouts. The tool leaves them as they are.
- The DualSense keeps the same places. Its touchpad, a plain PS button and the mute button sit between the sticks. The d-pad
  is four arrow buttons whose middles are the Switch's four d-pad buttons; a real DualSense has its d-pad above the left
  stick, but the lines end where the Switch has it. The face buttons carry Cross, Circle, Square and Triangle, Options and
  Create are small upright buttons in the places of + and −, and the shoulders read L2, L1, R2 and R1.
- `--keep-switch-pictures` leaves all six pictures as the Switch's, and the rings of the hint boxes where the Switch has them. A picture whose texture is not the size of the drawing
  stays too (another game version), and the tool prints a note.
- Tested in play on 1.9.0 (Mac, keyboard input, US and Japanese fonts). With both styles every line of the controls page
  and every ring of the hint boxes ends on a drawn button of the right kind, also in frames 0.3 s apart. Position mode
  with a `controls.txt` and label mode both show the button you press. With a real Xbox pad over Bluetooth and the same
  `controls.txt`, each button pressed did what its icon says. A real PlayStation pad was not used.

These prompts keep the Switch look:

- The sticks: the game's own stick with a small L or R, in both styles (see above). The right stick press texture is a
  drawn Switch R button: the game has no texture of its own for it.
- A prompt whose button has no Xbox picture or letter here, for example Switch A on a shoulder button. The tool prints a
  note for it.

`--style playstation` shows PlayStation buttons instead: L1 and L2 (R1 and R2 in the font), and Options and Create for + and −.
The tool draws the Cross, Circle, Square and Triangle on a disc of the same
size and place as the game's. It was tested in play with the controller pictures (see above).

`--style switch` keeps the game's own icons and draws them sharp at `--scale 2` or more. It traces each texture and glyph from
your RomFS: an edge goes where the game's anti-aliasing puts it, and a grey ring stays grey. So the icons look the same as the
game's, at the larger size. With a `controls.txt` remap a prompt shows the game's own picture of the button you press. Where a
texture has no picture of that button (R, ZR, the right stick press, + and −), a drawn Switch key stands in. At 2x or more every
button glyph of the font is traced, the moving stick glyphs too. The controller pictures stay the Switch's, and the hint box
rings move to the button you press. Use `--face-buttons label` (key 6 on Switch, or a Nintendo controller).

`--scale 2`, `3` or `4` draws the textures, the font glyphs and the controller pictures that many times the game's size, for
sharp prompts on a big display. The default, `--scale 1`, keeps the game's size. At 2x or more the textures and the whole glyph
sheet of every font grow. The tool enlarges the glyphs that it does not change by repeating their pixels. The game
reserves memory for each file by its resource size table. So the tool also writes
`System/Resource/ResourceSizeTable.product.srsizetable`, with the entries of `Layout/Common.blarc` and the seven
`Font/Font_*.bfarc` raised by how much they grew. At 2x the decompressed `Layout/Common.sblarc` grows by about 4 MB, and
each font by about 0.5 MB.

At `--scale 1`, only the bytes of the changed textures and, with a remapped face button, the positions of the hint box rings (eight bytes
of a ring or dot pane) differ. Every other member and byte of the pack is the original, and the
decompressed `Layout/Common.sblarc` keeps its size. The tool writes only `Pack/Bootup.pack`, the seven `Font/Font_*.sbfarc`,
the size table (at `--scale 2` or more) and `manifest.tsv`. The other
files in the folder stay and go into the manifest too. It stops when the folder already has one of these files
(another mod's, or an earlier run). `--replace` overwrites it, and that other mod's changes to these files are lost: the
tool always starts from the game's own files. `--preview <picture.png>` also writes a picture of the changed textures, and `<picture>-pictures.png` with the controller pictures.

#### A mods folder that already holds files

To keep a folder that works, write into a copy and swap it in. Quit the game first.

```bash
MODS="$HOME/Library/Application Support/MacNativeGames/BotW/mods"
cp -R "$MODS/botw190" "$MODS/botw190.new"
python3.14 games/botw/scripts/botw_prompts.py "$MODS/botw190.new" --version botw190 \
  --app "/path/to/Breath of the Wild.app" --face-buttons position \
  --controls "$HOME/Library/Application Support/MacNativeGames/BotW/controls.txt"
mv "$MODS/botw190" "$MODS/botw190.backup" && mv "$MODS/botw190.new" "$MODS/botw190"
```

The copy needs `--version`, because `botw190.new` is not a version name. The old folder becomes `botw190.backup`, which
the game never loads. With no mods folder yet, run the tool on `$MODS/botw190` itself, as above.

To turn the mods off, delete `$MODS/botw190/manifest.tsv` or the whole folder. This turns off every mod in the folder.
To go back to the folder without the prompts, swap the backup back in:

```bash
mv "$MODS/botw190" "$MODS/botw190.prompts" && mv "$MODS/botw190.backup" "$MODS/botw190"
```

For Tears of the Kingdom the steps are the same, with `make_button_icons.py`, `TOTK/mods/<version>` and `--version`.

#### How a new game plugs in

1. Add the game's version name to `APP_VERSIONS` in `games/totk/scripts/make_mod_manifest.py`, so that the manifest
   tool accepts its mods folder.
2. Write a small backend that finds the game's prompt art for each Switch button and writes it back. `botw_prompts.py`
   is the example: two tables of Switch button -> art item (`FACE` and `SHAPE`), and the code that reads and writes
   those items.
3. Reuse the rest: `prompt_layout.py`, `sarc_repack.py`, `write_manifest()` from `make_mod_manifest.py`, and the RomFS
   loader (`runtime/romfs.c`). In `prompt_layout.py`, `prompt_controls()` gives the controller button for each Switch
   button from `controls.txt` and the face mode. `describe()` prints the check lines.

### Notes

- Do not edit `manifest.tsv` by hand; regenerate it.
- Do not change files while the game runs. The loader records each file's identity at start and stops
  if a file changes mid-session.
- There is one overlay directory for player mods: to use several mods, merge their files into one
  version folder and generate one manifest. Do not nest mod folders. If two mods change the same path,
  choose one file.
- The build replaces a few RomFS files itself: `UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs` (quick-menu
  timing; `<n>` is 100, 110 or 140 depending on the version), `Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`, and with the
  2K output `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`. Two overlays cannot replace one path (the start would
  stop with "two overlays replace the same file"), so:
  - the **UI archive** is *merged*: a translation's archive keeps its textures and takes the build's quick-menu edit - the
    tool writes the merged archive in the mod folder, keeps the mod's own copy next to it as
    `<name>.mod-original`, and the merged file replaces the build's overlay (#73). It refuses when the mod changed the
    members the build patches (the capture animations) rather than silently dropping one side;
  - the **heap table** is folded in by the build: with `--mod` the 2K overlay is built on top of the mod's table;
  - any other one (the game-over flow) is left to the app, with a line saying which file it left out.
- `make_mod_manifest.py` handles all of them itself: the UI archive is merged as above, and any other mod file whose
  path the app replaces is left out of the manifest (with a line saying so), because both overlays cannot be loaded. For
  the heap table the line says whether the app's 2K overlay carries the mod's sizes (`already carries these heap sizes`,
  which the build arranges with `--mod`) or says to build with `--mod`.
- A mod manager (TKMM, or Ryujinx's `mods/contents/<title id>/<name>`) provides `exefs/` and `romfs/` in one folder:
  `--mod` takes that folder - its `exefs` patches are merged, and a manager's merged `.ips` may hold several mods'
  records in one file - and `--install-mod-romfs` has the build replace the version folder with its `romfs/` and
  write the manifest itself (no `--exclude` needed: the manager's heap table and the app's 2K overlay are reconciled).
  Re-merge in the manager and build again after every change: the game checks every file's hash
  at start. This is also how several mods with code patches are
  combined: merge them in the manager first, then build with `--mod <the manager's output folder>
  --install-mod-romfs`; the build refuses
  two patch files that change the same bytes and names both of them.
- A mod with a code patch also needs its `exefs/` patch to match the game version the app is built from.
  The build checks what the file itself says: a `.pchtxt` is refused when its `@nsobid-` names another
  main, and an `.ips` / `.ips32` named after a build id is left out when that id is not this main's.
- This guide covers the Mac app; installing mods into the iPhone/iPad app container is not covered.

### When the start stops

The game closes the window and writes one line to `~/Library/Logs/MacNativeGames/totk.log` instead of starting with data
it cannot trust. The line above it names the file. The log is off by default: turn it on with
`defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES` (`org.macnativegames.botw` for Breath of the Wild) and start
the game again. The `grep` checks on this page need it too.

- `two overlays replace the same file` - the manifest lists a path the app replaces itself: a manifest written by hand
  or by another tool, since `make_mod_manifest.py` leaves those paths out by itself (the UI archive it merges instead).
  Regenerate it with that script.
- `two overlays add the same file` - two overlays (the app's, or two mod folders) add one path the RomFS lacks.
- `RomFS overlay integrity mismatch` - a file in the mods folder differs from the SHA-256 in the manifest: files were
  copied in after the manifest was written. Regenerate it (`make_mod_manifest.py` writes nothing for a mismatching dump).
- `RomFS overlay names a file the RomFS lacks` / `adds a file the RomFS already has` - a replacement's original is wrong
  (a file copied from another game version); regenerate the manifest and rebuild the app for that version.
- `RomFS overlay file changed while the game ran` - the folder was edited during play; restart the game (and regenerate
  the manifest).

The window can also vanish with no line of ours: a mod's data can make Metal abort the process (the log then has a host
call stack, for example `-[MTLTextureDescriptorInternal validateWithDevice:]` "requests 9 mipmap levels, but the
dimensions (1, 1, 1) can only support 1"). A texture with more mip levels than its size holds is something the Switch
never checks and Metal refuses to build; the app caps the chain at what the dimensions allow and logs
`[tex] <w>x<h>x<d> ... 声明 N 级 mip ...`, so check the mod's texture if the picture looks wrong.

<a id="chinese"></a>
## 中文

一个 mod 有两种方式进入游戏：

- **RomFS 内容**在游戏运行时加载，是否生效完全由 `manifest.tsv` 决定，App 不会扫描文件夹找 mod。
- **主模块的代码补丁**（`exefs/main.pchtxt`、`exefs/*.ips`、`exefs/*.ips32`）在**构建时**用 `--mod`
  合并进 App，见下面的[代码补丁](#code-zh)。

`exefs/` 里的文件在运行时一律不加载：App 跑的是构建时改写好的三个代码模块，按 SHA-256 绑定。代码只能
在构建这一环节改变。

启动器把 `TOTK_ROMFS_MODS` 指向

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

`<version>` 是构建这个 App 时使用的游戏版本：`v0`（1.0.0）、`v112`（1.1.2）、`v121`（1.2.1）、
`v143`（1.4.3）。

### manifest.tsv 是什么

只有列在 `manifest.tsv` 里的文件才会被提供给游戏。每一行是四个用制表符分开的字段：

```text
<RomFS 路径>	<大小>	<mod 文件的 SHA-256>	<原版文件的 SHA-256，或 ->
```

启动时加载器会逐个核对 mod 文件的大小和 SHA-256，并核对原版文件的 SHA-256。最后一个字段是 `-`
表示这是游戏 RomFS 里没有的新文件（用于新增资源）。路径区分大小写，必须和 RomFS 里的写法一致。

没有 `manifest.tsv` 就等于没有 mod。只把文件复制进目录但不写进清单，游戏不会使用：

- 清单里没有的**新路径**会被当成不存在，游戏完全看不到这个文件。
- 覆盖已有文件但大小或哈希变了、又没有重新生成清单，启动会直接报
  `RomFS overlay file missing or wrong size` 或 `RomFS overlay integrity mismatch`。
- 原版有这个路径、但清单里没列，游戏会读取原版文件，复制进去的文件看起来“没有效果”。

### 安装步骤

构建可以用 `--mod <mod 目录> --install-mod-romfs` 完成第 2、3 步。这个参数会把版本目录整个替换掉（原来的内容
作为备份留在旁边，见下文[模组的 RomFS 文件](#data-zh)）；不带它时构建不会碰这个目录，只在结束时打印手动执行的两条命令。下面是
手动流程（构建做的是同一件事，只是先在旁边的目录里做好再整体换进来）。

1. 先完全退出游戏。
2. 把 mod 的 RomFS 路径**直接**放进版本目录。如果 mod 带一层 `romfs/` 文件夹，复制的是它里面的
   内容，不是 `romfs` 这一层：

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. 用构建好的 App 生成清单。在仓库根目录执行：

   ```bash
   python3.14 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   脚本会读取 App 里的 `Contents/Resources/totk/extracted/<version>/romfs`，计算每个文件和原版
   文件的哈希，跳过与原始完全相同的文件，然后写入 `manifest.tsv`。成功时会打印类似
   `...: 12 files (10 replaced, 2 added)` 的一行。

   `--check`、`--exclude`、`--version` 都是**这个脚本**的选项，不是 `build_app.py` 的（构建只接收 `--mod` 和
   `--install-mod-romfs`，自己带 `--check` 调用这个脚本，与传入的参数无关）：`--check` 只报告清单会
   包含什么、不写任何文件；`--exclude` 把某个路径留给 App 自己的版本。

4. 启动游戏，确认日志：

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   应出现 `.../mods/v121 checked, N files`；有新增文件时还会打印 `M files added to the RomFS`。

### 换版本时

游戏读取的是安装目录本身，不会跟着下载或解压出来的那个文件夹走。如果新版本删掉了旧版本的某些
文件，先把旧的版本目录整个移走（或备份后删除）再复制，否则被删掉的文件会继续生效。每次改动后
都要重新生成清单。

<a id="code-zh"></a>
### 代码补丁（构建时合并）

`--mod <模组文件夹>` 在构建 App 时把模组主模块的代码补丁（IPS、IPS32，或文本格式 `.pchtxt`）合并进去。

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" --mod "/path/to/Mod"
```

App 没有模块加载器：启动时它映射的是构建时的那三个代码模块（main、sdk、subsdk0），已经改写为 Apple 芯片版本
并按 SHA-256 绑定。`--mod` 在改写之前把补丁应用到当前版本核对过的原版 main 上，所以构建出的 App 仍与它绑定的
哈希一致；RomFS（数据）部分不受影响，仍按上面的说明在启动时加载。

多个带代码补丁的 mod 先在模组管理器里合并：TKMM 的合并输出就是一个文件夹，里面一套 `exefs/`、一套
`romfs/`（管理器也可能把几个 mod 的记录合进同一个 `.ips`），`--mod` 原样接收这个文件夹。两个补丁文件改到同一批
字节时构建会拒绝并报出两个文件名，所以「合并时被另一个 mod 覆盖」这种情况会停在构建阶段，不会不声不响地打进去。

文件夹按 mod 的常见结构放文件——代码在 `exefs/`、数据在 `romfs/`；只有一半也可以：没有 `exefs/`
就是没有代码要合并，没有 `romfs/` 时构建不会碰版本目录。`exefs/` 里没有可合并进 main 的东西（只有
`subsdk9`、只有 `main.npdm`，甚至空的 `exefs/`）只算提示、不算失败：构建照常往下走，只是这个 mod 只生效数据
那一半。带 `--update` 时补丁合并进更新后的模块，也就是 App 实际运行的那个。

<a id="data-zh"></a>
### 模组的 RomFS 文件：`--install-mod-romfs`

不加这个参数时，构建不会写版本目录（`~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`）：
里面可能有另一个 mod 的文件，也可能是手动复制进去的。加上这个参数，构建会用模组的 `romfs/` 替换它并生成
`manifest.tsv`：

```bash
python3.14 build_app.py "/path/to/Tears of the Kingdom.xci" -k "/path/to/prod.keys" \
  --mod "/path/to/Mod" --install-mod-romfs
```

这时构建最后会：

- 用 mod 的 `romfs/` 的副本**替换** `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>`，
  所以构建结束后这个目录里就是这一个 mod 的 RomFS，没有别的。原来的内容（另一个 mod 的文件、手动复制进去的
  文件）不会删除，而是挪到旁边的 `<version>.backup-<日期>-<时间>`（游戏不读这个目录，不需要了自己删掉）。如果
  这个 mod 的文件最终一个都不会被游戏加载，那就什么都没替换，目录保持不动，见下文；
- 用 `scripts/make_mod_manifest.py` 对着刚构建好的 App 生成 `manifest.tsv`，所以 App 自己替换的路径会被排除
  （包括堆表：2K 覆盖文件已经带着 mod 的堆大小；UI 归档则是合并，模组原文件会以 `<name>.mod-original` 留在旁边，
  游戏不会读它）；路径大小写写错的会终止构建并给出原因，而不是等到启动时才停止。

版本目录在完整副本就绪之前不会被动：构建先把 mod 的 `romfs/` 复制到它旁边的一个目录里、在那里写好清单，确认完整
之后才把新的一份换进来（旧的目录变成备份）。在此之前还会用 `--check` 在 mod 自己的目录上跑一遍清单工具，所以路径写错
的 mod 会在版本目录还没被碰过时就停下；另外，如果这个 mod 的文件最终一个都不会被游戏加载（全部与原版相同，或者全是
App 自己会替换的文件），它其实什么都没替换——这时构建会说明并保持版本目录不动，而不是换成一个没有清单的目录。
构建开始时和复制前各提示一次：

```text
--install-mod-romfs: the player's mod folder for the version this build makes will be REPLACED by The Second Quest
1.4.3/romfs (and its manifest.tsv) when the build ends; what it holds now is moved next to it as
<folder>.backup-<date>-<time>, not deleted. Without the flag the build leaves the folder alone.
...
.../mods/v143: replaced by The Second Quest 1.4.3/romfs and its manifest.tsv (--install-mod-romfs); what it held before is kept in .../mods/v143.backup-20261021-213000
4/4 app: .../mods/v143 holds 5250 files from The Second Quest 1.4.3/romfs now (what it held before: .../mods/v143.backup-20261021-213000; delete that folder once you no longer need it)
   left out (the app replaces it, and the 2K overlay already carries these heap sizes): System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
   .../mods/v143/manifest.tsv: 5229 files (4200 replaced, 1029 added)
```

不带这个参数时，构建完全不碰这个目录，只打印手动执行的两条命令。

运行时没有任何变化，游戏仍然在启动时加载这个目录和清单；构建只负责复制和写清单。iPhone / iPad 构建
目前没有玩家 mod 目录，所以那里 `--mod` 只合并代码补丁（`--install-mod-romfs` 会被直接拒绝：没有可填的目录）。

构建做的事，按顺序：

1. 照常解出并核对原版 ExeFS。补丁只打在当前版本、核对过的原版模块上，绝不会打在已经改写过的模块上。
2. 打补丁。偏移量都是**主地址**（游戏代码所在的地址空间，和 App 自己补丁表用的数字一致）：

   - `.pchtxt`：社区文本格式，读取规则和运行时一致。第一行必须是 `@nsobid-<build id>`：改写器表里用的
     16 字节（32 位十六进制）或运行时比较的 20 字节（40 位十六进制；运行时自己的 `TOTK_PCHTXT` 实验只认长
     的这种）。后面是 `<十六进制主地址> <十六进制字节>` 行，只有 `@enabled` 之后的行生效：`@disabled` 把后面的
     段落重新关掉（作者留着但不启用某段改动的写法），`@enabled` 再打开，`@stop` 结束整个文件。第一条补丁行之前
     必须有 `@flag offset_shift 0x100`（说明这些地址不含 IPS 算法里的 0x100 字节 NSO 头，构建正是这样打的）；
     没有这一行、或者是别的位移值，都按文件和行号拒绝，而不是打偏 0x100 字节。`@flag print_values`（补丁工具的
     显示选项，我们见过的社区 pchtxt 都带）可以出现；其它 flag 会拒绝。`@nsobid-` 那一行决定目标 build，所以文件名任意。
   - `.ips` / `.ips32`：按模拟器的用法应用，其偏移量包含 0x100 字节的 NSO 头，构建时会减掉 0x100。
     偏移小于 0x100、文件截断、缺少 `EOF` 都算错误。这种文件本身不写模块名和 build id，所以只有名字是
     `main`，或者是 main 的 build id（`exefs/<build id>.ips`，Atmosphere 和模组管理器的约定）时才会
     当作 main 补丁读入；其余情况（给 `sdk`、`subsdk9` 的补丁，或者名字里是另一个 build id 的补丁）
     会留出清单并提示。名为 `main` 的 `.ips` 没有 id 可核对；按 build id 命名，或改用 `.pchtxt`，才会有这项检查。

3. 打完后先检查再往下走。以下情况会被拒绝，并指出地址和原因：补丁落在 main 的 text、rodata、data
   之外（text 之后的填充区是保留的）；两个补丁文件改同一批字节；补丁什么都没改；补丁改到了 App 自己
   打补丁的位置——60 帧与实时单帧步进的地址、渲染/输出分辨率、超宽屏、布料，以及改写器登记的 cut。
   这些功能由 App 设置（`TOTK_FPS60`、`TOTK_REALTIME`、`TOTK_RENDER_RES`、`TOTK_OUTPUT_RES`、
   `TOTK_ASPECT`）：运行时会逐个核对那里原来的指令，被改了就直接停止游戏。
4. 把打过补丁的模块改写为 Apple 芯片版本，App 按它的 SHA-256 绑定。`--work` 里的报告（1.0.0 是
   `analysis/phase12-validation.json`，其他版本是 `analysis/<version>-port/phase12-validation.json`，
   字段 `mod_exefs`）记录 mod 目录、每个补丁文件的 SHA-256、改动的地址和合并后模块的哈希。

iPhone / iPad 构建同样处理：`--mod` 加 `--platform ios`。

mod 的 `exefs/` 里可能还有这些文件，构建这样处理：

| mod 的 `exefs/` 里 | 结果 |
| --- | --- |
| 给 main 的 `*.pchtxt`、`*.ips`、`*.ips32` | 合并进构建；`.ips` 必须叫 `main` 或按 main 的 build id 命名，`.pchtxt` 由 `@nsobid-` 指定版本、文件名任意 |
| `main.npdm` | 忽略并提示：原生加载器不读模块元数据 |
| `subsdk9` 等额外代码模块 | **不加载**：构建输出和报告里都会注明；App 只加载构建时的三个模块（main、sdk、subsdk0），模块自己的代码不会运行 |
| 整个替换的 `main` | 拒绝：只能用补丁，构建才能按版本核对 |
| 没有可合并进 main 的东西（只有 `subsdk9`、只有 `main.npdm`，甚至空的 `exefs/`） | 只提示、不拒绝：main 不做任何合并，构建继续；这个 mod 的代码不进 App，数据部分要装进版本目录才生效 |

用 exlaunch 这类 hook 框架做的 mod，运行时改代码的部分就在这种额外模块里：它的 trampoline 在本 App
里不会安装。像 The Second Quest 这样的 mod，主模块补丁和整个 RomFS 都会生效，但依赖那个模块的那部分
代码改动不会。要支持它就得让 App 具备模块加载能力（把模块也改写为 Apple 芯片版本，并实现它用到的
服务），本版本没有做。

构建之后再改 mod 的代码就要重新构建（mod 的字节已经是 App 的一部分），而带 `--mod --install-mod-romfs` 的
构建也会顺带替换它的数据目录。只改 RomFS 文件不需要构建：把文件放进版本目录、重新生成清单即可（上面的第 2、3
步）。已经合并过的结果会复用：记录的哈希一致时跳过改写；记录里也写了改写器自己的 SHA-256，所以 `patch_nso.py`
一改就会重新改写，而不是复用旧镜像。2K 输出覆盖文件会记下它是在哪份堆表上生成的：先带 `--mod` 构建、之后不带
`--mod` 再构建时，会按原版堆表重新生成，而不是继续沿用 mod 的堆大小。

### 旷野之息

旷野之息 App 从这里读取 mod：

```text
~/Library/Application Support/MacNativeGames/BotW/mods/<版本>
```

`<版本>` 是 `botw160`（1.6.0）或 `botw190`（1.9.0，含追加内容）。

上面的 RomFS 部分都适用：把 mod 的 RomFS 内容复制进这个目录，再用 App 生成清单（目录名为 `botw160` 或 `botw190`，或加 `--version`）：

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw190"
python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" --app "/path/to/Breath of the Wild.app"
```

启动后用 `grep "mods/botw190 checked" ~/Library/Logs/MacNativeGames/botw.log | tail -1` 检查（1.6.0 用 `botw160`）。

- 如果 mod 让某个资源比原版大，还必须带上为同一版本生成的资源大小表
  `System/Resource/ResourceSizeTable.product.srsizetable`。没有它游戏会按原版大小分配，启动时就会停止（例如
  `Pack/Bootup_<语言>.pack` 变大的翻译 mod）。
- 游戏语言跟随 App 的首选语言，也由它决定 mod 要放在哪个语言目录（`en-US` 对应 `USen`）。只给这个游戏换语言：
  `defaults write org.macnativegames.botw AppleLanguages -array en-US`。

<a id="prompts-zh"></a>
### 按键提示

有两个工具可以重绘游戏的按键提示，让每个提示显示这个 Switch 按键实际要按的手柄按键。两个工具都从你自己的 App 读取
图案，把一个 RomFS 模组和它的 `manifest.tsv` 写进你指定的目录。项目里不含任何游戏内容。

#### 自动图标（旷野之息）

旷野之息 1.6.0 或 1.9.0 的 App 自己绘制按键提示，不用运行任何工具。每次启动时，启动器这样选图标：

| 6 键（`controller-type.txt`） | 图标 |
| --- | --- |
| Xbox | Xbox，按位置 |
| PlayStation | PlayStation，按位置 |
| Switch | 游戏自己的 Switch 图标，2x 及以上时描摹成清晰的大图（1x 时就用游戏原图，不生成图标包） |
| 自动 | 启动时连接的手柄；没有时用游戏上次见到的手柄（`controller-detected.txt`）。Xbox 和其他手柄：Xbox；DualSense 和 DualShock：PlayStation；任天堂手柄和还没连过手柄时：Switch。`TOTK_FACE_BUTTONS=label` 时按标签绘制 |

图标也跟随你的 `controls.txt`：每个提示显示在这个手柄上实际要按的按键，用这个手柄自己对每个位置的叫法（按位置映射时，`south` 是 Xbox
的 A 或 PlayStation 的 Cross）。启动器按主显示器的尺寸绘制图标，取 1x 到 4x。
倍数是显示器上能放下的最大 16:9 画面的像素高度除以 1080，向上取整。所以 1080p 为 1x，MacBook Pro 的 3456x2234 为 2x，5K 为 3x。换了类型、手柄、显示器尺寸或
`controls.txt` 后的第一次启动，会用 App 自己的 RomFS 生成一套图标
（M1 Pro 上约 8 秒，在窗口打开之前），保存在 `controls.txt` 旁边的 `button-icons/<版本>/`，最多保留四套（1x 时每套约
37 MB，2 倍时约 44 MB）。以后的启动直接沿用。游戏只在启动时读取图标：游戏中按了 6 键或换了手柄时，日志会写
`button icons: xbox-position now, playstation-position from the next start`，新图标在下次启动时显示。
换到另一台显示器后，按它的尺寸绘制的图标也从下次启动起显示。

- `mods/<版本>` 里的模组如果替换了这套图标里的任何一个文件，以模组为准：整套自动图标都不加载，日志会写明。这套图标包括
  `Pack/Bootup.pack` 和各个 `Font/Font_*.sbfarc`，2x 及以上还包括
  `System/Resource/ResourceSizeTable.product.srsizetable`。所以带有自己图标的模组（例如用下面的 `botw_prompts.py`
  生成的）会关掉自动图标。游戏这时显示那个模组的图标或游戏自己的图标。删掉模组里的图标文件即可使用自动图标。
  如果模组在这套文件里只替换了资源大小表（例如高清贴图包），自动图标照常加载：2x 及以上时，启动器用模组的大小表生成这套图标的大小表
  （`botw_prompts.py --mods`），每个图标条目取两者中较大的值，加载时它代替模组的大小表。模组的大小表改变后，这套图标会重新生成。
- 生成失败时（例如构建用的工作目录里的 Python 已删除），游戏以 Switch 图标启动，日志写明原因。工具用构建这个 App 的
  工作目录里的 Python 运行（fontTools 随 `games/totk/requirements.txt` 安装）。
- 1.6.0 已在游戏里测试（Xbox，2x）：提示和文字里的按键图标都显示 Xbox 按键。某个版本的文件和 1.9.0 不同时，那张贴图或那个字体保留游戏自己的图标，日志会写明。2x 及以上时，
  每个要变大的文件都需要资源大小表里的条目。缺少条目时，工具按 1x 绘制整套图标。
- `build_app.py --no-button-icons` 构建不带自动图标的 App。`--button-icons` 给王国之泪的 App 加上：它的启动器用同样的方式
  选择和生成图标，始终是 1x，用 `make_button_icons.py` 生成（还没在游戏里测试）。

下面的工具可以手动生成同样的图标，写进模组目录。

- 王国之泪：`games/totk/scripts/make_button_icons.py` 重绘图标字体。`xbox` 和 `playstation` 风格加上
  `--face-buttons position|label` 和 `--controls <controls.txt>` 后按你的设置显示（`--controls` 必须同时给
  `--face-buttons`）。不加这两个参数时，输出和以前完全一样：默认的按位置布局。`xbox` 和
  `playstation` 风格把摇杆按下画成 LS 和 RS（L3 和 R3）；以固定图片显示的提示仍是 Switch 图标。
- 旷野之息 1.6.0 和 1.9.0：`games/botw/scripts/botw_prompts.py` 以 Xbox（或 PlayStation）风格重绘八张按键贴图和手柄提示页上的手柄图片（见下文）。

写文件之前，工具会为它改动的每个 Switch 按键打印一行，例如 `Switch A <- Xbox X`；`make_button_icons.py` 在加了 `--face-buttons` 或 `--controls` 时才打印。先核对这些行。

提示取决于面键的映射方式：游戏的 `TOTK_FACE_BUTTONS`，或 6 键选的手柄类型。`--face-buttons` 要用同一种：

| 游戏的设置 | `--face-buttons` |
| --- | --- |
| 6 键选 Xbox 或 PlayStation | `position` |
| 6 键选 Switch | `label` |
| 6 键为自动，设了 `TOTK_FACE_BUTTONS=label` | `label` |
| 6 键为自动，没设 `TOTK_FACE_BUTTONS` | `position`（任天堂手柄为 `label`） |

快捷键说明会显示当前的手柄类型。映射方式选错时，每个面键提示的字母都不对：Xbox 的 A 和 B 互换，X 和 Y 也互换。
`--controls` 要用游戏读取的那份 `controls.txt`，它在游戏的数据目录里
（`~/Library/Application Support/MacNativeGames/BotW/controls.txt`，或 `.../TOTK/controls.txt`）。改了这个文件或映射方式后，
加 `--replace` 重新运行工具。

#### 旷野之息的按键提示

```bash
MODDIR="$HOME/Library/Application Support/MacNativeGames/BotW/mods/botw190"
python3.14 games/botw/scripts/botw_prompts.py "$MODDIR" --app "/path/to/Breath of the Wild.app" \
  --face-buttons position --controls "$HOME/Library/Application Support/MacNativeGames/BotW/controls.txt"
```

1.6.0 用 `botw160`。需要 Python 3.14 和 fontTools（`python3.14 -m pip install fonttools`）。工具修改
`Pack/Bootup.pack` > `Layout/Common.sblarc` 里的八张 48x48 按键贴图，还有六张手柄图片（见下一节）：

- A、B、X、Y 换成一个带有你实际要按的字母的圆盘，放在游戏原来圆盘的位置。
- L 和 ZL 换成 Xbox 的 LB、LT、RB 或 RT 按键，放在游戏原来按键的框里。
- 左摇杆按下（两张贴图）保留游戏的摇杆和上面的小 L，`--scale 2` 及以上时描摹得清晰（`controls.txt` 改了键位时显示你实际按的摇杆）。
- 新字母取自你的 App 里已有的 Switch 系统字体，并加粗到与游戏的字母一致。

文字里的图标（手柄提示、拾取窗口、相机模式，以及消息里的 R、ZR、+、−）是按键字体的字形，即每个 `Font/Font_*.sbfarc` 里的
`External_00.bffnt`（一张 BC4 字形表）。每个按键只有一幅图（`make_button_icons.py` 的图），工具用它同时生成贴图和字形，所以两者一样。
字体里 A、B、X、Y 显示你实际要按的字母。L、R、ZL、ZR 变成 LB、RB、LT、RT，
+ 和 − 变成 Xbox 的 Menu 和 View 按键。两种风格里所有摇杆都保留游戏的图（摇杆和一个小 L 或 R）：
两个摇杆按下的字形，以及推摇杆的字形（带表示方向的箭头）。`--scale 2` 及以上时它们像 `--style switch` 一样描摹得清晰。十字键的字形和屏幕上快捷图标的十字键（Switch 上是四个圆点）换成十字键。
一个 4x4 的 BC4 块里可能同时有被改字形和相邻字形的像素。工具会重新编码这样的块，所以块里相邻字形的灰色像素可能变成块的八个灰度中最近的一个。
在 `--scale 1` 时，1.9.0 的七个字体里没有相邻像素改变。2 倍或以上时整张字形表重新编码，所以未改的字形也可能有灰色像素变动一级。

#### 手柄提示页上的手柄图片

手柄提示页的布局（`PaAllControllerTipsNN_00`）里只有一种手柄图：掌机模式的 Switch，不随你用的手柄变化。它是两张图，正面图（`Nt_NNillust_00`，576x288）和顶边图
（`Nt_NNillust_01`，576x100），带文字的引线指向图上的按键。游戏里的提示框（`PaMessageTipsDrcImageNN_00`）用同一幅图的四块
228x228 小图（`Nt_GamePadIllustSnn_00` 到 `_03`）：左右肩部视图，以及掌机右半和左半的正面。工具在这六张贴图里
画一个 Xbox 手柄，加 `--style playstation` 时画一个 DualSense。贴图有灰度和透明度两个通道。图案由我们自己的绘图代码和系统字体画成，不是游戏的原图；工具只从游戏取按键的位置，
所以引线仍然指在同类按键上：

- 左摇杆、十字键（Switch 是四个圆键，这里画成十字）、右摇杆和四个面键在 Switch 按键的位置。View 和 Menu 在 − 和 + 的位置，
  LT、LB、RT、RB 在 ZL、L、ZR、R 的位置。中间的 Guide 键、握把和机身是 Xbox 的形状，不是 Switch 的形状。
- 四个面键保持手柄本身的布局：上 Y、左 X、右 B、下 A（`--style playstation` 时是三角、方、圆、叉）。`controls.txt` 只改变按键
  执行的动作，所以它改变文字里的按键提示，不改变菱形。提示框（`PaMessageTipsDrcImageNN_00` 的 `N_A_00` 到 `N_Y_00`）里的圆环和圆点会移到你为
  这个 Switch 按键实际要按的菱形按键上（没有任何面键对应的按键，圆环留在原处，工具会打印一行说明）。每个肩键上写的是这个位置上的 Switch 按键你实际要按的按键（和按键提示是同一条
  规则）；没有任何按键对应的肩键留空。由摇杆按下驱动的按键也留空：图上不写 LS、RS、L3 或 R3。
- Pro 手柄图（`amiiboProConNN_00`）只出现在 amiibo 扫描提示里，Wii U GamePad 图（`Nt_GamePadIllustS_*`、`amiiboDRCNN_00`）属于
  Wii U 版的界面，工具都不改。
- DualSense 也保持这些位置。触控板、一个不带标志的 PS 键和静音键在两个摇杆之间。十字键是四个箭头键，每个箭头键的中心就是 Switch
  四个十字键的位置；真正的 DualSense 十字键在左摇杆上方，但引线指向 Switch 的位置。面键上是叉、圆、方、三角，Options 和 Create
  是 + 和 − 位置上的竖直小键，肩键写 L2、L1、R2、R1。
- `--keep-switch-pictures` 让这六张图保持 Switch 的样子，提示框的圆环也留在 Switch 的位置。贴图尺寸与图案不符时（别的游戏版本）也保持原样，工具会打印一行说明。
- 已在 1.9.0 游戏里测试（Mac，键盘输入，美版和日文字体）。两种风格下，操作说明页的每条引线和提示框的每个圆环都指在画出的同类按键上，
  相隔 0.3 秒的几帧里也是如此。按位置模式加 `controls.txt` 和按标签模式都显示你实际要按的按键。
  用蓝牙连接的真实 Xbox 手柄和同一个 `controls.txt` 测试时，按下的每个按键都做了图标上写的动作。没有用真实的 PlayStation 手柄测试。

下面这些提示仍是 Switch 样式：

- 摇杆：两种风格都是游戏自己的摇杆和小 L 或 R（见上文）。右摇杆按下的贴图是画出来的 Switch R 按键：游戏没有它自己的贴图。
- 对应按键在这里没有 Xbox 图案或字母的提示，例如把 Switch A 放在肩键上。工具会为它打印一行说明。

加 `--style playstation` 则显示 PlayStation 按键：L1、L2（字体里还有 R1、R2），+ 和 − 变成 Options 和 Create。
工具把叉、圆、方、三角画在与游戏原图同样大小和位置的圆盘上。它和手柄图片一起在游戏里测试过（见上文）。

加 `--style switch` 则保留游戏自己的图标，在 `--scale 2` 及以上时画得清晰。工具从你的 RomFS 描摹每张贴图和每个字形：边缘落在游戏抗锯齿
所在的位置，灰色圆环仍是灰色，所以图标和游戏原图一样，只是更大。`controls.txt` 改了键位时，提示显示你实际要按的那个按键在游戏里的原图。
某张贴图没有那个按键的原图时（R、ZR、右摇杆按下、+ 和 −），用画出来的 Switch 按键代替。2x 及以上时字体里的所有按键字形都会描摹，
包括推摇杆的字形。手柄图片保持 Switch 原样，提示框的圆圈移到你实际要按的按键上。请用 `--face-buttons label`（6 键选 Switch，或任天堂手柄）。

`--scale 2`、`3` 或 `4` 把贴图、字形和手柄图片画成游戏原尺寸的这么多倍，在大显示器上提示更清晰。默认的 `--scale 1`
保持游戏原尺寸。2x 及以上时贴图和每个字体的整张字形表都会变大。工具把不改的字形按重复像素放大。游戏按资源大小表给每个文件预留内存，
所以工具还会写 `System/Resource/ResourceSizeTable.product.srsizetable`，把 `Layout/Common.blarc` 和七个
`Font/Font_*.bfarc` 的条目按各自增大的量调大。2x 时解压后的 `Layout/Common.sblarc` 约大 4 MB，每个字体约大 0.5 MB。

`--scale 1` 时，只有改动的贴图字节，以及面键被重映射时提示框圆环的位置（圆环或圆点图层的八个字节）不同：包里其余每个成员、每个字节都是原版，解压后的 `Layout/Common.sblarc` 大小不变。工具只写
`Pack/Bootup.pack`、七个 `Font/Font_*.sbfarc`、资源大小表（`--scale 2` 及以上）和 `manifest.tsv`；目录里的其他文件保留，也会写进清单。目录里已经有这些文件之一（别的模组的，
或者上次生成的）时工具会停下。加 `--replace` 会覆盖它，那个模组对这些文件的改动也就没了：工具总是从游戏原版的文件开始改。
`--preview <图片.png>` 另外写一张改动贴图的图片，以及带手柄图片的 `<图片>-pictures.png`。

#### 已有文件的模组目录

为了不动正在用的目录，先在副本里生成，再换进来。先完全退出游戏。

```bash
MODS="$HOME/Library/Application Support/MacNativeGames/BotW/mods"
cp -R "$MODS/botw190" "$MODS/botw190.new"
python3.14 games/botw/scripts/botw_prompts.py "$MODS/botw190.new" --version botw190 \
  --app "/path/to/Breath of the Wild.app" --face-buttons position \
  --controls "$HOME/Library/Application Support/MacNativeGames/BotW/controls.txt"
mv "$MODS/botw190" "$MODS/botw190.backup" && mv "$MODS/botw190.new" "$MODS/botw190"
```

副本要加 `--version`，因为 `botw190.new` 不是版本名。原来的目录变成 `botw190.backup`，游戏不会读取它。还没有模组目录时，
直接对 `$MODS/botw190` 运行工具，同上。

关掉模组：删除 `$MODS/botw190/manifest.tsv` 或整个目录。这会关掉目录里的所有模组。要回到没有按键提示的目录，把备份换回来：

```bash
mv "$MODS/botw190" "$MODS/botw190.prompts" && mv "$MODS/botw190.backup" "$MODS/botw190"
```

王国之泪步骤相同，换成 `make_button_icons.py`、`TOTK/mods/<version>`，并加 `--version`。

#### 新游戏怎么接入

1. 在 `games/totk/scripts/make_mod_manifest.py` 的 `APP_VERSIONS` 里加上这个游戏的版本名，清单工具才接受它的模组目录。
2. 写一个小的后端：为每个 Switch 按键找到游戏里的提示图案，改好后写回。`botw_prompts.py` 就是例子：两张
   「Switch 按键 -> 图案」的表（`FACE` 和 `SHAPE`），加上读写这些图案的代码。
3. 其余部分直接复用：`prompt_layout.py`、`sarc_repack.py`、`make_mod_manifest.py` 的 `write_manifest()`，以及 RomFS
   加载器（`runtime/romfs.c`）。`prompt_layout.py` 里的 `prompt_controls()` 根据 `controls.txt` 和面键映射方式，给出每个
   Switch 按键对应的手柄按键；`describe()` 打印核对用的行。

### 注意

- 不要手改 `manifest.tsv`，每次重新生成。
- 游戏运行中不要改 mod 文件；加载器会在启动时记录每个文件的身份，运行中变化会停止游戏。
- 玩家 mod 只有一个覆盖目录：多个 mod 就合并到同一个版本目录，最后生成一份清单；不要把 mod
  文件夹嵌套进去。两个 mod 改同一个路径时，只能选一个文件。
- 构建本身也会替换几个 RomFS 文件：`UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs`（快捷菜单时机，`<n>`
  按版本是 100、110 或 140）、`Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`，开 2K 输出时还有
  `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`。两份覆盖无法替换同一个路径（启动会报“two overlays replace the
  same file”），所以：
  - **UI 归档**是**合并**的：翻译模组的归档保留自己的贴图，同时接受构建对快捷菜单的改动——工具把合并后的归档写进模组
    目录，模组原文件保留为旁边的 `<name>.mod-original`，合并后的文件取代构建的覆盖层（#73）。如果模组改动了构建也会
    修改的成员（那些 capture 动画），工具会拒绝，而不是悄悄丢掉一侧；
  - **堆表**由构建叠加：带 `--mod` 时 2K 覆盖文件是在模组那份堆表之上生成的；
  - 其余路径（比如 game-over 流程）留给 App，并打印说明排除了哪个文件。
- 这些文件由 `make_mod_manifest.py` 自己处理：UI 归档按上面的方式合并，其它与 App 自身替换项相同的模组文件不会写进
  清单（并打印一行说明），因为两份覆盖无法同时加载。堆表那一行说明 App 是否已经带着这些堆大小，
  否则提示用 `--mod` 重新构建。
- 模组管理器（TKMM，或 Ryujinx 的 `mods/contents/<title id>/<name>`）给出的是同一个文件夹里的 `exefs/` 和 `romfs/`：
  把这个文件夹交给 `--mod`（里面的 `exefs` 补丁会合并，管理器合并出来的 `.ips` 可能一个文件里装着好几个模组的记录），
  再加上 `--install-mod-romfs` 让构建把对应版本的 mods 目录整个换成它的 `romfs/` 并生成清单（不需要再手动
  `--exclude`：管理器的堆表和 App 的 2K 覆盖文件会自动协调）。每次在管理器里重新合并后都要重新构建，因为游戏启动时会逐个校验文件哈希。多个带代码
  补丁的 mod 也是这样合的：先在管理器里合并，再用 `--mod <管理器的输出文件夹> --install-mod-romfs` 构建；两个补丁文件改到同一批字节
  时构建会拒绝，并同时报出这两个文件。
- 带代码补丁的 mod，其 `exefs/` 补丁也要和构建所用的游戏版本一致。构建按文件自己写的东西核对：`.pchtxt` 的
  `@nsobid-` 不是这个 main 就拒绝；按 build id 命名的 `.ips` / `.ips32`，id 不是这个 main 就留出并提示。
- 本文面向 Mac App；iPhone/iPad 的 App 容器内安装未在此说明。

### 启动停止时

游戏会关掉窗口，并在 `~/Library/Logs/MacNativeGames/totk.log` 写下最后一行原因，它上面一行就是出问题的文件。日志默认关闭：先用 `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`（旷野之息是 `org.macnativegames.botw`）打开，再启动一次。本页的 `grep` 检查也需要先打开日志。

- `two overlays replace the same file`：清单里列了 App 自己也会替换的路径——通常是手工写的清单或别的工具写的清单（UI 归档
  的情况会改成合并而不是排除），
  因为 `make_mod_manifest.py` 自己就会把这类路径排除。用那个脚本重新生成即可。
- `two overlays add the same file`：两个覆盖（App 的或两个模组目录）新增了同一个 RomFS 里没有的路径。
- `RomFS overlay integrity mismatch`：模组目录里的文件和清单里的 SHA-256 不一致，也就是生成清单之后又拷了文件进去。
  重新生成清单即可（`make_mod_manifest.py` 遇到对不上的文件不会写清单）。
- `RomFS overlay names a file the RomFS lacks` / `adds a file the RomFS already has`：替换项的“原件”对不上（文件是从
  别的游戏版本拷来的）；按对应版本重新生成清单并重新构建 App。
- `RomFS overlay file changed while the game ran`：游戏运行中改了文件夹；重启游戏并重新生成清单。

窗口也可能直接消失、日志里没有上面这些行：某个模组的数据能让 Metal 直接终止进程（此时日志里有宿主调用栈，例如
`-[MTLTextureDescriptorInternal validateWithDevice:]` 报 “requests 9 mipmap levels, but the dimensions (1, 1, 1) can only
support 1”）。mip 级数超过尺寸允许的贴图在 Switch 上没人检查，Metal 却拒绝创建；App 会把级数裁到尺寸允许的数量并记一行
`[tex] <w>x<h>x<d> ... 声明 N 级 mip ...`，画面不对时应检查该模组的贴图。
