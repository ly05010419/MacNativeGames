# Optional cross-device progress sync (Tears of the Kingdom)

This implements the player workflow in #798: continue the last completed in-game
save on a Mac, iPad or iPhone, keep offline branches, and recover earlier progress.
It uses a player-selected folder in ordinary iCloud Drive and does not require an
iCloud container entitlement or a paid Apple developer team. Free signing still
has its normal installation and renewal limits.

## Enable and use

- Mac: open **Game Progress** from the application's menu.
- iPhone/iPad: long-press the Home Screen game icon and choose **游戏进度**.
  Opening management this way does not start the game; **Play / 进入游戏** starts it.
- On each device, choose the same iCloud Drive folder once. The system picker
  grants access; a security-scoped bookmark remembers it.
- Save in the game, leave the source online briefly, then start the game on the
  other device. iCloud propagation can take time. The Files app is not needed
  for routine discovery or downloads.
- A game already running only stages new remote records. Exit and reopen to
  apply them. Returning to a running game never presents sync alerts; news stays
  in the progress view or the next pre-game launch. Remote files never replace
  the running game's working directory.
- If devices independently continue the same older save, both branches remain.
  Choose a branch in management. The other branch remains available in history.
- History and named backups can be restored. Restoration is a new selection
  event; it does not overwrite the old immutable version. History is retained
  without automatic pruning in this first implementation.

Cloud sync is opt-in. Local saves remain available offline or when sync is
paused. Cancelling the initial picker leaves the existing save path in use.

## Storage and safety

`progress_store.m` stores immutable records in `<profile>/.progress/records`,
a separate managed working generation in `work`, and a durable `state.json`
selecting the generation. Account identity and the complete game save belong to
one version. Device graphics, controls, game images, keys, shaders and logs are
not part of the synchronized payload. Original unmanaged saves remain in place.

`fs_host.c` serializes save mutations against snapshot creation and calls the
commit boundary before a successful save commit returns. Cloud I/O remains
asynchronous. The runtime reads account identity and save files from the managed
profile while device-specific files keep their original location.

Records include parents, game version, file lengths and SHA-256 digests.
Ancestry determines whether a version is a successor or a separate branch;
timestamps are for display. Explicit selection can resolve multiple heads.
`CacheStorageKey.dat` stays in the snapshot but does not alone create a new
player-progress version.

`progress_transport.m` uses coordinated file access, requests downloads for
ubiquitous items and retries incomplete records. A directory presenter and a
15-second foreground timer supplement one another. A record is accepted only
after its identity, paths, bounds and all file hashes validate. Incomplete or
invalid inputs stay staged and cannot become the active generation. Local
application selects a complete generation transactionally.

Receipts mean a device possesses and verified a version. They do not prove that
its running game has loaded it. A local upload does not prove another device
received it, and an unchanged cloud listing does not prove every device is
current.

## Scope and limitations

- Implemented for TOTK. BOTW has no management shortcut or startup integration.
- No real-time server coordination, cross-device exclusive lock, merging of
  different game worlds, or transfer of unsaved in-memory state.
- iCloud availability and propagation are controlled by the OS. An app that is
  suspended, terminated or offline cannot promise immediate synchronization.
- Moving or deleting the selected folder can require selecting it again.
- Full version records duplicate save content. History storage grows until an
  explicit retention policy is implemented.
- This is a separate proposal from #785's entitled iCloud-container backend and
  its current generation/last-save conflict policy. The formats are not
  interchangeable. They must not both write the same active profile.

## Verification

`python3 -m unittest discover -s tests` runs the store, coordinator, transport,
commit-gate and full-volume probes using synthetic data only. The native probes
are macOS-only; packaging tests also check the TOTK shortcut and BOTW exclusion.

The pre-rebase implementation was built for macOS and iOS and completed one real
TOTK 1.4.3 Mac → iPad → iPhone → Mac save/load round with user-authorized iCloud
Drive folders and free-team signing. That device round is evidence for the
pre-rebase build, not a claim that this rebased PR was installed and played.
Current PR build/test results and remaining platform gaps are recorded in the PR
body. Long-term reliability, every game version, visionOS device behavior and
immediate lock-screen propagation have not been fully validated.

Only source, synthetic tests and this document belong in the contribution. No
real save files, device evidence, private profiles, game content or credentials
are included.

Progress UI, status and recovery messages follow the existing `TOTK_UI` Chinese/English selector (`TOTK_UI_LANG` can override it). Static Home Screen shortcuts use localized `InfoPlist.strings` resources with an English fallback. Player-created backup names and record metadata are not translated.
