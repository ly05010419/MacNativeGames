// bink_decode <movie.bik>: writes the movie's frames to stdout as raw
// yuv420p, decoded as binkw32 shows them (from black, bink_format.h), for an
// encoder to read (ffmpeg -f rawvideo). Prints "<width> <height> <frames>
// <rate> <div>" on stderr first. Exits non-zero when the movie cannot be
// decoded or yields a different number of frames than its header says.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "runtime/hle/bink_format.h"

namespace {

using recomp::bink::Le32;

int Fail(const char* path, const char* what) {
  std::fprintf(stderr, "bink_decode: %s: %s\n", path, what);
  return 1;
}

// One frame as yuv420p: the planes, rows packed.
bool WriteFrame(const AVFrame* f, SwsContext** sws, std::vector<uint8_t>* buf) {
  const int w = f->width, h = f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  buf->resize(size_t(w) * h + 2 * size_t(cw) * ch);
  uint8_t* dst[4] = {buf->data(), buf->data() + size_t(w) * h,
                     buf->data() + size_t(w) * h + size_t(cw) * ch, nullptr};
  int stride[4] = {w, cw, cw, 0};
  *sws = sws_getCachedContext(*sws, w, h, AVPixelFormat(f->format), w, h, AV_PIX_FMT_YUV420P,
                              SWS_POINT, nullptr, nullptr, nullptr);
  if (!*sws) return false;
  sws_scale(*sws, f->data, f->linesize, 0, h, dst, stride);
  return std::fwrite(buf->data(), 1, buf->size(), stdout) == buf->size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: bink_decode <movie.bik>\n");
    return 2;
  }
  const char* path = argv[1];
  // The header as binkw32 reads it: frames at +8, width +20, height +24,
  // rate +28 / +32.
  uint8_t h[44];
  FILE* in = std::fopen(path, "rb");
  const bool read = in && std::fread(h, 1, sizeof h, in) == sizeof h;
  if (in) std::fclose(in);
  if (!read || std::memcmp(h, "BIK", 3) != 0) return Fail(path, "no Bink header");
  const int64_t frames = Le32(h + 8);
  av_log_set_level(AV_LOG_ERROR);
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path, av_find_input_format("bink"), nullptr) < 0 ||
      avformat_find_stream_info(fmt, nullptr) < 0)
    return Fail(path, "not a Bink movie ffmpeg can read");
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) return Fail(path, "no video stream");
  AVStream* st = fmt->streams[vs];
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return Fail(path, "no decoder");
  dec->get_buffer2 = recomp::bink::BlackGetBuffer;
  if (avcodec_open2(dec, codec, nullptr) < 0) return Fail(path, "cannot open the decoder");
  std::fprintf(stderr, "%u %u %lld %u %u\n", Le32(h + 20), Le32(h + 24), (long long)frames,
               Le32(h + 28), Le32(h + 32));

  AVPacket* pkt = av_packet_alloc();
  AVFrame* f = av_frame_alloc();
  SwsContext* sws = nullptr;
  std::vector<uint8_t> buf;
  int64_t n = 0;
  bool eof = false, ok = true;
  while (ok) {
    const int r = avcodec_receive_frame(dec, f);
    if (r == 0) {
      ok = WriteFrame(f, &sws, &buf);
      av_frame_unref(f);
      n++;
      continue;
    }
    if (r != AVERROR(EAGAIN) || eof) break;
    if (av_read_frame(fmt, pkt) < 0) {
      avcodec_send_packet(dec, nullptr);
      eof = true;
      continue;
    }
    if (pkt->stream_index == vs) avcodec_send_packet(dec, pkt);
    av_packet_unref(pkt);
  }
  sws_freeContext(sws);
  av_frame_free(&f);
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  if (!ok) return Fail(path, "cannot write a frame");
  if (n != frames) {
    std::fprintf(stderr, "bink_decode: %s: %lld frames decoded, the header says %lld\n", path,
                 (long long)n, (long long)frames);
    return 1;
  }
  return 0;
}
