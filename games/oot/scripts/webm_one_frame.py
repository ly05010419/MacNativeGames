"""A one-keyframe WebM cut from the opening movie, without re-encoding and without ffmpeg.

The opening overlay (make_opening_overlay.py) needs the original movie's first AV1 keyframe in a WebM of its own. Until
2026-11-12 ffmpeg made it (`-frames:v 1 -c:v copy -an -map_metadata -1 -fflags +bitexact`) and ffprobe checked it, so
every player had to install Homebrew's ffmpeg for one remux. This module reads the original's EBML header, its one video
track and its first SimpleBlock, and writes the same elements in the same order and with the same size fields as that
ffmpeg command did (the game played that file): EBML header, Segment (8-byte size) with SeekHead + Void up to Info, Info
(1 ms ticks, muxer "Lavf", the frame's duration), Tracks (the track with UID 1, language "und", the original codec,
default duration, Video and CodecPrivate), Tags (DURATION), one Cluster holding the keyframe, and Cues pointing at it.
For the pinned original the bytes equal ffmpeg's (make_opening_overlay.MOVIE_ONE_FRAME_SHA). `inspect` reads a WebM
back on its own (tracks, blocks, duration) for the checks.

Element ids and layouts: the Matroska / WebM specification (matroska.org, RFC 9559).
"""
import struct

EBML_HEADER, SEGMENT, SEEK_HEAD, SEEK, SEEK_ID, SEEK_POSITION, VOID = 0x1A45DFA3, 0x18538067, 0x114D9B74, 0x4DBB, 0x53AB, 0x53AC, 0xEC
INFO, TIMECODE_SCALE, MUXING_APP, WRITING_APP, DURATION = 0x1549A966, 0x2AD7B1, 0x4D80, 0x5741, 0x4489
TRACKS, TRACK_ENTRY, TRACK_NUMBER, TRACK_UID, TRACK_TYPE, FLAG_LACING, LANGUAGE = 0x1654AE6B, 0xAE, 0xD7, 0x73C5, 0x83, 0x9C, 0x22B59C
CODEC_ID, CODEC_PRIVATE, DEFAULT_DURATION, VIDEO, PIXEL_WIDTH, PIXEL_HEIGHT = 0x86, 0x63A2, 0x23E383, 0xE0, 0xB0, 0xBA
TAGS, TAG, TARGETS, TAG_TRACK_UID, SIMPLE_TAG, TAG_NAME, TAG_STRING = 0x1254C367, 0x7373, 0x63C0, 0x63C5, 0x67C8, 0x45A3, 0x4487
CLUSTER, TIMECODE, SIMPLE_BLOCK = 0x1F43B675, 0xE7, 0xA3
CUES, CUE_POINT, CUE_TIME, CUE_TRACK_POSITIONS, CUE_TRACK, CUE_CLUSTER_POSITION, CUE_RELATIVE_POSITION = 0x1C53BB6B, 0xBB, 0xB3, 0xB7, 0xF7, 0xF1, 0xF0
MASTERS = {EBML_HEADER, SEGMENT, SEEK_HEAD, SEEK, INFO, TRACKS, TRACK_ENTRY, VIDEO, TAGS, TAG, TARGETS, SIMPLE_TAG, CLUSTER, CUES,
           CUE_POINT, CUE_TRACK_POSITIONS}
INFO_AT = 161   # where ffmpeg puts Info in the Segment: the SeekHead and a Void fill the space it reserves before it
HEAD_BYTES = 1 << 20   # the original's header and first block are in its first kilobytes


def _vint(data, at):
    """(value, length) of the EBML variable-size integer at data[at] (marker bit removed), and whether it is 'unknown'."""
    first = data[at]
    length = next((n for n in range(1, 9) if first & (0x80 >> (n - 1))), None)
    if length is None:
        raise ValueError('bad EBML size at %d' % at)
    value = first & (0xFF >> length)
    for byte in data[at + 1:at + length]:
        value = value << 8 | byte
    return value, length, value == (1 << (7 * length)) - 1


def _element_id(data, at):
    first = data[at]
    length = next((n for n in range(1, 5) if first & (0x80 >> (n - 1))), None)
    if length is None:
        raise ValueError('bad EBML id at %d' % at)
    return int.from_bytes(data[at:at + length], 'big'), length


