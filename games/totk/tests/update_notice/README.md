# Mac port update notice

The packaged Zelda launcher checks public GitHub metadata on each launch, after
its single-instance checks and before starting the game. `MNGSourceRevision` in
Info.plist records the Git checkout's HEAD when packaging. A source archive without
Git metadata skips the check. Local patches are not an official release version.

A comparison against upstream main triggers a notice for relevant shared/runtime
changes. Fork divergence is allowed only when upstream has new commits; changes
only on the fork do not cause a notice. Documentation, tests and iOS-only changes
are filtered; GitHub's truncated file lists are treated conservatively.

Play continues normally; View changes opens a constructed GitHub compare URL;
Skip this update suppresses the current head until a different one appears.
There is no download, installation or daily check throttle. The two requests share
a three-second deadline. Offline/error/invalid metadata continues startup silently.
`MNG_CHECK_UPDATES=0` opts out. The notice follows the existing Chinese/English host
UI language policy. Only public repository metadata is sent/requested; no keys,
saves, playtime, credentials or paths are uploaded.

Run `make -C games/totk probe-update-notice` for offline metadata-policy tests.
Mac launcher compilation covers all supported Zelda version defines. A complete
new app/IPA build is not required by this probe and is not claimed by it.
