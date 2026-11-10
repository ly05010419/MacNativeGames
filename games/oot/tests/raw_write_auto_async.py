#!/usr/bin/env python3
"""The write-range analysis takes 0.2-0.3 s per new input identity (2026-11-10: 256-308 ms in the game log, one long
frame each). It must never run on the thread that replays the frame: the request is handed to a serial queue, the
dispatch falls back until the result is collected on a later dispatch."""
import pathlib, re, sys
root = pathlib.Path(__file__).resolve().parents[1] / "third_party/d2r_gfx/gfx/nvn"
auto = (root / "nvn_raw_write_auto.inc").read_text()
domain = (root / "nvn_raw_write_domain.inc").read_text()
body = auto[auto.index("static NSDictionary *raw_write_auto_prove"):]
call = re.search(r"\bprove\(", body)
assert call, "the analyzer call is gone"
queued = body.find("dispatch_async(raw_write_auto_queue()")
assert 0 <= queued < call.start(), "the analyzer is called outside the background queue"
assert body.rstrip().endswith("return nil;\n}") or re.search(r"\}\);\s*return nil;\s*\}\s*$", body), \
    "raw_write_auto_prove must return without a result (the dispatch falls back until the result is collected)"
collect = domain.find("raw_write_auto_collect();")
first_use = domain.find("sh->raw_write_auto_domains")
assert 0 <= collect < first_use, "finished analyses must be collected before the proved domains are matched"
gate = body.find("sh->raw_write_auto_pending)return nil;")
attempted = body.find("[sh->raw_write_attempted addObject:identity]")
assert 0 <= gate < attempted, "a shader with an analysis in flight must not queue another, nor mark the identity attempted"
assert "sh->raw_write_auto_pending=1;" in body and "sh->raw_write_auto_pending=0;" in auto, "the in-flight mark is set at the hand-over and cleared at collection"
print("PASSED")
