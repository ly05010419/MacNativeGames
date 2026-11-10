#!/usr/bin/env python3
"""A draw that writes depth or stencil and samples its pass's depth attachment through a texture slot must read the depth
stored when the draw starts (the console's behaviour). On Apple's GPUs the attachment sampled in the pass that writes it
reads blocks of undefined content (2026-11-10, Ocarina of Time: the seed bed's blended rim, white blocks around the bed).
The draw path makes a copy and the slot resolvers hand the copy out for that draw."""
import pathlib, re
src = (pathlib.Path(__file__).resolve().parents[1] / "third_party/d2r_gfx/gfx/nvn/nvn_mtl.m").read_text()
call = src.find("(S.ds.dw || S.ds.st) && has_ser(att_rs, att_n, R.rt.depth->serial)) { if (!dsnap_prepare(R.rt.depth)) dsnap_clear(); }")
assert call > 0, "the draw path does not copy the depth attachment for a draw that writes and samples it"
assert src.find("else dsnap_clear();", call) - call < 200, "a draw that does not need the copy must drop the previous draw's copy"
prep = src[src.index("static int dsnap_prepare"):src.index("static int att_store_on")]
assert prep.index("rp_flush();") < prep.index("enc_b_copy_texture("), "the pass must end (its depth stored) before the copy"
assert "args_dirty_all();" in prep, "argument tables encoded with the attachment must be encoded again with the copy"
for fn in ("tex_fast(int tid)", "tex_fast_rid(int tid, void **out)"):
    body = src[src.index("static inline __attribute__((always_inline)) " + ("void *" if "rid" not in fn else "uint64_t ") + fn):][:400]
    assert "dsnap_hit(tid)" in body, fn + " does not resolve the attachment to the copy"
print("PASSED")
