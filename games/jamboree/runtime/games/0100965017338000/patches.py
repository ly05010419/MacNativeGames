CUTS = {
  # Super Mario Party Jamboree 2.3.0 (v458752) main: nninitStartup (main+0x1a6d58) initializes the Bezel engine's malloc StandardAllocator with virtual address
  # memory over 56 GB of address space (main+0x982840). The Switch's 64 GB alias region holds it, while this profile uses 17 GB; the allocator commits pages as
  # needed and the 3.285 GB application pool bounds what it maps. The engine reserves another 2 GB region first; the Mac profile therefore gives the alias region
  # 16 GB, enough for both and for the SDK's address bookkeeping under either host address layout.
  'd70ce1726c979e45ed4200e9c2b23b2a': [
    (0x982840, 'c801c0d2', '2800c0d2', 'StandardAllocator::Initialize(nullptr, size): address space mov x8,#0xe00000000 -> mov x8,#0x100000000 (56 GB -> 4 GB)'),
  ],
}