def elements(data, start=0, end=None, clip=False):
    """The elements of data[start:end] as (id, element start, payload start, payload end); an unknown size runs to end.
    clip: data is only the first bytes of a file, an element running past them ends where they do (read in order and
    stop before the cut)."""
    end = len(data) if end is None else end
    at = start
    while at < end:
        eid, id_length = _element_id(data, at)
        size, size_length, unknown = _vint(data, at + id_length)
        payload = at + id_length + size_length
        stop = end if unknown else payload + size
        if stop > end:
            if not clip:
                raise ValueError('element %#x at %d runs past its parent' % (eid, at))
            stop = end
        yield eid, at, payload, stop
        at = stop


def _children(data, payload, stop):
    return {eid: (begin, inner, finish) for eid, begin, inner, finish in elements(data, payload, stop)}


def _size(value, width=None):
    width = width or next(n for n in range(1, 9) if value < (1 << (7 * n)) - 1)
    return ((1 << (7 * width)) | value).to_bytes(width, 'big')


def _el(eid, payload, width=None):
    return eid.to_bytes((eid.bit_length() + 7) // 8, 'big') + _size(len(payload), width) + payload


def _uint(eid, value, width=None):
    return _el(eid, value.to_bytes(width or max(1, (value.bit_length() + 7) // 8), 'big'))


def read_original(head):
    """From the original movie's first bytes: its EBML header, its one video track's elements and its first block."""
    top = elements(head, clip=True)
    header, segment_element = next(top, None), next(top, None)
    if not header or not segment_element or header[0] != EBML_HEADER or segment_element[0] != SEGMENT:
        raise ValueError('not a WebM file')
    ebml_header = head[header[1]:header[3]]
    segment = {}
    for eid, begin, inner, finish in elements(head, segment_element[2], segment_element[3], clip=True):
        segment.setdefault(eid, (begin, inner, finish))
        if eid == CLUSTER:   # the first cluster holds the first block; later ones may be cut
            break
    if TRACKS not in segment or CLUSTER not in segment:
        raise ValueError('the opening movie has no tracks or no cluster in its first %d bytes' % len(head))
    tracks = [e for e in elements(head, *segment[TRACKS][1:]) if e[0] == TRACK_ENTRY]
    if len(tracks) != 1:
        raise ValueError('the opening movie should have one track, it has %d' % len(tracks))
    track = _children(head, *tracks[0][2:])
    raw = {eid: head[begin:finish] for eid, (begin, _, finish) in track.items()}
    payload = {eid: head[inner:finish] for eid, (_, inner, finish) in track.items()}
    if payload.get(TRACK_TYPE) != b'\x01' or payload.get(CODEC_ID) != b'V_AV1':
        raise ValueError('the opening movie track is not AV1 video')
    cluster = next((e for e in elements(head, *segment[CLUSTER][1:], clip=True) if e[0] == SIMPLE_BLOCK), None)
    if cluster is None:
        raise ValueError('the first cluster has no block')
    return dict(ebml_header=ebml_header, codec_id=payload[CODEC_ID], flag_lacing=int.from_bytes(payload[FLAG_LACING], 'big'),
                default_duration=raw[DEFAULT_DURATION], default_duration_ns=int.from_bytes(payload[DEFAULT_DURATION], 'big'),
                video=raw[VIDEO], codec_private=raw[CODEC_PRIVATE], first_block=head[cluster[2]:cluster[3]])


def one_frame(original):
    """The one-keyframe WebM for read_original's result, laid out as ffmpeg's remux (see the module notes)."""
    block = original['first_block']
    track_number, length, _ = _vint(block, 0)
    if track_number != 1 or not block[length + 2] & 0x80:
        raise ValueError('the first block is not a keyframe of track 1')
    duration_ms = original['default_duration_ns'] // 1_000_000   # one frame, in the 1 ms ticks ffmpeg writes
    info = _el(INFO, _uint(TIMECODE_SCALE, 1_000_000) + _el(MUXING_APP, b'Lavf') + _el(WRITING_APP, b'Lavf')
               + _el(DURATION, struct.pack('>d', float(duration_ms))))
    entry = (_uint(TRACK_NUMBER, 1) + _uint(TRACK_UID, 1, 8) + _uint(FLAG_LACING, original['flag_lacing'])
             + _el(LANGUAGE, b'und') + _el(CODEC_ID, original['codec_id']) + _uint(TRACK_TYPE, 1)
             + original['default_duration'] + original['video'] + original['codec_private'])
    tracks = _el(TRACKS, _el(TRACK_ENTRY, entry, 8))
    seconds = duration_ms / 1000
    stamp = '%02d:%02d:%012.9f' % (seconds // 3600, seconds % 3600 // 60, seconds % 60)
    tags = _el(TAGS, _el(TAG, _el(TARGETS, _uint(TAG_TRACK_UID, 1, 8))
                         + _el(SIMPLE_TAG, _el(TAG_NAME, b'DURATION') + _el(TAG_STRING, stamp.encode() + b'\0'))))
    cluster_payload = _uint(TIMECODE, 0) + _el(SIMPLE_BLOCK, block)
    cluster = _el(CLUSTER, cluster_payload)
    tags_at = INFO_AT + len(info) + len(tracks)
    cluster_at = tags_at + len(tags)
    cues_at = cluster_at + len(cluster)
    block_in_cluster = len(_uint(TIMECODE, 0))
    cues = _el(CUES, _el(CUE_POINT, _uint(CUE_TIME, 0) + _el(CUE_TRACK_POSITIONS, _uint(CUE_TRACK, 1)
               + _uint(CUE_CLUSTER_POSITION, cluster_at) + _uint(CUE_RELATIVE_POSITION, block_in_cluster))))
    seeks = b''.join(_el(SEEK, _el(SEEK_ID, eid.to_bytes(4, 'big')) + _uint(SEEK_POSITION, at))
                     for eid, at in ((INFO, INFO_AT), (TRACKS, INFO_AT + len(info)), (TAGS, tags_at), (CUES, cues_at)))
    seek_head = _el(SEEK_HEAD, seeks)
    room = INFO_AT - len(seek_head) - 9   # a Void with an 8-byte size fills the rest of the reserved space
    if room < 0:
        raise ValueError('the SeekHead does not fit in the space before Info')
    segment = seek_head + _el(VOID, bytes(room), 8) + info + tracks + tags + cluster + cues
    return original['ebml_header'] + _el(SEGMENT, segment, 8)


def inspect(data):
    """What a WebM holds, read independently of one_frame: tracks (codec, size), blocks (track, keyframe, payload) and the
    duration in seconds."""
    top = list(elements(data))
    if [e[0] for e in top] != [EBML_HEADER, SEGMENT] or top[1][3] != len(data):
        raise ValueError('a WebM is an EBML header and one Segment')
    segment = _children(data, top[1][2], top[1][3])
    info = _children(data, *segment[INFO][1:])
    scale = int.from_bytes(data[info[TIMECODE_SCALE][1]:info[TIMECODE_SCALE][2]], 'big')
    duration = struct.unpack('>d', data[info[DURATION][1]:info[DURATION][2]])[0] * scale / 1e9
    tracks = []
    for eid, _, inner, finish in elements(data, *segment[TRACKS][1:]):
        if eid != TRACK_ENTRY:
            continue
        track = _children(data, inner, finish)
        video = _children(data, *track[VIDEO][1:]) if VIDEO in track else {}
        value = lambda kids, key: data[kids[key][1]:kids[key][2]]
        tracks.append(dict(codec=value(track, CODEC_ID).decode(), width=int.from_bytes(value(video, PIXEL_WIDTH), 'big'),
                           height=int.from_bytes(value(video, PIXEL_HEIGHT), 'big')))
    blocks = []
    for eid, _, inner, finish in elements(data, *segment[CLUSTER][1:]):
        if eid == SIMPLE_BLOCK:
            number, length, _ = _vint(data, inner)
            blocks.append(dict(track=number, keyframe=bool(data[inner + length + 2] & 0x80), payload=data[inner + length + 3:finish]))
    return dict(tracks=tracks, blocks=blocks, duration=duration, clusters=sum(1 for e in elements(data, top[1][2], top[1][3]) if e[0] == CLUSTER))
