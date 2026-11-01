# Horizon services used by XC3

One row per service and command the game actually calls, filled in from boot logs (stage 4 onward). The runtime stops with a named failure on any service it does not answer. `TOTK_TRACE_SVC=1` traces syscalls and `TOTK_IPC_DUMP=1` dumps raw IPC messages.

Status values:
- **real**: answered with real data from the Mac or the dump.
- **stub-ok**: an empty or fixed answer that the game accepts, with the reason given.
- **TODO**: called but not answered correctly yet.

Most SDK calls run unchanged inside `sdk`. The runtime replaces a function by its symbol (`runtime_*_override` tables) where the SDK would reach a Horizon service or shared memory the host does not have.

| Service / function | What the game does with it | Answer | Status | File | Since |
|---|---|---|---|---|---|
| `nn::fs::IsSaveDataExisting(Uid const&)` and `(bool*, Uid const&)` | checks its own save before mounting it, right after `nnMain`; without a hook the SDK opens `fsp-srv` | true for the local account (the save folder exists once storage is ready) | real | `runtime/fs_host.c` | 2026-10-28 |
| `nn::hid::GetTouchScreenStates<16>(TouchScreenState<16>*, int)` | reads touch history; without a hook it reads unmapped hid shared memory (fault at 0x18) | one state with no touches, returns 1 | real (the Mac has no touch screen) | `runtime/input.c` | 2026-10-28 |
| `nn::oe::Enable/DisableAlbumImageTakenNotification()` | asks to hear about screenshots; without a hook the SDK's `oe` object is null (fault at 0) | nothing (no album on the Mac, so no notification ever comes) | stub-ok | `runtime/applet_host.c` | 2026-10-28 |
| `nn::oe::IsUserInactivityDetectionTimeExtended()`, `GetUserInactivityDetectionTimeExtensionMode()` | reads back the auto-sleep setting it sets | the value last set through `SetUserInactivityDetectionTimeExtended` (0 or 1) | stub-ok (macOS handles sleep) | `runtime/applet_host.c` | 2026-10-28 |
| `nn::account::ListAllUsers(int*, Uid*, int)` | lists the console's users | the one local account | real | `runtime/account.c` | 2026-10-28 |
| `nn::prepo::AddSessionId(PlayReport*, const char*)` | tags a play report; without a hook the SDK opens `prepo:u` | success (no report is ever sent) | stub-ok | `runtime/applet_host.c` | 2026-10-28 |
| `nn::oe::GetDisplayVersion(DisplayVersion*)` | 2.2.1 reads its own version while starting (1.0.0 never calls it); without a hook the SDK's `IApplicationFunctions` proxy is null (fault at 0 in `sdk+0x33a4c4`) | the build's version string (`TOTK_GAME_VERSION`) | real | `runtime/services.c` |
| `nn::ec::ShowShopAddOnContentList(ApplicationId, UserHandle const&)` | 2.2.1's DLC menu, "Purchase": opens the eShop library applet; without a hook the SDK's `CreateLibraryApplet` reads a null proxy (fault at 0) | returns at once, as if the shop were closed (no eShop, no library applets) | stub | `runtime/system_services.c` |
| `nn::aoc::CountAddOnContent`, `ListAddOnContent`; `nn::fs::MountAddOnContent`, `QueryMountAddOnContentCacheSize` | the add-ons installed, and their data mounted as `aoc1`–`aoc4` | 2.2.1: the verified add-ons in `extracted/aoc/<index>` (from `scripts/extract_aoc.py`), mounted read-only; cache size 0; 1.0.0: none | real | `runtime/fs_host.c`, `runtime/services.c` |
| `nn::oe::GetDesiredLanguage()` | picks the text and voice language | the Mac locale mapped to a language this version supports (control.nacp) | real | `runtime/services.c` | 2026-10-28 |
| `audren:u`, `audout:u` (IPC) | the audio renderer (Wwise) and an AudioOut stream, 48 kHz stereo PCM16 | Core Audio | real (as in ot0) | `runtime/audio_*.c` | copy |
| The ot0 set (`fs`, `hid` npad, `account`, `set`, `time`, `vi`, `bsd`, `ssl`, …) | as inherited | as in `games/ot0` | not reviewed per call yet | `runtime/*` | copy |

## Audit of unanswered imports (2026-10-28)
From `analysis/runtime-layout.json`: the imports of main and subsdk0 in service namespaces that no `runtime_*_override` table names. These are left unanswered on purpose:
- `nn::prepo::PlayReport/Array/Struct` builders, `nn::settings::operator==`, `nn::time::operator-` and `ClaimPeriodicBenefit…`: client-side code, run in `sdk` (the save path is hooked).
- `nn::hid::Vibration*` classes: they reach `nn::hid::detail`, which `hid_feedback.c` answers.
- `nn::audio::*AudioOut*` and resamplers: the AudioOut calls go through `audout:u` IPC, which is answered; the resamplers are pure code.
- `nn::settings::GetAvailableLanguageCode*`: `set` IPC, which is answered.
- `nn::vi::CloseDisplay/DestroyLayer/Finalize`: only at exit.
- `nn::nfp::GetCommonInfo/Unmount`: only after a mount, which already fails (no device).

Still open: `nn::album::SaveCurrentScreenshot`. Add it when the game first calls it.

## Notes
- A stub that returns success where the game needs data is a bug. Write down why each stub is enough.
- Do not copy service code from an emulator. Reading one for the IPC layout is fine; cite which one and what was read.
