# EAX capture: real Creative hardware vs x86recomp

x86recomp renders Miles' EAX providers with its own environmental reverb
(`src/runtime/audio`). There is no exact reference for what Creative's
cards did with the same EAX properties, so this directory measures it:

- **eaxcap.exe** (Win32, Windows 98 SE or later) drives a game's own
  Miles (`mss32.dll` 6.5 and its `miles\` plug-ins) through a fixed list
  of cases, plays each stimulus as a 3D sample on the chosen 3D provider
  (Creative EAX 1/2/3 on the card, Miles Fast 2D, DirectSound3D, ...) and
  records the card's output ("What U Hear") into a WAV file and a JSON
  manifest per case.
- **eax_render** (Linux) renders the same cases with x86recomp's own audio
  code, driven the way `mss32.cpp` drives it, into the same file layout.
- **analyze.py** aligns both sets, deconvolves the sweeps and compares
  reverb decay, onset, levels and the obstruction/occlusion filters.

Both programs take their cases and stimuli from `cases.c`, so a case means
the same thing on both sides.

## Hardware and drivers

Primary reference: **Sound Blaster Audigy 2 ZS on Windows 98 SE**
(EMU10K2 family, EAX 1-4, period-correct for 2002-2004 games). Secondary:
**X-Fi Titanium on Windows XP**. The same eaxcap.exe runs on both.

Audigy 2 ZS / Windows 98 SE:

1. Creative's last Windows 98 SE / Me driver package for the Audigy 2
   series, and DirectX 9.0c (the last for 98 SE). Check that the card's
   own entries appear in `eaxcap --list` under "DirectSound".
2. Speakers: 2 speakers (or 2/2.1) in Creative's Speaker Settings and in
   Control Panel > Multimedia > Advanced (Speaker setup: desktop stereo).
   No 5.1/7.1: the analysis reads two channels.
3. Turn off everything Creative adds on top of what a game asks for: CMSS
   / CMSS-3D off, and in the EAX console (Audio HQ / EAX Control Panel)
   the master or "global" effect set to no effect. Games' own EAX still
   works; only the card-wide effect has to go.
4. Volume Control: Wave at the top (0 dB) and keep it there for every run;
   other playback sources (CD, MIDI, Line, Mic) muted.
5. Recording: Volume Control > Options > Properties > Recording, tick
   "What U Hear", OK, then tick its Select box (the only selected
   source). This records the card's digital mix before the DAC, at the
   card's native 48 kHz.
6. Set the What U Hear recording level with the level check (below) so
   `level_tone` peaks around -12 dBFS. eaxcap prints the peak of every
   recording; long reverbs at +10 dB late reverb can be 10 dB louder.

X-Fi Titanium / Windows XP: Game Mode (hardware EAX), Crystalizer and
CMSS-3D off, EAX effects in the console off, speakers 2/2.1, Wave at
0 dB, "What U Hear" as the recording source as above, 48 kHz.

Alternative capture on a second machine (if What U Hear is unusable):
send the card's S/PDIF output (Audigy 2 ZS: switch the line out 1 /
digital out jack to digital output in Creative's speaker settings) or its
line output into another computer recording 48 kHz 16- or 24-bit stereo,
start that recording, run eaxcap.exe as usual (it still records What U
Hear, whatever it gets, for its manifests), stop the long recording, and
cut it into per-case files with:

```bash
python3 -I tools/eax_capture/split_capture.py --long spdif.wav --capture capture --out capture_spdif
```

Each recording starts with half a second of silence and a sync click;
the splitter takes the recordings of every provider directory in the
order they were made, finds the clicks in order and cuts every case to
the same length and click position as eaxcap.exe's own file. Then
analyze `capture_spdif` instead of `capture`.

## Building eaxcap.exe (Linux)

```bash
make -C tools/eax_capture
```

This cross-compiles `tools/eax_capture/build/eaxcap.exe` with
`i686-w64-mingw32-gcc` and runs `check_imports.py`, which fails the build
unless every import is in `win98_imports.txt` (functions Windows 98 SE's
kernel32/user32/winmm/dsound and its own msvcrt.dll export), nothing is
imported by ordinal, there is no TLS directory and the subsystem version
is 4.0. eaxcap.exe has no C runtime startup code (its entry point is
`mainCRTStartup` in eaxcap.c) and links libgcc and mingw's math
statically.

## What to copy to the Windows machine

Make a folder with a short path, e.g. `C:\EAXCAP`, containing:

```
C:\EAXCAP\eaxcap.exe          tools/eax_capture/build/eaxcap.exe
C:\EAXCAP\mss32.dll           the game's Miles 6.x mss32.dll
C:\EAXCAP\miles\*.*           the game's miles folder, all of it
```

Take them from the game whose EAX is being measured (Miles 6.5e for the
EAX 3 provider; 6.1c works too: the tool uses only exports both have, and
logs the version it found). In `miles\`, `msseax.m3d` holds the EAX
providers, `msssoft.m3d` Fast 2D and `mssds3d.m3d` DirectSound3D. Copy
them by USB stick, network share or CD. They are the game's files: never
commit them here.

Leave room for the recordings: 16-bit stereo at 48 kHz is 11 MB a minute;
a full EAX 3 run is about 14 minutes (160 MB), EAX 2 13, EAX 1 8, Fast 2D
under 1.

## Running

From a DOS prompt in the folder:

```
cd C:\EAXCAP
eaxcap --list
eaxcap --provider eax2 --cases level_tone
eaxcap --provider eax3,eax2,eax1,fast2d,ds3dhw --out capture
```

- `--list` prints the 3D providers Miles found, the DirectSound and waveIn
  devices and the cases. Use `--in N` if What U Hear is not on the default
  waveIn device.
- The level check plays a 1 kHz tone at -6 dBFS straight ahead; adjust the
  recording level until its peak is about -12 dBFS.
- The full run waits for Enter or Space in the small "eaxcap" window (not
  the console), then records provider by provider; close the window to
  stop. Keep it in front: DirectSound mutes programs without the focus, so
  recording pauses until it is clicked again and a case that lost the
  focus, or whose input overran, is recorded again. The window also keeps
  the screen saver and monitor power-down away.
- Each line shows the case, its length, the recording's peak and the
  latency of the sync click. `NO SYNC CLICK FOUND` means nothing was
  recorded (wrong source or level; after two in a row eaxcap waits for a
  key); `CLIPPED` means the level is too high. At the end of each provider
  it prints an `eaxcap ... --cases` line for any case that failed.
- Every case starts from a complete known EAX state: when a provider opens,
  eaxcap reads each environment preset's "EAX all parameters" from it, and
  before each case it selects another environment, then the case's, writes
  that preset back, and then sets the case's properties; both samples get
  every source property set explicitly. It then reads everything back:
  `MISMATCH` lines (and `state_mismatch` in the manifest) name properties
  that do not read back as intended.
- Other options: `--rate 44100` (default 48000, the Audigy's native rate),
  `--cases env_*,obst_*` (patterns), `--stim impulse,sweep,noise,tone` (in
  place of each case's own stimuli), `--miles DIR`, `--dry-run`, `--yes`.
- Everything printed is also appended to `eaxcap.log` next to the exe.

Output, per provider tag (`eax1`, `eax2`, `eax3`, `fast2d`, `ds3dhw`,
...):

```
capture\eax2\env_05_stoneroom.sweep.wav    the recording, 16-bit stereo
capture\eax2\env_05_stoneroom.sweep.json   everything about it
capture\eax2\stim_sweep.wav                the stimuli, as played
capture\run_<tick>.json                    devices, OS, providers, Miles
```

The manifest records the provider, case, every attribute set (and Miles'
result code), every listener and source attribute read back from the
provider afterwards, the raw "EAX all parameters" block, positions,
timeline, the sync click's position, the recording's peak, the waveIn
device and the OS version. `run_*.json` also lists the attributes each
provider's plug-in enumerates.

Bring the whole `capture` folder back to Linux.

## The cases

Every recording: 0.5 s of silence, then two 3D samples started together:
the sync click (an impulse at 1 m straight ahead, direct path only:
"EAX2 sample room" -10000, or EAX 1's reverb mix 0) and the stimulus,
whose sample begins with 0.25 s of silence. Then the stimulus and enough
time for 1.5 decay times of reverb (from the decay time the provider
reports). The listener is at the origin facing +z; the source is 1 m
ahead at the minimum distance unless the case moves it.

| group | cases | measures |
|---|---|---|
| level | `level_tone` | recording level, no reverb |
| env | `env_00_generic` .. `env_25_psychotic` | EAX's 26 presets (impulse and sweep) |
| eax1 | `eax1_volume_*`, `eax1_decay_*`, `eax1_damping_*` | EAX 1.0's controls |
| listener | `room_*`, `roomhf_*`, `roomlf_*`, `decay_*`, `decayhf_*`, `decaylf_*`, `refl_*`, `refldelay_*`, `reverb_*`, `revdelay_*`, `envsize_*`, `diffusion_*`, `echodepth_*`, `moddepth_*` | one listener property at a time from GENERIC (`listener_base`) |
| source | `obst_*`, `occl_dry_*`, `src_dry_*` | direct-path filters, measured with the reverb off (the recording is the direct path alone) |
| source | `occl_wet_*`, `excl_*`, `src_wet_*` | room-path levels in GENERIC |
| distance | `dist_*`, `dist_air100_*`, `dist_rolloff1_*` | air absorption and room rolloff, 1-32 m |
| position | `pan_az*`, `dist_dry_*` | Fast 2D style panning and distance attenuation, no reverb; runs on every provider |

Cases needing a newer EAX version than the provider are skipped; EAX
attributes on non-EAX providers are logged as skipped. `eaxcap --dry-run`
prints the whole list.

## Rendering with x86recomp and comparing

```bash
cmake --build out/build/debug --target eax_render
out/build/debug/tools/eax_capture/eax_render --out render
python3 -I tools/eax_capture/analyze.py --hw capture --sim render --tsv report.tsv --plot plots
```

eax_render takes the same `--provider`, `--rate`, `--cases` and `--stim`
options (default: fast2d, ds3dhw, eax1, eax2 and eax3, every case). It
opens no audio device: it pulls the mixer offline (`Mixer::OpenOffline`),
with the reverb as in the game, and
sets each voice's levels the way `mss32.cpp`'s `ApplyEax` does. `ds3dhw`
("DirectSound3D Hardware Support", which mss32.cpp does not offer) is the
mixer's DirectSound3D pan and distance law alone, the one the EAX
providers use. Render at the rate the hardware was recorded at. Renders
are 32-bit float WAVs. `--miles-version 6.5|6.1` picks whose Fast 2D the
fast2d set follows (default 6.5, msssoft.m3d; 6.1 is Mssfast.m3d's); the
EAX and ds3dhw sets take the 6.1c gains, as `mss32.cpp` does for EAX.

analyze.py needs numpy (scipy and matplotlib are not used). Either set
can be analyzed alone (`--sim render` works before any hardware data
exists).

It first checks the hardware recordings' EAX state: eaxcap's own
mismatches, focus losses, input overruns and missing sync clicks, and
every property read back against what the case intended: the provider's
own read-back of the case's environment (its `env_*` recording, which
sets nothing on top) with the case's properties applied. A property that
differs and has the previous recording's value was carried over; those
cases are listed as an `eaxcap --provider ... --cases ...` line to record
again. This also works on manifests from the first eaxcap.exe, which did
not reset the EAX state between cases. Read-backs that are no state
problem are accepted: vectors Miles does not return (the EAX 3 pans read
`[-1, 3.4e38, 3.4e38]`: Miles' -1 in the first word, eaxcap's 0x7F fill
after it) count as unreadable, and EAX 3.0's environment reads 26
(`EAX_ENVIRONMENT_UNDEFINED`) once a listener property was set on top of
it. With `--sim`, read-backs that differ from the render's are listed
separately: they show where the model reports a different state than
the hardware, not a bad recording.

For each recording it then finds the sync click (the first sound after
the samples were started), places the stimulus 0.25 s after it,
deconvolves sweeps into impulse responses and prints, per provider, one
row per case with `hardware/render` in each cell. Levels are relative to
each set's own `level_tone` (else `src_dry_ref`, else `pan_az0`), so the
card's gain and the recording level drop out; the raw references are
printed first. The reverb is looked for from the reflections delay the
provider reports, so the direct sound's filter ringing is not counted.

- reverb cases: RT60 mid-band (500 Hz-1 kHz), at 4 kHz and 125 Hz, from
  the reverb-only energy decay curve with the noise floor removed (T20
  when the decay stands 35 dB clear of the noise, T10 at 25 dB, `-`
  otherwise; sweeps give far more range than impulses); EDT; reverb onset
  after the direct sound (ms); early (first 50 ms) and late reverb level
  and wet/direct ratio (dB re a unity direct path);
- dry source cases: direct level, and the direct path's change at
  250 Hz/1k/4k/8k against `src_dry_ref` (the obstruction and occlusion
  shelves);
- wet source and distance cases: direct and reverb levels, absolute and
  against the family's reference case;
- noise cases: level, left/right levels and balance, band levels.

A summary of hardware-minus-render differences of the relative metrics
(decay, onset, wet/direct, the rel_* columns) follows. `--tsv` writes
every metric (per octave band too) in long format; `--plot` writes SVG
energy decay curves and magnitude responses per case.

Balance (R-L) is capped at +-50 dB: beyond that a channel is silent for
the comparison (the hardware's far channel sits at its crosstalk, about
-55 dB, the render's at nothing).

What the renderer still does differently from an Audigy 2 ZS is listed in
ARCHITECTURE.md (audio, "Measured against hardware").

## Running eaxcap.exe through x86recomp (later)

eaxcap.exe is a plain Win32 EXE, so the port side can run it as a third
reference: lift it with `recomp_lift` and run it on the runtime with
`recomp_mss32`. That needs HLE handlers for its imports (a few kernel32,
user32 window and winmm waveIn calls; waveIn would record the mixer's own
output), a `GetProcAddress` that resolves the `_AIL_*@N` names to the
mss32 HLE when the EXE loads mss32.dll itself, and the HLE for the Miles
exports that have none yet (`AIL_close_digital_driver`,
`AIL_3D_sample_attribute`, the attribute enumerators, `AIL_lock`). Until
then eax_render covers the port side.

## Files

| file | |
|---|---|
| `cases.h`, `cases.c` | case list, stimuli, attribute names, WAV header; shared C |
| `eaxcap.c` | the Win32 recorder |
| `Makefile`, `check_imports.py`, `win98_imports.txt` | cross build and the Windows 98 SE import check |
| `eax_render.cpp`, `CMakeLists.txt` | the Linux renderer (`eax_render`, and a ctest smoke run) |
| `analyze.py` | comparison |
| `split_capture.py` | cuts a second machine's long recording into cases |
