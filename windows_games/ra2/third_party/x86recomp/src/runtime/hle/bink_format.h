// What binkw32 and tools/bink_decode share about Bink movies.
//
// Bink's skip blocks copy from the previous frame. RAD's decoder starts
// from black; ffmpeg's first reference is all zero bytes, which is bright
// green in YUV (a credits movie may keep its whole background as skip
// blocks). So every frame buffer starts out black: Y 0, U/V 128, A 255.
// Shared by binkw32 and tools/bink_decode (which makes compact movies from
// the same frames the game would show).
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::bink {

inline int BlackGetBuffer(AVCodecContext* ctx, AVFrame* f, int flags) {
  int r = avcodec_default_get_buffer2(ctx, f, flags);
  if (r < 0) return r;
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(f->format));
  if (!d || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || (d->flags & AV_PIX_FMT_FLAG_RGB))
    return r;
  for (int p = 0; p < 4 && f->data[p]; p++) {
    int h = f->height;
    if (p == 1 || p == 2) h = -((-h) >> d->log2_chroma_h);
    uint8_t v = (p == 1 || p == 2) ? 128 : p == 3 ? 255 : 0;
    std::memset(f->data[p], v, size_t(f->linesize[p]) * h);
  }
  return r;
}

inline uint32_t Le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}

// Compact movies (made by a packaging step from tools/bink_decode's
// frames): "XRMV", the size of the rest (where a Bink header has it, at +4,
// so archives and file handles read both the same way), the length of the
// original Bink header, that header (width, height, frames, rate, audio
// track IDs: what the game sees), then a container ffmpeg reads with the
// video and one audio stream per Bink track, in the header's order.
constexpr char kCompactMagic[4] = {'X', 'R', 'M', 'V'};

// A Bink movie or a compact one (8 bytes readable).
inline bool IsMovie(const uint8_t* p) {
  return !std::memcmp(p, "BIK", 3) || !std::memcmp(p, kCompactMagic, 4);
}

enum class Compact { No, Yes, Broken };

// When `data` is a compact movie: its Bink header into *header and the
// container left in `data`.
inline Compact SplitCompact(std::vector<uint8_t>& data, std::vector<uint8_t>* header) {
  if (data.size() < 12 || std::memcmp(data.data(), kCompactMagic, 4) != 0) return Compact::No;
  const uint32_t n = Le32(data.data() + 8);
  if (n < 44 || 12 + uint64_t(n) > data.size() || std::memcmp(data.data() + 12, "BIK", 3) != 0)
    return Compact::Broken;
  header->assign(data.begin() + 12, data.begin() + 12 + n);
  data.erase(data.begin(), data.begin() + 12 + n);
  return Compact::Yes;
}

}  // namespace recomp::bink
