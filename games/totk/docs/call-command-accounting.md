# CallCommands command-memory accounting

TOTK 1.0.0 can crash during a tower flight toward the Zora region when an
`AreaMergeTarRenderer` allocation returns null. The failure is `main+0x855560`,
fault address `0x537`, following a request for `0x8d8` bytes aligned to 8.

[PR #2120](https://github.com/ly05010419/MacNativeGames/pull/2120) proposed a
32 KiB to 64 KiB heap increase. The initial heap identification, walk and size
recommendation were
[lili2029's investigation](https://github.com/ly05010419/MacNativeGames/issues/1742#issuecomment-6730940890).
The follow-up comparison requested in that PR instead identified excess
command-memory accounting upstream of the allocation.

## Matched comparison

The local comparison used TOTK 1.0.0, the same pre-patch save at Mount Lanayru
Skyview Tower, the same flight, 30 FPS, and matching infinite-stamina code
patches. Other cheats were disabled in the comparison sessions. The reference
was Ryujinx 1.3.3-e2143d4 running the original guest SDK. It completed the flight.
The native baseline reproduced the null allocation with its original 32 KiB heap.

All 31 observed allocation-entry caller/alignment pairs matched in order. Only
requests 17 and 18 differed in size:

| Value (bytes) | Native baseline | Ryujinx | Native accounting experiment |
|---|---:|---:|---:|
| Buffer subtotal | 2048 | 2048 | 2048 |
| Command subtotal | 2104 | 1720 | 1720 |
| Raw total | 4152 | 3768 | 3768 |
| Reservation, rounded to 4096 | 8192 | 4096 | 4096 |
| Associated metadata | 856 | 728 | 728 |

The command subtotal differs by 384 bytes. Crossing the page boundary adds
4096 bytes of reservation and 128 bytes of associated metadata: 4224 extra
bytes in a 32768-byte heap. The native failure snapshot contained free payloads
of 2136 and 696 bytes; neither could satisfy the later 2264-byte request.

The reservation caller is `main+0x856368`; its metadata allocation is at
`main+0x8563d4`. The final failing allocation's caller is `main+0x855550`.
The focused scalar probes checked stack frames, saved-register aliases and
the raw/rounded-size equations against the exact game image. Native command
subtotals were derived from raw total minus the saved buffer subtotal;
Ryujinx's probe also read the actual graphics-context command subtotal.

## Cause and correction

`inline_recording` copies host records through `cmd_alloc`, which initially
charges their host byte sizes to `hw_used`. Direct recording APIs normally
replace this charge with the hardware command cost through `hw_fix`.
`CallCommands` previously retained the copied host-byte charge.

In the verified NVN 55 SDK, Call uses control references to existing GPU
segments. Copy uses a distinct mode that can copy GPU command words. Review
of the full control-tag dispatch, including deferred bindings, found Call's
control-copy helpers advance the control cursor without adding fresh GPU
command bytes. `GetCommandMemoryUsed` reads the command cursor separately.

For the verified profiles, `inline_call_recording` therefore saves and restores
both `hw_used` and `last_rec` around the existing host copy. Restoring `last_rec`
preserves the parent's charge cursor for a subsequent `hw_fix`. Host bytes,
record order, flattening, replay and handle lifetime still use the same path.
Copy retains its existing accounting; this change does not establish that
Copy's current hardware-cost estimates are exact.

The SDK NSO SHA-256 is
`91573d4444d0dcd07080db78fece2b098b76080d77cdb2e1d301a2a41d7c4eea`,
shared by TOTK 1.0.0 and 1.2.1. Version 1.4.3 uses a different SDK.
The capability is selected in the runtime bridge, compiled per game version:
Mac 1.4.3 shares the common graphics object with older versions, so testing its
version macro in that graphics object would incorrectly enable the change.
The bridge returns false for 1.4.3, BOTW and Switch 2 profiles. An absent weak
hook also retains legacy accounting. This is not a universal claim about all
NVN SDKs or other backends.

## Validation and limits

The private accounting experiment, with the original 32768-byte heap, produced
all three Ryujinx subtotals above. All 31 observed allocation requests then
matched Ryujinx, including successful completion of the formerly failing
`0x8d8` request with the matching call stack. The user repeated the flight and
reported passing the old crash point. The public wrapper has the same
accounting operations; its capability gate replaces the private startup flag.

`tests/test_totk_call_command_accounting.py` compiles the real recorder,
allocator, flattening, Call/Copy and typed command functions with synthetic
owned buffers. UBSan checks both bulk and per-record copying, missing/false/
true capability hooks, nonzero parent charges, multiple and nested calls,
Copy, subsequent recording, empty lists, growth and source retirement. It
compares host-byte hashes, replayed draw arguments and control counters, and
checks a synthetic 384-byte difference crossing the same page boundary.
Separate builds check the runtime capability for each supported profile.

Gameplay validation covers one successful native flight in 1.0.0. Version
1.2.1 has matching SDK evidence and CPU coverage, but no gameplay retest.
iOS has no device playtest. The reference allocation trace contains explicit
debugger/history gaps; the paired scalar probes provide separate evidence.
The successful native trace contains 31 allocation entries and 31 success
events, with zero dropped counts in observed records, but its window-close
path emitted no terminal summary. These observations are not a complete
allocator-coverage or long-duration stability claim.

No game binaries, keys, saves, shaders, heap dumps or private traces are
included in this change.
