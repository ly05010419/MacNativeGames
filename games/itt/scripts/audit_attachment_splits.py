#!/usr/bin/env python3
"""Read-only classification of ITT passdump attachment-store boundaries.

Records evidence, not permission to merge passes. A texture's depth/stencil
sampling plane and all intervening writes still require runtime/GPU validation.
No diagnostic timing is converted to normal FPS or estimated savings.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

ROW = re.compile(r'\[pass\] F(\d+) ([PBdM]) (\d+) (.*)')
SPLIT = '绘制采样本通道画过的附件，先写回再画'


def fields(text):
    return dict(re.findall(r'(\w+)=(\S+)', text))


def audit(lines):
    passes, boundaries = {}, []
    for line in lines:
        match = ROW.search(line)
        if not match:
            continue
        frame, kind, serial, body = match.groups()
        key = (int(frame), int(serial))
        entry = passes.setdefault(key, dict(attachments={}, textures=[], draws=[]))
        if kind == 'P':
            entry['attachments'][body.split()[0]] = fields(body)
        elif kind == 'B' and ' tex reg' in body:
            entry['textures'].append(fields(body))
        elif kind == 'd':  # uppercase D is only the first-draw summary
            entry['draws'].append(fields(body))
        elif kind == 'M' and body == SPLIT:
            boundaries.append((key, entry['draws'][-1].copy() if entry['draws'] else {}))

    findings = []
    for (frame, serial), previous in boundaries:
        before = passes[(frame, serial)]
        after = passes.get((frame, serial + 1))
        depth = before['attachments'].get('d', {})
        next_depth = after['attachments'].get('d', {}) if after else {}
        same_depth = bool(depth.get('ser') and depth.get('ser') == next_depth.get('ser'))
        sampled = [t for t in after['textures'] if t.get('ser') == depth.get('ser')] if after and same_depth else []
        stencil_only = previous.get('fs') == '-' and previous.get('dw') == '0' and previous.get('st') == '1'
        findings.append(dict(frame=frame, pass_serial=serial,
                             preceding_no_fragment_depth_write_off_stencil_on=stencil_only,
                             same_depth_sampled_in_next_pass=bool(sampled),
                             sampled_metal_formats=sorted({t.get('fmt', '?') for t in sampled}),
                             preceding_draw=previous,
                             draws_in_ended_pass=len(before['draws']),
                             next_pass_recorded=after is not None))
    return dict(scope='Logged boundaries only; not a dependency proof or an FPS estimate',
                split_count=len(findings),
                splits_by_frame=dict(sorted(Counter(str(f['frame']) for f in findings).items())),
                stencil_then_same_depth_sample_count=sum(
                    f['preceding_no_fragment_depth_write_off_stencil_on'] and
                    f['same_depth_sampled_in_next_pass'] for f in findings),
                findings=findings)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    with args.log.open(errors='replace') as source:
        result = audit(source)
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(f"{result['split_count']} splits; "
          f"{result['stencil_then_same_depth_sample_count']} follow stencil-only-state draws and sample the same depth target")


if __name__ == '__main__':
    main()
