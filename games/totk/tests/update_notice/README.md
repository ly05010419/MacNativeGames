# Mac port update notice

The packaged Zelda launcher reads cached public GitHub metadata after its
single-instance check. It never waits for a network request. A relevant cached
update offers Play, View changes or Skip this update before the game starts.
The first launch without a cached result starts normally.

The launcher passes the full build revision and data folder to the Mac runtime.
`runtime_front_run` schedules a utility-queue check, without waiting or presenting
UI. The result is available on a later launch. Keeping the worker in the runtime
matters: the launcher's `execv` would terminate a background thread started there.
There is no extra process, app copy or downloaded game content.

Checks happen at most once per 24 hours per game's data folder, including offline
and failed attempts. A small atomic `port-update-state.plist` records the attempt
before networking and caches the comparison's base/head. A nonblocking file lock
prevents concurrent checkers. The two public GitHub requests share a three-second
background deadline. A clock moving backwards does not trigger repeated retries.
Deleting the cache resets the interval; a missing/unwritable folder skips checks.

`MNGSourceRevision` in Info.plist identifies the packaging checkout's full HEAD;
main's existing short `MNGSourceCommit` remains available to the HUD. Archives
without Git metadata skip checks. Cached results from a different build are not
shown. Fork divergence counts only when upstream has additional commits. Docs,
tests and iOS-only changes are filtered; truncated file lists are conservative.

Play continues normally. View changes opens a URL constructed from validated
hashes. Skip stores that revision in `port-update-skipped.txt`, independently of
background cache writes; the previous cache's `skipped` field is still respected.
`MNG_CHECK_UPDATES=0` disables both notices and background requests. Notices follow
the existing Chinese/English UI policy. Requests contain only public repository
metadata, never credentials, keys, saves, playtime or user paths. This announces
source changes, not a prebuilt release or a guarantee of compatibility.

## Focused validation for the review follow-up

`make -C games/totk probe-update-notice` uses synthetic metadata and a temporary
folder that it removes. It covers cache/base validation, the 24-hour boundary,
offline attempts, concurrent locking, persistent Skip, installed-head clearing,
opt-out and a deliberately blocked fake network to verify scheduling returns.
No live network, alert or game is needed. Launcher/frontend syntax was checked for Mac TOTK 1.0.0 and BotW 1.6.0. The focused
synthetic packaging test could not import `cryptography` in the available Python
environment; no new dependencies were installed for this follow-up. No full test suite, game launch,
complete app build or iOS/device test was performed for this revision. The changed
frontend is Mac-only; the iOS frontend is separate.
