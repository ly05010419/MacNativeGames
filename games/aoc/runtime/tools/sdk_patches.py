# The rewriter's patches of Nintendo SDK module builds (sdk), which several games can ship: dm/tools/patch_nso.py applies those of a module's NSO build ID, whatever the game.
# A patch of a game's own module (main) goes in its profile's patches.py (dm/games/<id>/patches.py). CUTS as described in patch_nso.py ("Registered patches")
CUTS = {
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
  '311deab1f062b504f3d646a49ddcd4e8': [
    (0x139468, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x1394fc, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Brain Memory Training Academy 1.0.0 (v0) sdk (SDK 16.2.4, the same build as Unicorn Overlord 1.05's, which has no virtual address memory and never allocates an address region):
  # the same node area cut. nninitStartup (main+0x73ad60) calls StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory, which aborted at its CentralHeap::Init.
  # The constructor is at sdk+0x134768 (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x13480c)
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
  # AI: THE SOMNIUM FILES - nirvanA Initiative 1.0.2 (v131072) sdk (SDK 13.3.5, Unity 2020.3.30f1): the same node area cut. nninitStartup (main+0x493300) calls
  # StandardAllocator::Initialize(nullptr, pool − used) with virtual address memory, as Patrick's Parabox. The constructor is at sdk+0x37c130 (limit = alias start + 0x3fe00000 at +0x28,
  # a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x37c1dc)
  '3644a6760469451cabc99f00c540acc2': [
    (0x37c140, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x37c1dc, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Pokémon Legends: Z-A 2.0.2 (v393216) sdk (SDK 19.3.11): the same node area cut. The constructor is at sdk+0x1423ec (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte
  # nodes, the area then registered as occupied at +0x142468). With main's allocator region below, the guest block fits the 5 GB proven on a phone
  'df7a3960c74c3a4803f826c587153252': [
    (0x1423fc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x142468, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Hollow Knight: Silksong 1.0.30000 (v655360) sdk (SDK 20.5.8, Unity 6000.0.50f1): the same node area cut. Unity 6 takes all its memory itself through nn::os::AllocateAddressRegion and
  # AllocateMemoryPages (no heap, no StandardAllocator in nninitStartup), and without the cut its regions start after the node area: the alias region's high-water mark was +1.00 GB at
  # nninitStartup and +3.00 GB in the first room of play, with 1.51 GB mapped. The constructor is at sdk+0x145bec (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte
  # nodes, the area then registered as occupied at +0x145c68)
  'e01950c58e17b080c90494784ea1dd29': [
    (0x145bfc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x145c68, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # SDK 20.5.22 sdk (The Legend of Zelda: Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0, the same build): the same node area cut. Tears of the Kingdom's resource heaps live in
  # address regions after the node area. At the first descent from the Great Sky Island the alias region's high-water mark was +2.25 GB of its 2.375 GB with 1.00 GB mapped, and the game then
  # took a memory shortage: its frame check (main+0x1fd32ec) logs MSF / R / Rf and plays the memory-shortage Blood Moon, which puts Link back at the start (GitHub issue #70; 7 of 8 Mac runs from
  # the issue's save). With an 8 GB alias region the mark went on to +2.50 GB, with this cut it stays at +1.75 GB, and neither showed the shortage. The constructor is at sdk+0x145cfc
  # (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x145d78); VammManager::AllocateAddressRegion (sdk+0x145df0) aligns
  # regions to 64 KB or 2 MB only, so no alignment cut is needed
  '574284b40c06cf1821a44857939083be': [
    (0x145d0c, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x145d78, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Super Mario Galaxy 1.3.1 / Super Mario Galaxy 2 1.4.0 sdk (SDK 18.3.5, the same build in both): the same node area cut. Galaxy 2's movie player initializes its StandardAllocator with
  # virtual address memory (main+0x62c280: Initialize(nullptr, 384 MB)), which aborted at its CentralHeap::Init: a 256 MB alias region holds no region after a 1 GB node area.
  # The constructor is at sdk+0x13be3c (limit = alias start + 0x3fe00000 at +0x28, a unit heap of 0x30-byte nodes, the area then registered as occupied at +0x13bee0)
  '6075151627016eef437c1312f2e3ed56': [
    (0x13be4c, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x13bee0, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # Absolum 1.1.1 (v393216) sdk (SDK 18.3.2; the AddressRegionManager at the offsets of Galaxy's SDK 18.3.5 build): the same node area cut, and regions of 1 GB and more aligned to 32 MB
  # instead of 1 GB. VammManager::AllocateAddressRegion (sdk+0x13bfd0) aligns a region by its size: 64 KB under 2 MB, 2 MB under 4 MB, 4 MB under 32 MB, 32 MB under 1 GB, else 1 GB
  # (+0x13c03c), and places it first-fit after the regions already there. Absolum's two large regions (main's malloc, and the .NET GC's region range from the game_env) are both over 1 GB, with a 4 KB
  # region between them, so the alignment lost up to 1 GB before each one (measured with a 64 GB alias region: 0.6 GB before malloc's at that run's alias base, 1 GB before the GC's), which
  # the 64 GB alias region of a Switch never misses and an iPhone's 4.375 GB cannot spare. 32 MB alignment is what the SDK gives regions from 32 MB up; nothing in the SDK or the game reads the region's alignment
  'f118ae1c2e7728471aaf1a0d58484968': [
    (0x13be4c, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x13bee0, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x13c03c, '0a00a852', '0a40a052', 'AllocateAddressRegion: alignment of regions of 1 GB and more mov w10,#0x40000000 → mov w10,#0x2000000 (32 MB, as from 32 MB up)'),
  ],
  # SDK 15.3.2 sdk 8618895246b4 (Paper Mario: The Thousand-Year Door 1.0.1, Celeste 1.4.1.1; Celeste has no virtual address memory and never makes an address region): Absolum's cuts on this
  # build. The AddressRegionManager constructor is at sdk+0x12e0e0 (node area limit at +0x12e0f0, registered as occupied at +0x12e184) and VammManager::AllocateAddressRegion at sdk+0x12e1d0
  # aligns regions of 1 GB and more to 1 GB (+0x12e2e0). Measured with a 64 GB alias region: Paper Mario's malloc region was placed at the next 1 GB boundary after the 1 GB node area, 0.69 GB further
  '8618895246b420329b77746b960a66ab': [
    (0x12e0f0, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x12e184, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x12e2e0, '0a00a852', '0a40a052', 'AllocateAddressRegion: alignment of regions of 1 GB and more mov w10,#0x40000000 → mov w10,#0x2000000 (32 MB, as from 32 MB up)'),
  ],
  # FINAL FANTASY RESONANCE 1.0.1 (v65536) sdk (SDK 20.5.17, the build STEINS;GATE RE:BOOT runs, which has no virtual address memory): the AddressRegionManager node area cut, as on the other
  # Unreal games' SDKs. The constructor is at sdk+0x145cac: +0x145cbc sets the node area limit, +0x145d28 registers it as an occupied region
  '0023a1b06130cd795f7bc4613772af0a': [
    (0x145cbc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x145d28, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # DRAGON QUEST VII Reimagined 1.2.0 (v131072) sdk (SDK 19.3.1): the same AddressRegionManager constructor as Octopath Traveler 0's SDK 19.3.13 build below, at the same offsets
  '145dde94fb574acc9e09780a11995bd3': [
    (0x1423fc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x142468, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
  # DRAGON QUEST XI S 1.0.4 (v262144) sdk (SDK 7.3.2, the same build as Super Mario Odyssey 1.4.1's, which has no virtual address memory and never allocates an address region): the node
  # area cut, and regions of 1 GB and more aligned to 32 MB, as Absolum's. Unreal Engine 4.18's nninitStartup takes one region for the whole engine, which a NumberLineAllocator
  # (main+0x177c4d0) hands out (16 GB by default, -SwitchVirtualAddressSpaceSize= in MB caps it: the profile's game_args); the 1 GB alignment would have put it 1 GB into the alias region.
  # The constructor is at sdk+0x32c450 (the limit at +0x28 and the occupied length both from orr w9,wzr,#0x3fe00000); VammManager::AllocateAddressRegion's placement at sdk+0x32c600 aligns
  # a region by its size as SDK 18 does: 64 KB under 2 MB, 2 MB under 4 MB, 4 MB under 32 MB, 32 MB under 1 GB, else 1 GB (+0x32c66c)
  'fcf93a41b08c5193a1410d262602dc62': [
    (0x32c460, 'e9230b32', '097ca052', 'AddressRegionManager constructor: node area limit orr w9,wzr,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x32c500, 'e9230b32', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region orr w9,wzr,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x32c66c, 'ea030232', '0a40a052', 'AllocateAddressRegion: alignment of regions of 1 GB and more orr w10,wzr,#0x40000000 → mov w10,#0x2000000 (32 MB, as from 32 MB up)'),
  ],
  # SDK 8.2.1 sdk 5fd4f531c119 (Super Smash Bros. Ultimate 13.0.5; Catherine: Full Body 1.0.1a, which has no virtual address memory and never makes an address region): the node area cut,
  # and regions of 1 GB and more aligned to 32 MB, as DRAGON QUEST XI S's SDK 7.3.2 build, whose AddressRegionManager this is at other offsets. Smash's engine reserves one region for all its
  # memory (cut to 4 GB in its patches.py); with the 1 GB node area and 1 GB alignment it started 1.95 GB into the alias region (measured with a 64 GB one). The constructor is at
  # sdk+0x336b40 (the limit at +0x336b50 and the occupied length at +0x336bf0, both orr w9,wzr,#0x3fe00000); VammManager::AllocateAddressRegion aligns a region by its size (+0x336d5c)
  '5fd4f531c1195e9e5510c9e73f22cd39': [
    (0x336b50, 'e9230b32', '097ca052', 'AddressRegionManager constructor: node area limit orr w9,wzr,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x336bf0, 'e9230b32', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region orr w9,wzr,#0x3fe00000 → mov w9,#0x3e00000'),
    (0x336d5c, 'ea030232', '0a40a052', 'AllocateAddressRegion: alignment of regions of 1 GB and more orr w10,wzr,#0x40000000 → mov w10,#0x2000000 (32 MB, as from 32 MB up)'),
  ],
  # Octopath Traveler 0 1.0.5 (v327680) sdk (SDK 19.3.13): the AddressRegionManager node area 0x3fe00000 → 0x3e00000 (62 MB), as Engage's SDK 12 (above). The same constructor (sdk+0x1423ec): +0x1423fc
  # sets the node area limit, +0x142468 registers it as an occupied region. Unreal's region (-SwitchVirtualAddressSpaceSize, the profile's game_args) then starts 62 MB into the alias region, and the guest
  # block stays small enough for an iPhone next to the Unreal heap (hle_uemalloc.c), which lives outside it
  '7bba185a35ee9b4d0af66a4df910a823': [
    (0x1423fc, '08fca752', '087ca052', 'AddressRegionManager constructor: node area limit mov w8,#0x3fe00000 → mov w8,#0x3e00000'),
    (0x142468, '09fca752', '097ca052', 'AddressRegionManager constructor: length of the node area registered as an occupied region mov w9,#0x3fe00000 → mov w9,#0x3e00000'),
  ],
}
