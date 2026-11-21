# The rewriter's patches of Nintendo SDK module builds (sdk), which several games can ship: dm/tools/patch_nso.py applies those of a module's NSO build ID, whatever the game.
# A patch of a game's own module (main) goes in its profile's patches.py (dm/games/<id>/patches.py). CUTS as described in patch_nso.py ("Registered patches")
CUTS = {
  # Kingdom Battle 1.9.589692, SDK 4.4.0: the older VammManager reserves a
  # full 1 GB node area (constructor +0x93dd8, occupied range +0x93e98).
  # Snowdrop's compact 3 GB region follows its small metadata reservation;
  # 1 GB placement alignment still cannot fit it in the iPhone guest block.
  # Platform-limit cuts: 64 MB node area and 32 MB large-region alignment.
  '9de92bad67c4150268943ab4843ace65': [
    (0x93dfc, 'e9030232', '0980a052', 'VammManager node area limit: 1 GB to 64 MB'),
    (0x93e8c, 'ea030232', '0a80a052', 'VammManager occupied node area: 1 GB to 64 MB'),
    (0x93fe0, 'ea030232', '0a40a052', 'AllocateAddressRegion: align regions of 1 GB and more to 32 MB'),
  ],
  # Pokemon Brilliant Diamond 1.3.0 (SDK 11.4.3): nninitStartup (main+0x2c1d5c) gives pool - used to StandardAllocator with virtual address memory.
  # Its first CentralHeap::Init aborts at sdk+0x2f98e4: the 1 GB node area leaves no room for the 1 GB-aligned 3.14 GB reservation in a 4.375 GB alias region.
  # The constructor (sdk+0x34e0b8) is the same mechanism as Engage's below: the limit and the occupied length must both shrink to 62 MB.
  # The SDK-wide alignment rule matches this build too (sdk+0x34e2c4, checked 2026-10-29); this row keeps it out until a game shipping the build is run with it.
  '2d369d9999e3e10e665106f840ffff9b': [
    (0x34e0c8, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x34e164, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Fire Emblem Engage 2.0.0 (v327680) sdk: AddressRegionManager node area 0x3fe00000 (about 1 GB) → 0x3e00000 (62 MB).
  # The constructor (sdk+0x346420) carves the node area out of the start of the alias region (a unit heap of 0x30-byte nodes, mapped only as used) before AllocateAddressRegion hands out regions.
  # The hardware alias region is 64 GB, so the 1 GB does not matter there; the host's alias region is only 5.375 GB (iOS caps a single reservation at 6 GB), and after StandardAllocator's 3.14 GB block (1 GB aligned)
  # Unity's low-level virtual allocator (main+0x9aa0f4 → AllocateAddressRegion) had only about 1.2 GB left; about 357 s into the prologue battle it got no region → returned 0 → null write crash at main+0x9a4664.
  # 62 MB holds about 1.35 million nodes (a few hundred regions are live in measurements). Both movs must change together: +0x346430 sets the node area limit, +0x3464cc registers it as an occupied region.
  '56ec24747ec9e7f19b3efb062483a19d': [
    (0x346430, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x3464cc, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Ace Attorney Investigations Collection 1.0.0.2 (v131072) sdk (SDK 17.5.3): the same node area cut as Engage's. Without it nninitStartup's StandardAllocator::Initialize(nullptr, pool − used)
  # (main+0x874b6c, virtual address memory) aborts at its first CentralHeap::Init: the 1 GB-aligned region of 3.14 GB did not fit in the 4.375 GB alias region after the node area.
  # The constructor is at sdk+0x139458 (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied)
  # The SDK-wide alignment rule matches this build too (sdk+0x13965c, checked 2026-10-29); this row keeps it out until a game shipping the build is run with it.
  '311deab1f062b504f3d646a49ddcd4e8': [
    (0x139468, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x1394fc, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Brain Memory Training Academy 1.0.0 (v0) sdk (SDK 16.2.4, the same build as Unicorn Overlord 1.05's, which has no virtual address memory and never allocates an address region):
  # the same node area cut. nninitStartup (main+0x73ad60) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory, which aborted at its CentralHeap::Init.
  # The constructor is at sdk+0x134768 (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x13480c)
  # The SDK-wide alignment rule matches this build too (sdk+0x13496c, checked 2026-10-29); this row keeps it out until a game shipping the build is run with it.
  '12f161039300861f1054371f0e36b8c9': [
    (0x134778, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x13480c, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Patrick's Parabox 1.0.1 (v65536) sdk (SDK 13.3.7, Unity 2020.3.30f1): the same node area cut. nninitStartup (main+0x1cd010) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual
  # address memory, which aborted at its CentralHeap::Init (sdk+0x324dec). The constructor is at sdk+0x37c3a0 (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then
  # registered as occupied at +0x37c44c)
  '9c73fb73f529fedc9ffefa28246711cc': [
    (0x37c3b0, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x37c44c, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Hollow Knight: Silksong 1.0.30000 (v655360) sdk (SDK 20.5.8, Unity 6000.0.50f1): the same node area cut. Unity 6 takes all its memory itself through nn::os::AllocateAddressRegion and
  # AllocateMemoryPages (no heap, no StandardAllocator in nninitStartup), and without the cut its regions start after the node area: the alias region's high-water mark was +1.00 GB at
  # nninitStartup and +3.00 GB in the first room of play, with 1.51 GB mapped. The constructor is at sdk+0x145bec (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte
  # nodes, the area then registered as occupied at +0x145c68)
  'e01950c58e17b080c90494784ea1dd29': [
    (0x145bfc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x145c68, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
}

# SDK-wide function patches (dm/tools/nso_match.py): the two fixes above that hold for any game on an iPhone's small address space, for every game's sdk of a build with no CUTS
# (patch_nso.py "Registered patches"), so a new game or a new version of one gets them without a cut for its build. The Switch's alias region is
# 64 GB, where neither matters; the host's is about 4.4 GB:
# - the AddressRegionManager constructor's node area: 0x3fe00000 (about 1 GB) at the start of the alias region → 0x3e00000 (62 MB, about 1.35 million 0x30-byte nodes; a few hundred
#   regions are live in measurements). Both instructions change together: the limit, and the length registered as an occupied region.
# - VammManager::AllocateAddressRegion's alignment of the large size classes: the placement aligns a region by its size (64 KB under 2 MB, 2 MB under 4 MB, 4 MB under 32 MB, 32 MB
#   under 1 GB, else 1 GB) and starts the next region at that alignment past a 4 KB guard; the three classes from 4 MB up take 2 MB, as SDK 19.3.14 and SDK 20 do themselves (they align
#   to 64 KB or 2 MB only: Cult of the Lamb's, Tears of the Kingdom's, FINAL FANTASY RESONANCE's builds). With 1 GB a region of 1 GB or more lost up to 1 GB of the alias region before it
#   (Absolum, Paper Mario, Smash, Pokémon Scarlet); with 4 MB and 32 MB, allocators that reserve a little more than a class (Unity's N MB + 252 KB) lose most of the next class to the
#   gap: Sea of Stars (SDK 16.2.0) had 2.59 GB of live regions with 0.86 GB of gaps between them in the prologue, its alias high-water mark at +4.26 GB of 4.375 GB with a 1.5 GB pool (with the hardware pool Unity got no
#   region and wrote at NULL + 0x3fffe0, main+0x48b770); Subnautica: Below Zero (SDK 13.3.5) 336 live regions holding 3.45 GB with 0.87 GB of gaps, ending at +4.37 GB. Nothing in the SDK
#   or the games measured reads a region's alignment. The three constants sit at +0x64 (32 MB, w11), +0x6c (1 GB, w10) and +0x70 (4 MB, w8) in every implementation below.
# One rule per reviewed implementation: a function size and cut offsets, with the fingerprints of every registered build that has them (the same body, other callees). Each was
# taken from that build's CUTS above and resolves to exactly the registered cuts there; on the 36 sdk builds on the game drives each rule matched once or not at all (2026-10-28).
# Older SDKs (Fire Emblem: Three Houses, Bayonetta, Donkey Kong Country: Tropical Freeze, Xenoblade Chronicles 2, Yoshi's Crafted World) have other implementations, not reviewed.
# Builds whose registered rows these rules reproduce exactly (20, checked on the game drive, 2026-10-29, 2026-10-30), or reproduce with the 2 MB alignment more and a game run with
# it (3, 2026-10-30), so their rows were removed: what was found on each
# - 3053b229…: Sea of Stars 2.0.59001 (v786432) sdk (SDK 16.2.0, the build of Super Mario Bros. Wonder 1.0.0 / 1.0.1): the node area cut and the 2 MB alignment, measured on Sea of Stars
#   (above). nninitStartup (main+0x8b66a0) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory, which aborted at sdk+0x126440 (Initialize+0xc0). The
#   constructor is at sdk+0x134718, the placement at sdk+0x1348b0. Unity 2021.3's virtual allocator (main+0x48deb0 → main+0x12e12c0) reserves N MB + 252 KB and aligns its blocks to 256 KB
#   itself, so with the SDK's classes each of its 94 regions of 16.25 MB took a 20 MB slot and each 4.25 MB one an 8 MB slot
# - 3644a676…: AI: THE SOMNIUM FILES - nirvanA Initiative 1.0.2 (v131072) sdk (SDK 13.3.5, Unity 2020.3.30f1; also Pokémon Scarlet 1.0.0 and Subnautica: Below Zero 1.23.55063, Unity
#   2019.4.36f1): the node area cut and the 2 MB alignment, measured on Below Zero (above, with DM_REGIONS at the crash site of the opening: 92 regions of 2.25 MB, 60 of 4.25 MB, 33 of
#   16.25 MB, 2 of 32.5 MB). nninitStartup (main+0x493300) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory. Scarlet's 12 GB allocator region gets
#   4.06 GB from the host's StandardAllocator cap and fits only with the node area cut and a large-class alignment under 1 GB (0x1803 otherwise). The constructor is at sdk+0x37c130, the
#   placement at sdk+0x37c2d0; Mario + Rabbids Sparks of Hope's SDK 13.3.5 build (43211771…, below) has the same code
# - b39add63…: Pokémon Legends: Arceus 1.1.1 (SDK 11.4.0, Kirby and the Forgotten Land's sdk build): nninitStartup (main+0x2779d8c) initializes StandardAllocator over 32 GB with virtual
#   address memory; the host's cap gives it 4.06 GB, which fits the 4.375 GB alias region only after a 62 MB node area and with 32 MB alignment (the region placement at sdk+0x34e1d8 aligns
#   regions of 1 GB and more to 1 GB; without it, Break at sdk+0x39fb54). The SDK-wide rules' cuts (node area implementation 3, alignment implementation 2), registered because both profiles
#   were brought up on this build; Kirby has no virtual address memory and never reaches them
# - fcf93a41…: DRAGON QUEST XI S 1.0.4 (v262144) sdk (SDK 7.3.2, the same build as Super Mario Odyssey 1.4.1's, which has no virtual address memory and never allocates an address region):
#   the node area cut, and regions of 1 GB and more aligned to 32 MB, as Absolum's. Unreal Engine 4.18's nninitStartup takes one region for the whole engine, which a NumberLineAllocator
#   (main+0x177c4d0) hands out (16 GB by default; the host gives it what the alias region has room for, dm/tools/ue_region.py); the 1 GB alignment would have put it 1 GB into the alias region.
#   The constructor is at sdk+0x32c450 (the limit at +0x28 and the occupied length both from orr w9,wzr,#0x3fe00000); VammManager::AllocateAddressRegion's placement at sdk+0x32c600 aligns a
#   region by its size as SDK 18 does: 64 KB under 2 MB, 2 MB under 4 MB, 4 MB under 32 MB, 32 MB under 1 GB, else 1 GB (+0x32c66c)
# - 30a1e87b…: SDK 10.4.8 sdk 30a1e87b52f5 (Pikmin 4 1.1.1, Unreal Engine 4.26.2): the same cuts as the SDK 10.4.1 build above, whose AddressRegionManager this is at the same offsets (the
#   constructor sdk+0x340e90, the node area limit +0x340ea0 and its occupied length +0x340f3c, AllocateAddressRegion's 1 GB alignment +0x34109c). Unreal's nninitStartup reserves its engine
#   region in the alias region (what the alias region has room for: dm/tools/ue_region.py), so the region starts 62 MB in and a 4 GB region needs no 1 GB alignment slack
# - 86188952…: SDK 15.3.2 sdk 8618895246b4 (Paper Mario: The Thousand-Year Door 1.0.1, Celeste 1.4.1.1; Celeste has no virtual address memory and never makes an address region): Absolum's
#   cuts on this build. The AddressRegionManager constructor is at sdk+0x12e0e0 (node area limit at +0x12e0f0, registered as occupied at +0x12e184) and VammManager::AllocateAddressRegion at
#   sdk+0x12e1d0 aligns regions of 1 GB and more to 1 GB (+0x12e2e0). Measured with a 64 GB alias region: Paper Mario's malloc region was placed at the next 1 GB boundary after the 1 GB node
#   area, 0.69 GB further
# - 574284b4…: SDK 20.5.22 sdk (The Legend of Zelda: Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0, the same build): the same node area cut. Tears of the Kingdom's resource heaps
#   live in address regions after the node area. At the first descent from the Great Sky Island the alias region's high-water mark was +2.25 GB of its 2.375 GB with 1.00 GB mapped, and the
#   game then took a memory shortage: its frame check (main+0x1fd32ec) logs MSF / R / Rf and plays the memory-shortage Blood Moon, which puts Link back at the start (GitHub issue #70; 7 of 8
#   Mac runs from the issue's save). With an 8 GB alias region the mark went on to +2.50 GB, with this cut it stays at +1.75 GB, and neither showed the shortage. The constructor is at
#   sdk+0x145cfc (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x145d78); VammManager::AllocateAddressRegion
#   (sdk+0x145df0) aligns regions to 64 KB or 2 MB only, so no alignment cut is needed
# - e8cdceb1…: Xenoblade Chronicles 3 2.2.1, SDK 12.3.9: the same AddressRegionManager constructor and size-class alignment as Engage's SDK 12.3.7. Its cut 4 GB malloc reservation must fit
#   the 4.375 GB alias region on iOS.
# - 86a2afd8…: Paper Mario: The Origami King 1.0.1 (v65536) sdk (SDK 8.3.1): the SDK-wide rules' cuts (node area and alignment implementation 1, the constructor at sdk+0xc96e0), registered
#   because the profile was brought up on this build. nninitStartup initializes a StandardAllocator over 62 GB with virtual address memory (the region returns to main+0x5c05ac); the host's
#   cap gives it 4.06 GB, which fits the 4.375 GB alias region only after the 62 MB node area and with 32 MB alignment. Blasphemous 1.0.8 ships this build too; it has no virtual address
#   memory (no system resource) and never reaches them
# - 5fd4f531…: SDK 8.2.1 sdk 5fd4f531c119 (Super Smash Bros. Ultimate 13.0.5; Catherine: Full Body 1.0.1a, which has no virtual address memory and never makes an address region): the node
#   area cut, and regions of 1 GB and more aligned to 32 MB, as DRAGON QUEST XI S's SDK 7.3.2 build, whose AddressRegionManager this is at other offsets. Smash's engine reserves one region
#   for all its memory (cut to 4 GB in its patches.py); with the 1 GB node area and 1 GB alignment it started 1.95 GB into the alias region (measured with a 64 GB one). The constructor is at
#   sdk+0x336b40 (the limit at +0x336b50 and the occupied length at +0x336bf0, both orr w9,wzr,#0x3fe00000); VammManager::AllocateAddressRegion aligns a region by its size (+0x336d5c)
# - f118ae1c…: Absolum's sdk (SDK 18.3.2, the same build in 1.1.1 v393216 and 1.1.2 v458752; the AddressRegionManager at the offsets of Galaxy's SDK 18.3.5 build): the same node area cut,
#   and regions of 1 GB and more aligned to 32 MB instead of 1 GB. VammManager::AllocateAddressRegion (sdk+0x13bfd0) aligns a region by its size: 64 KB under 2 MB, 2 MB under 4 MB, 4 MB
#   under 32 MB, 32 MB under 1 GB, else 1 GB (+0x13c03c), and places it first-fit after the regions already there. Absolum's two large regions (main's malloc, and the .NET GC's region range
#   from the game_env) are both over 1 GB, with a 4 KB region between them, so the alignment lost up to 1 GB before each one (measured with a 64 GB alias region: 0.6 GB before malloc's at
#   that run's alias base, 1 GB before the GC's), which the 64 GB alias region of a Switch never misses and an iPhone's 4.375 GB cannot spare. 32 MB alignment is what the SDK gives regions
#   from 32 MB up; nothing in the SDK or the game reads the region's alignment
# - 0023a1b0…: FINAL FANTASY RESONANCE 1.0.1 (v65536) sdk (SDK 20.5.17, the build STEINS;GATE RE:BOOT runs, which has no virtual address memory): the AddressRegionManager node area cut, as on the other
#   Unreal games' SDKs. The constructor is at sdk+0x145cac: +0x145cbc sets the node area limit, +0x145d28 registers it as an occupied region
# - 145dde94…: DRAGON QUEST VII Reimagined 1.2.0 (v131072) sdk (SDK 19.3.1): the same AddressRegionManager constructor as Octopath Traveler 0's SDK 19.3.13 build below, at the same offsets
# - 43211771…: Mario + Rabbids Sparks of Hope 1.6.2225577 (SDK 13.3.5): the same constructor and placement code as nirvanA Initiative above. Snowdrop reserves a small metadata region before its three-
#   pool range (reduced from 12 GB to 3 GB in the game's patches.py). Both the 62 MB node area and 32 MB alignment are needed: with the original 1 GB alignment the 3 GB request returns
#   0x1803, even after the node-area cut. Its own pools align to 64 KB / 512 KB and do not require 1 GB alignment of the enclosing address region.
# - 7bba185a…: Octopath Traveler 0 1.0.5 (v327680) sdk (SDK 19.3.13): the AddressRegionManager node area 0x3fe00000 → 0x3e00000 (62 MB), as Engage's SDK 12 (above). The same constructor (sdk+0x1423ec):
#   +0x1423fc sets the node area limit, +0x142468 registers it as an occupied region. Unreal's region (what the alias region has room for: dm/tools/ue_region.py) then starts 62 MB into the
#   alias region, and the guest block stays small enough for an iPhone next to the Unreal heap (hle_uemalloc.c), which lives outside it
# - 871fb219…: Cult of the Lamb 1.5.28 (v1966080) sdk (SDK 19.3.14, Unity 2022.3.62f3): the same node area cut, the SDK-wide rule's implementation 5 (the constructor at sdk+0x1423ec), registered while
#   the rules skipped builds a profile was brought up on (not on the game drive to check against them). nninitStartup calls StandardAllocator::Initialize(nullptr, pool − used) with virtual
#   address memory (the region returns to main+0xb7e45c). Region placement aligns only to 64 KB or 2 MB (sdk+0x142544), as SDK 20, so it needs no alignment cut
# - 8bfebccd…: SDK 20.5.19 sdk 8bfebccd6f5c (BALL x PIT 1.293, Unity 6000.0.62f1): the same node area cut, for the same reason as Hollow Knight: Silksong's (Unity 6 takes all its memory through
#   AllocateAddressRegion). The AddressRegionManager constructor is the SDK 20.5.22 build's above at the same offsets (sdk+0x145cfc, the limit at +0x145d0c, the occupied length at +0x145d78)
# - d9add76f…: SDK 10.4.1 sdk d9add76fc264 (Advance Wars 1+2: Re-Boot Camp 1.0.0, Unity 2020.2.1f3; The Great Ace Attorney Chronicles 1.0.0, which has no virtual address memory and never makes an
#   address region): the node area cut, and regions of 1 GB and more aligned to 32 MB, as DRAGON QUEST XI S's SDK 7.3.2 build, whose AddressRegionManager this is at other offsets.
#   nninitStartup (main+0x365310) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory, which aborted at its CentralHeap::Init (sdk+0x2ee060), and that
#   region is more than 1 GB. The constructor is at sdk+0x340e90 (the limit at +0x340ea0 from orr w8,wzr,#0x3fe00000, the occupied length at +0x340f3c from orr w9,wzr,#0x3fe00000);
#   VammManager::AllocateAddressRegion (sdk+0x341030) aligns a region by its size: 64 KB under 2 MB, 2 MB under 4 MB, 4 MB under 32 MB, 32 MB under 1 GB, else 1 GB (+0x34109c)
# - df7a3960…: Pokémon Legends: Z-A 2.0.2 (v393216) sdk (SDK 19.3.11): the same node area cut. The constructor is at sdk+0x1423ec (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte
#   nodes, the area then registered as occupied at +0x142468). With main's allocator region below, the guest block fits the 5 GB proven on a phone
# - fed212d6…: EA SPORTS FC 27 1.8d.310c (v393216) sdk (SDK 22.2.0, also Well Dweller's, which has no virtual address memory): the same node area cut, the SDK-wide rule's implementation 6 (the
#   constructor at sdk+0x14def0), registered because the profile was brought up on this build. Frostbite's VirtualAlloc reserves 264 regions, 2.8 GB, in its first minute (main+0x17f9884,
#   AllocateAddressRegion), which the 1 GB node area pushed past the alias region. Region placement aligns only to 64 KB or 2 MB (sdk+0x14e060), as SDK 20
# - 2aef2cff…: Batman: Arkham Knight 1.0.5 (v327680) sdk (SDK 14.3.0, Unreal Engine 3; also Persona 4 Golden 1.0.2, which has no virtual address memory and never reaches them): the node
#   area cut, registered while the rules skipped builds a profile was brought up on, and now the 2 MB alignment (run on Batman, 2026-10-30). The constructor is at sdk+0x12d8a0, the
#   placement's constants at sdk+0x12daa4 / +0x12daac / +0x12dab0. With the engine's allocator on the host the route makes no region of 256 MB or more, so the run shows only that nothing
#   else moved
# - 60751516…: Super Mario Galaxy 1.3.1 / Super Mario Galaxy 2 1.4.0 sdk (SDK 18.3.5, the same build in both; also Monster Train 2 and Donkey Kong Country Returns HD): the node area cut
#   and now the 2 MB alignment (run on Monster Train 2, 2026-10-30: its 0.94 GB region stays at alias +0.06 GB). Galaxy 2's movie player initializes its StandardAllocator with virtual
#   address memory (main+0x62c280: Initialize(nullptr, 384 MB)), which aborted at its CentralHeap::Init: a 256 MB alias region holds no region after a 1 GB node area. The constructor is
#   at sdk+0x13be3c (the limit at +0x13be4c, the area registered as occupied at +0x13bee0), the placement's constants at sdk+0x13c034 / +0x13c03c / +0x13c040
# - 5bcf525f…: OCTOPATH TRAVELER II 1.1.1 (v131072) sdk (SDK 12.3.2, Unreal Engine 4.27): the node area cut, as DRAGON QUEST VII Reimagined's, and now the 2 MB alignment (run on the
#   game, 2026-10-30: its 2 GB region moves from alias +0.75 GB to +0.06 GB). The constructor is at sdk+0x346180 (the limit at +0x346190, the occupied length at +0x34622c), the
#   placement's constants at sdk+0x346384 / +0x34638c / +0x346390. Unreal's engine region (dm/tools/ue_region.py) starts 62 MB into the alias region
node_area, alignment = 'AddressRegionManager node area 1 GB → 62 MB', 'AllocateAddressRegion: regions of 4 MB and more aligned to 2 MB, as SDK 20'
def mov(old, value):   # the original mov / orr of a constant, as the registered reasons name it (its destination register: the word's low 5 bits)
    rd = int(old[:2], 16) & 31
    return f'orr w{rd},wzr,#{value:#x}' if int(old[6:8], 16) == 0x32 else f'mov w{rd},#{value:#x}'
def node(size, fingerprints, limit, limit_old, limit_new, occupied, occupied_old, occupied_new):
    return {'name': node_area, 'size': size, 'fingerprints': fingerprints, 'cuts': [
        (limit, limit_old, limit_new, f'AddressRegionManager constructor: node area limit {mov(limit_old, 0x3fe00000)} → {mov(limit_new, 0x3e00000)}'),
        (occupied, occupied_old, occupied_new,
         f'AddressRegionManager constructor: length of the node area registered as an occupied region {mov(occupied_old, 0x3fe00000)} → {mov(occupied_new, 0x3e00000)}')]}
def align(size, fingerprints, old_32m, old_1g, old_4m):   # the three large classes' constants, at the same offsets in every implementation
    return {'name': alignment, 'size': size, 'fingerprints': fingerprints, 'cuts': [
        (0x64, old_32m, '0b04a052', f'AllocateAddressRegion: alignment of regions from 32 MB to 1 GB {mov(old_32m, 0x2000000)} → mov w11,#0x200000 (2 MB, as SDK 20)'),
        (0x6c, old_1g, '0a04a052', f'AllocateAddressRegion: alignment of regions of 1 GB and more {mov(old_1g, 0x40000000)} → mov w10,#0x200000 (2 MB, as SDK 20)'),
        (0x70, old_4m, '0804a052', f'AllocateAddressRegion: alignment of regions from 4 MB to 32 MB {mov(old_4m, 0x400000)} → mov w8,#0x200000 (2 MB, as SDK 20)')]}
MATCH_CUTS = {'sdk': [
    # SDK 7.3.2 (DRAGON QUEST XI S), 8.2.1 (Super Smash Bros. Ultimate), 8.3.1 (Paper Mario: The Origami King)
    node(276, ['5c81bb821d119d00ffcda6c3a4f1b827fc08c084fa53995e3a6262b5bf37cded'], 0x10, 'e9230b32', '097ca052', 0xb0, 'e9230b32', '097ca052'),
    # SDK 10.4.1 (Advance Wars 1+2), 10.4.8 (Pikmin 4)
    node(272, ['6fe2b1b95eced45e081611b786a8ff80b656bdcad6bd58082ee3264f5163ec44'], 0x10, 'e8230b32', '087ca052', 0xac, 'e9230b32', '097ca052'),
    # SDK 11.4.3 (Pokémon Brilliant Diamond), 12.3.2 (OCTOPATH TRAVELER II), 12.3.9 (Xenoblade Chronicles 3), 13.3.5 (Mario + Rabbids Sparks of Hope), 14.3.0 (Batman: Arkham Knight)
    node(272, ['3ab9bc1471da6658c861c809d8aa43b3be34fd78a65f7136cb346e442a52968b', '14660c33f9b064a429405d34d1ab0d1b8fd3867d5b335e2831afb59c2a399746'],
         0x10, '08fca752', '087ca052', 0xac, '09fca752', '097ca052'),
    # SDK 15.3.2 (Paper Mario: The Thousand-Year Door), 16.2.0 (Sea of Stars), 16.2.4 (Brain Memory Training Academy), 17.5.3 (Ace Attorney Investigations Collection),
    # 18.3.2 (Absolum), 18.3.5 (Super Mario Galaxy)
    node(240, ['cd7fb0efd387e5676d349376acf368b706116e089c30485f2d33970d9fb23145', 'd16909674e7f982f2fe1c765f7cd366341c2aaa9eb56a28c52a48d3c1057a126',
               '3549234b3e27a013791bbd834b5ff7dd501a5826c4216c80961bd996eef556ea'], 0x10, '08fca752', '087ca052', 0xa4, '09fca752', '097ca052'),
    # SDK 19.3.11 (Pokémon Legends: Z-A), 19.3.13 (Octopath Traveler 0), 20.5.19 (BALL x PIT), 20.5.22 (Tears of the Kingdom)
    node(244, ['b02f0514e2cc25a65261ec23cefa9a1fe7b244b3db741f183deb334935541eee'], 0x10, '08fca752', '087ca052', 0x7c, '09fca752', '097ca052'),
    # SDK 22.2.0 (EA SPORTS FC 27, Well Dweller): the SDK 19 / 20 constructor, instruction for instruction up to its cold path, which gives each failing result its own abort (32 bytes more)
    node(276, ['8960fe237061c387226a3775daf750a646dd0ee12eccf3d1340248683a6a7d78'], 0x10, '08fca752', '087ca052', 0x7c, '09fca752', '097ca052'),
    # SDK 22.2.0 built for the Switch 2 (Fire Emblem Fortune's Weave): the constructor at sdk+0x16ba30 with return address signing and its own frame. The host's StandardAllocator cap counts on the
    # 62 MB (alias_room): with 1 GB the capped region did not fit the alias region
    node(328, ['5f2f7a70fa5c25aeb7020f75dc785fd6ffe6c2f5c72d8fcb30e0ea9a0bb4bbc5'], 0x18, '08fca752', '087ca052', 0xa4, '09fca752', '097ca052'),
    # SDK 7.3.2 (DRAGON QUEST XI S), 8.2.1 (Super Smash Bros. Ultimate), 8.3.1 (Paper Mario: The Origami King), 10.4.1 (Advance Wars 1+2), 10.4.8 (Pikmin 4)
    align(352, ['006abd214d94c93386735550cee4fe7708bc1f0d1812663123cfa3b9611f1533', '42167e4d3c710e11937e45a9e420fdd0ed9a410386c17da4d1d896b847d3b0d6'], 'eb030732', 'ea030232', 'e8030a32'),
    # SDK 11.4.0 (Pokémon Legends: Arceus, Kirby and the Forgotten Land): the next rule's size classes with 8 more bytes of body; the 4 GB StandardAllocator region of Arceus's nninitStartup
    # (the host's cap) fits after the 62 MB node area only with it
    align(360, ['b8c0853dcad6b42ca2d504945c9944dd913a991297499e700a97a85bcf5a77c2'], '0b40a052', '0a00a852', '0808a052'),
    # SDK 12.3.2 (OCTOPATH TRAVELER II), 12.3.9 (Xenoblade Chronicles 3), 13.3.5 (Mario + Rabbids Sparks of Hope), 14.3.0 (Batman: Arkham Knight), 15.3.2 (Paper Mario: The Thousand-Year Door)
    align(352, ['4bec18d9a1f8c73408e95b54d971b2ae87ccb5451b185e3378c05801ad03fa39', '57ad718ebd4e8ebf49a3199c62e1ba2002867df5d124cfee6b4d30aeffee0b8d'], '0b40a052', '0a00a852', '0808a052'),
    # SDK 16.2.0 (Sea of Stars), 18.3.2 (Absolum), 18.3.5 (Super Mario Galaxy)
    align(388, ['bb9debe722dc0807c947365b7acfb1aa84f82af125e981f31b1d02b44d8032c8', '58845d068f4e828c39e03f66fbe03487387404183b0bcc14068eb45b571d2c02'], '0b40a052', '0a00a852', '0808a052'),
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'node area 1 GB → 62 MB and regions of 4 MB and more aligned to 2 MB, as SDK 20: the SDK-wide rules', 'now': 'sdk', 'goal': 'sdk', 'purpose': 'address space',
     'checked': '20 registered builds rewritten byte-identical with the rules alone, 2026-10-29 and 2026-10-30; the 2 MB alignment on every sdk build on the drive, 2026-10-30',
     'why': 'one rule per reviewed implementation; every sdk build without CUTS gets them'},
    {'what': 'rows the rules would add the alignment cuts to (2d369d99, 12f16103, 311deab1)', 'now': 'build', 'goal': 'sdk', 'purpose': 'address space',
     'checked': "the rules resolved on each build's sdk, 2026-10-29 and 2026-10-30",
     'why': 'the rules give these builds the same node area cut and the three alignment cuts more; each row keeps the alignment out',
     'blocked': 'the games that reach them (Pokémon Brilliant Diamond, Monster Hunter Rise, 13 Sentinels: Aegis Rim; Brain Memory Training Academy; Ace Attorney Investigations Collection) have no Mac build: run one per build with the alignment cuts'},
    {'what': 'rows of builds not on the game drive (56ec2474, 9c73fb73, e01950c5)', 'now': 'build', 'goal': 'sdk', 'purpose': 'address space',
     'why': 'likely what the rules give (each names one of their implementations)',
     'blocked': 'no copy on the drive to compare'},
    {'what': 'VammManager node area and alignment, SDK 4.4.0 (9de92bad)', 'now': 'build', 'goal': 'sdk', 'purpose': 'address space',
     'why': 'an older implementation no rule covers (its large regions aligned to 32 MB, not the rules\' 2 MB); a rule once a second game ships it',
     'blocked': 'one game ships it'},
]
