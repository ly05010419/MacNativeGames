#!/usr/bin/env python3
"""Static resource-class matrix for one frame's compute dispatch chain (plan P0, Stage 1).

The title screen of Ocarina of Time submits fourteen compute dispatches back to back (~23 ms of GPU
time, none of them pipelined by the port's shared compute encoder). Before any barrier may be
dropped, the chain's consecutive pairs have to be classified. This script does the part that can be
done without the game running:

  * per dispatch, the resources its reflection says it may read or write (storage buffers with
    NVN's rw flag, textures, images, uniform/driver blocks), its threadgroup size, and whether its
    writes are *generic* (the game writes through raw addresses the translator could not map to a
    binding: the raw-write notes or a raw_write_proof capsule);
  * per consecutive pair, whether the two known resource sets intersect on a (kind, reg) class, or
    are class-disjoint, or cannot be classified because one side writes through generic addresses.

A class-disjoint answer is necessary but not sufficient for overlapping the two dispatches: the
runtime still has to prove the bound byte ranges disjoint (nvn_dispatch_chain.h's alias input). A
generic-write side has no complete set at all, so the pair is UNKNOWN and keeps its barrier. The
counts this prints are therefore an *upper bound* on what static classification alone could justify,
and the basis for the Stage 2 runtime range proof.

  dispatch_chain_matrix.py --seed <work>/oot/build/shader-seed/oot --gpu-work <log with [gpu-work] lines>
      [--frame N] [--json out.json]
"""
import argparse
import json
import pathlib
import re
import sys

WORK_RE = re.compile(r"\[gpu-work\] F(\d+) .*?dispatch=(\d+) key=(\S+) grid=(\d+)x(\d+)x(\d+) tg=(\d+)x(\d+)x(\d+)")

# A resource class is (kind, reg-or-index). Registers are the game's NVN binding slots; the same
# slot in two dispatches may carry different memory, so an intersection is only "may alias".
READ_KINDS = {"tex", "samp_tex", "cbuf", "drvcb", "support", "cbuf_ptr"}
WRITE_KINDS = {"img"}
RAW_WRITE_NOTE = "Global memory operation"


def normalise(key):
    """The dispatch log prints the 80-character variant key; the seed files are named by it."""
    return key.split("-")[0]


def load_reflection(seed, prefix):
    for path in seed.glob(prefix + "*.json"):
        if path.name.endswith("_b.json") or "_b." in path.name:
            return json.loads(path.read_text())
    for path in seed.glob(prefix + "*.json"):
        return json.loads(path.read_text())
    return None


def classify(reflection):
    """Known read/write classes, generic-write flag and image count of one dispatch."""
    reads, writes, generic = set(), set(), False
    images = 0
    for binding in reflection.get("bindings", []):
        kind = binding.get("kind")
        reg = binding.get("reg")
        if reg is None:
            reg = binding.get("index")
        cls = (kind, reg)
        if kind in READ_KINDS:
            reads.add(cls)
        if kind in WRITE_KINDS:
            writes.add(cls)
            images += 1
        if kind == "sbuf":
            # NVN's rw flag is the storage buffer's own declaration; a read-only buffer can still be
            # written through a raw address the translator noted, so both sets stay conservative.
            if binding.get("rw"):
                writes.add(cls)
            else:
                reads.add(cls)
    notes = reflection.get("notes") or []
    if any(RAW_WRITE_NOTE in note for note in notes):
        generic = True
    if reflection.get("raw_write_proof"):
        generic = True
    return dict(reads=reads, writes=writes, generic=generic, images=images)


def pair_verdict(a, b):
    """Classify (previous, current): disjoint, may-alias, or unknown (generic writes)."""
    if a["generic"] or b["generic"]:
        return "unknown"
    previous_writes, current_reads = a["writes"], b["reads"]
    current_writes, previous_reads = b["writes"], a["reads"]
    if previous_writes & current_reads or current_writes & previous_reads or previous_writes & current_writes:
        return "may-alias"
    return "class-disjoint"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=pathlib.Path, required=True, help="translated shader seed folder (shader manifest + <key>_b.json)")
    parser.add_argument("--gpu-work", type=pathlib.Path, required=True, help="game log with [gpu-work] lines (flag pass_gpu capture)")
    parser.add_argument("--frame", type=int, default=None, help="frame to analyse (default: the first frame with dispatches)")
    parser.add_argument("--json", type=pathlib.Path, default=None, help="write the full matrix here")
    args = parser.parse_args()

    frames = {}
    for line in args.gpu_work.read_text(errors="replace").splitlines():
        match = WORK_RE.search(line)
        if not match:
            continue
        frame = int(match.group(1))
        entry = dict(key=match.group(3), grid=tuple(int(match.group(i)) for i in (4, 5, 6)),
                     tg=tuple(int(match.group(i)) for i in (7, 8, 9)))
        frames.setdefault(frame, []).append(entry)
    if not frames:
        sys.exit(f"{args.gpu_work}: no [gpu-work] lines; run with TOTK_PASS_GPU_FILE and 'flag pass_gpu N' first")
    frame = args.frame if args.frame is not None else min(frames)
    if frame not in frames:
        sys.exit(f"{args.gpu_work}: no dispatches recorded for frame {frame}")

    chain = []
    for entry in frames[frame]:
        prefix = normalise(entry["key"])
        reflection = load_reflection(args.seed, prefix)
        if reflection is None:
            sys.exit(f"no reflection for {prefix} under {args.seed}")
        chain.append(dict(dispatch=len(chain), prefix=prefix, grid=entry["grid"], tg=entry["tg"],
                          **classify(reflection)))

    rows, counts = [], {"class-disjoint": 0, "may-alias": 0, "unknown": 0}
    for previous, current in zip(chain, chain[1:]):
        verdict = pair_verdict(previous, current)
        counts[verdict] += 1
        rows.append(dict(previous=previous["prefix"], current=current["prefix"], verdict=verdict))

    print(f"frame {frame}: {len(chain)} compute dispatches")
    print(f"{'#':>2} {'key':10s} {'grid':>12s} {'tg':>9s} {'read classes':>3s} {'write classes':>3s}  generic-writes  images")
    for item in chain:
        print(f"{item['dispatch']:2d} {item['prefix'][:8]:10s} {str(item['grid']):>12s} {str(item['tg']):>9s} "
              f"{len(item['reads']):13d} {len(item['writes']):13d}  {str(item['generic']):14s} {item['images']:6d}")
    print("\nconsecutive pairs:")
    for index, row in enumerate(rows):
        print(f"  {index:2d}->{index+1:2d} {row['previous'][:8]} -> {row['current'][:8]}: {row['verdict']}")
    print(f"\nstatic upper bound: {counts['class-disjoint']} of {len(rows)} pairs are class-disjoint "
          f"({counts['may-alias']} may alias, {counts['unknown']} unknown because a side writes generic addresses)")
    print("Note: class-disjoint is necessary, not sufficient; the runtime byte-range proof decides "
          "(nvn_dispatch_chain.h alias input). Generic-write pairs keep their barrier by policy.")

    if args.json:
        args.json.write_text(json.dumps(dict(frame=frame, chain=[dict(dispatch=c["dispatch"], key=c["prefix"],
                                  grid=c["grid"], tg=c["tg"], generic=c["generic"], images=c["images"],
                                  reads=sorted(map(str, c["reads"])), writes=sorted(map(str, c["writes"]))) for c in chain],
                                  pairs=rows, counts=counts), indent=2) + "\n")
        print(f"matrix written to {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
