# BOTW 1.9 on a constrained iOS address map

An iPad mini 6 (A15, 4 GB), iPadOS 27.2.1, signed by a free Personal Team with
`increased-memory-limit` in both the profile and signature, reports a task maximum
address of `0x7180000000`. Extended virtual addressing is not granted.

The ordinary iOS reservation needs 4.75 GiB for the stack/heap area and at least
4 GiB of alias space. It fails before `nnosInitialize` returns. A diagnostic run
with `TOTK_ALIAS_GIB=1` passed startup and entered gameplay in BOTW 1.9.0:

```text
guest layout: alias 0x7130000000+0x40000000
SetHeapSize 0xc3000000 (image=0x315c000 alias=0x1000 pool=0xcd500000)
ENTER nnMain
```

That layout still reserves a 4 GiB heap and leaves only 256 MiB at the top of the
six-GiB window for additional host mappings. A later run reached a 2859 MiB
footprint and aborted allocating an `NvShader`. Physical pressure and the host's
address headroom both need consideration; startup success alone is insufficient.

The small-map policy chooses a 1 GiB alias default for BOTW 1.9 when the measured
Horizon window is no larger than six GiB. Larger/unknown maps keep the ordinary
16 GiB request and 4 GiB fallback floor. An explicit `TOTK_ALIAS_GIB` still wins.
Other games keep their alias policy: TOTK must not inherit this 1 GiB assumption.

On iOS, BOTW's heap reservation now rounds the reported application pool up to a
2 MiB granule instead of imposing an additional 4 GiB floor. The retail pool
`0xcd500000` needs a 3286 MiB reservation, which still covers the observed
3120 MiB heap. Stack offsets and the reported application pool remain the same.
Together with the 1 GiB alias, this reserves 5078 MiB and leaves 1066 MiB of the
window available, recovering 810 MiB of virtual address space for the host.
This does not reduce the game's physical memory requirements or promise a frame rate.

The compiled C regression checks the measured boundary, unknown/larger maps,
heap granularity, pool overrides, and the remaining host headroom:

```sh
python -m unittest discover -s tests -p test_ios_botw_memory.py -v
```
