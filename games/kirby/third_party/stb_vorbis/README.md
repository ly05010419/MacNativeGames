Copy of nxnative's `dm/third_party/stb_vorbis` (seedds/nxnative beea17c, 2026-10-29), unchanged. Upstream: https://github.com/nothings/stb `stb_vorbis.c` at commit 1ee679c (version 1.22). License in LICENSE (MIT or public domain, your choice; the same text ends the source file).

Use: `runtime/movie_host.c` decodes the Vorbis sound track of Kirby's WebM movies with it (the opening movie, `mov/EventMovie`), included with `STB_VORBIS_NO_STDIO`, `STB_VORBIS_NO_PULLDATA_API` and `STB_VORBIS_NO_INTEGER_CONVERSION`; each raw packet goes in as an Ogg page of its own.

nxnative's local change is kept (upstream still has the bug, nothings/stb issues #879 and #1594): a type-2 residue interleaves every channel of its submap, so `decode_residue` reads up to n × the submap's channel count values, not n × 2, and `start_decoder`'s temporary-memory estimate counts every channel of the stream for type-2 residues. Without it, 5.1 tracks encoded by libvorbis come out distorted.
