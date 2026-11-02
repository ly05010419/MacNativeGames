#include "runtime/audio/source.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace recomp::audio {

namespace {

uint32_t Le32(const uint8_t* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// ---- WAV -------------------------------------------------------------------

class WavSource : public Source {
 public:
  WavSource(Bytes data, const WavInfo& w) : data_(std::move(data)), w_(w) {
    channels_ = w.channels;
    rate_ = int(w.rate);
    frames_ = w.data_len / w.block_align;
  }
  int Read(float* out, int n) override {
    int got = int(std::min<int64_t>(n, frames_ - pos_));
    const uint8_t* p = data_->data() + w_.data_offset + pos_ * w_.block_align;
    int samples = got * channels_;
    if (w_.bits == 16) {
      for (int i = 0; i < samples; i++)
        out[i] = int16_t(Le16(p + 2 * i)) * (1.0f / 32768.0f);
    } else {
      for (int i = 0; i < samples; i++) out[i] = (p[i] - 128) * (1.0f / 128.0f);
    }
    pos_ += got;
    return got;
  }
  bool Seek(int64_t f) override {
    pos_ = std::clamp<int64_t>(f, 0, frames_);
    return true;
  }
  int64_t Length() const override { return frames_; }
  int bytes_per_frame() const override { return w_.block_align; }

 private:
  Bytes data_;
  WavInfo w_;
  int64_t frames_ = 0, pos_ = 0;
};

// ---- MP3 -------------------------------------------------------------------

class Mp3Source : public Source {
 public:
  explicit Mp3Source(Bytes data) : data_(std::move(data)) {}
  ~Mp3Source() override {
    av_parser_close(parser_);
    avcodec_free_context(&ctx_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
  }

  bool Init(std::string* err) {
    codec_ = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!codec_) return *err = "no MP3 decoder in libavcodec", false;
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!Reset()) return *err = "cannot open the MP3 decoder", false;
    // Decode the first frame to learn the format.
    if (!Decode() || pending_.empty())
      return *err = "no decodable MPEG audio frames", false;
    return true;
  }

  int Read(float* out, int n) override {
    int done = 0;
    while (done < n) {
      size_t avail = (pending_.size() - pending_pos_) / channels_;
      if (!avail) {
        if (!Decode()) break;
        continue;
      }
      size_t take = std::min<size_t>(avail, n - done);
      std::memcpy(out + done * channels_, pending_.data() + pending_pos_,
                  take * channels_ * sizeof(float));
      pending_pos_ += take * channels_;
      done += int(take);
    }
    return done;
  }

  bool Seek(int64_t f) override {
    if (f != 0) return false;
    return Reset();
  }

 private:
  bool Reset() {
    av_parser_close(parser_);
    avcodec_free_context(&ctx_);
    parser_ = av_parser_init(AV_CODEC_ID_MP3);
    ctx_ = avcodec_alloc_context3(codec_);
    if (!parser_ || !ctx_ || avcodec_open2(ctx_, codec_, nullptr) < 0)
      return false;
    off_ = 0;
    flushed_ = false;
    pending_.clear();
    pending_pos_ = 0;
    return true;
  }

  // Decodes the next frame into pending_. False at the end of the data.
  bool Decode() {
    pending_.clear();
    pending_pos_ = 0;
    for (;;) {
      int r = avcodec_receive_frame(ctx_, frame_);
      if (r == 0) {
        Convert();
        av_frame_unref(frame_);
        if (!pending_.empty()) return true;
        continue;
      }
      if (r == AVERROR_EOF) return false;
      if (r != AVERROR(EAGAIN)) return false;
      if (flushed_) return false;
      // Feed the parser until it yields a packet.
      const uint8_t* in = data_->data() + off_;
      int left = int(data_->size() - off_);
      uint8_t* out = nullptr;
      int out_size = 0;
      int used = av_parser_parse2(parser_, ctx_, &out, &out_size, left ? in : nullptr,
                                  left, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) return false;
      off_ += used;
      if (out_size) {
        pkt_->data = out;
        pkt_->size = out_size;
        if (avcodec_send_packet(ctx_, pkt_) < 0) {
          // A corrupt frame: skip it like a hardware decoder would.
          continue;
        }
      } else if (!left) {
        avcodec_send_packet(ctx_, nullptr);  // drain
        flushed_ = true;
      }
    }
  }

  void Convert() {
    if (!channels_) {
      channels_ = std::min(frame_->ch_layout.nb_channels, 2);
      rate_ = frame_->sample_rate;
    }
    pending_.resize(size_t(frame_->nb_samples) * channels_);
    ConvertFrame(frame_, channels_, pending_.data());
  }

  Bytes data_;
  const AVCodec* codec_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  size_t off_ = 0;
  bool flushed_ = false;
  std::vector<float> pending_;
  size_t pending_pos_ = 0;
};

}  // namespace

uint32_t RiffSize(const uint8_t* p) {
  if (std::memcmp(p, "RIFF", 4) != 0) return 0;
  return Le32(p + 4) + 8;
}

bool ParseWav(const uint8_t* p, size_t len, WavInfo* w) {
  if (len < 12 || std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "WAVE", 4))
    return false;
  bool have_fmt = false;
  size_t off = 12;
  while (off + 8 <= len) {
    uint32_t id = Le32(p + off), size = Le32(p + off + 4);
    size_t body = off + 8;
    if (id == 0x20746D66 && size >= 16 && body + 16 <= len) {  // "fmt "
      w->format = Le16(p + body);
      w->channels = Le16(p + body + 2);
      w->rate = Le32(p + body + 4);
      w->block_align = Le16(p + body + 12);
      w->bits = Le16(p + body + 14);
      have_fmt = true;
    } else if (id == 0x61746164) {  // "data"
      if (!have_fmt) return false;
      w->data_offset = uint32_t(body);
      // Some writers put a bogus size on the last chunk; clamp to the file.
      w->data_len = uint32_t(std::min<size_t>(size, len - body));
      return true;
    }
    off = body + size + (size & 1);
  }
  return false;
}

std::unique_ptr<Source> MakeWavSource(Bytes data, std::string* err) {
  WavInfo w;
  if (!ParseWav(data->data(), data->size(), &w)) {
    *err = "malformed WAV";
    return nullptr;
  }
  if (w.format != 1 || (w.bits != 8 && w.bits != 16) || w.channels < 1 ||
      w.channels > 2 || w.block_align != w.channels * w.bits / 8 || !w.rate) {
    *err = "unsupported WAV format " + std::to_string(w.format) + " (" +
           std::to_string(w.channels) + " ch, " + std::to_string(w.bits) + " bit)";
    return nullptr;
  }
  return std::make_unique<WavSource>(std::move(data), w);
}

// ---- headerless PCM and IMA ADPCM ------------------------------------------

namespace {

// Already-decoded frames.
class FloatSource : public Source {
 public:
  FloatSource(std::vector<float> frames, int channels, int rate)
      : f_(std::move(frames)) {
    channels_ = channels;
    rate_ = rate;
    n_ = int64_t(f_.size()) / channels;
  }
  int Read(float* out, int n) override {
    int got = int(std::min<int64_t>(n, n_ - pos_));
    std::memcpy(out, f_.data() + pos_ * channels_, size_t(got) * channels_ * sizeof(float));
    pos_ += got;
    return got;
  }
  bool Seek(int64_t f) override {
    pos_ = std::clamp<int64_t>(f, 0, n_);
    return true;
  }
  int64_t Length() const override { return n_; }

 private:
  std::vector<float> f_;
  int64_t n_ = 0, pos_ = 0;
};

// IMA ADPCM (the step table and index adjustments of the IMA standard).
constexpr int16_t kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
constexpr int kImaIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
  int pred = 0, index = 0;
  int Decode(int nibble) {
    int step = kImaStep[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    pred = std::clamp(pred + diff, -32768, 32767);
    index = std::clamp(index + kImaIndex[nibble & 7], 0, 88);
    return pred;
  }
};

// One WAV IMA ADPCM block: a 4-byte header per channel (first sample,
// step index), then 4-byte groups per channel in turn, low nibble first.
void DecodeImaBlock(const uint8_t* b, size_t len, int ch, std::vector<float>* out) {
  if (len < size_t(4 * ch)) return;
  ImaState st[2];
  for (int c = 0; c < ch; c++) {
    st[c].pred = int16_t(Le16(b + 4 * c));
    st[c].index = std::clamp<int>(b[4 * c + 2], 0, 88);
  }
  size_t data = len - 4 * size_t(ch);
  size_t per_ch = data * 2 / size_t(ch);  // samples per channel after the header
  size_t base = out->size();
  out->resize(base + (per_ch + 1) * size_t(ch));
  float* o = out->data() + base;
  for (int c = 0; c < ch; c++) o[c] = st[c].pred / 32768.0f;
  const uint8_t* p = b + 4 * ch;
  for (size_t group = 0; group * 8 < per_ch; group++)
    for (int c = 0; c < ch; c++)
      for (int k = 0; k < 4; k++) {
        uint8_t byte = p[(group * size_t(ch) + size_t(c)) * 4 + size_t(k)];
        for (int half = 0; half < 2; half++) {
          size_t i = group * 8 + size_t(k) * 2 + size_t(half);  // sample index
          int v = st[c].Decode(half ? byte >> 4 : byte & 0xF);
          if (i < per_ch) o[(1 + i) * size_t(ch) + size_t(c)] = v / 32768.0f;
        }
      }
}

}  // namespace

bool DecodeRaw(const uint8_t* data, size_t len, const RawFormat& f, std::vector<float>* out,
               std::string* err) {
  out->clear();
  if (f.channels < 1 || f.channels > 2 || f.rate <= 0) {
    *err = "bad channel count or rate";
    return false;
  }
  if (f.adpcm) {
    if (f.block_size < 4u * uint32_t(f.channels)) {
      *err = "IMA ADPCM without a block size";
      return false;
    }
    for (size_t off = 0; off < len; off += f.block_size)
      DecodeImaBlock(data + off, std::min<size_t>(f.block_size, len - off), f.channels, out);
    return true;
  }
  if (f.bits == 16) {
    out->resize(len / 2 / size_t(f.channels) * size_t(f.channels));
    for (size_t i = 0; i < out->size(); i++)
      (*out)[i] = int16_t(Le16(data + 2 * i)) * (1.0f / 32768.0f);
    return true;
  }
  if (f.bits == 8) {
    out->resize(len / size_t(f.channels) * size_t(f.channels));
    for (size_t i = 0; i < out->size(); i++)
      (*out)[i] = f.pcm_signed ? int8_t(data[i]) * (1.0f / 128.0f)
                               : (data[i] - 128) * (1.0f / 128.0f);
    return true;
  }
  *err = "unsupported PCM bit depth " + std::to_string(f.bits);
  return false;
}

std::unique_ptr<Source> MakeRawSource(const uint8_t* data, size_t len, const RawFormat& f,
                                      std::string* err) {
  std::vector<float> frames;
  if (!DecodeRaw(data, len, f, &frames, err)) return nullptr;
  return std::make_unique<FloatSource>(std::move(frames), f.channels, f.rate);
}

std::unique_ptr<Source> MakeMp3Source(Bytes data, std::string* err) {
  auto s = std::make_unique<Mp3Source>(std::move(data));
  if (!s->Init(err)) return nullptr;
  return s;
}

void ConvertFrame(const AVFrame* f, int out_ch, float* out) {
  int ch = f->ch_layout.nb_channels;
  auto fmt = AVSampleFormat(f->format);
  bool planar = av_sample_fmt_is_planar(fmt);
  auto packed = av_get_packed_sample_fmt(fmt);
  for (int i = 0; i < f->nb_samples; i++)
    for (int c = 0; c < out_ch; c++) {
      int sc = std::min(c, ch - 1);
      float v = 0;
      if (packed == AV_SAMPLE_FMT_FLT)
        v = planar ? reinterpret_cast<const float*>(f->data[sc])[i]
                   : reinterpret_cast<const float*>(f->data[0])[i * ch + sc];
      else if (packed == AV_SAMPLE_FMT_S16)
        v = (planar ? reinterpret_cast<const int16_t*>(f->data[sc])[i]
                    : reinterpret_cast<const int16_t*>(f->data[0])[i * ch + sc]) /
            32768.0f;
      out[size_t(i) * out_ch + c] = v;
    }
}

std::unique_ptr<Source> MakeSource(Bytes data, std::string* err) {
  if (data->size() >= 12 && RiffSize(data->data()))
    return MakeWavSource(std::move(data), err);
  return MakeMp3Source(std::move(data), err);
}

// ---- QueueSource -------------------------------------------------------------

QueueSource::QueueSource(int channels, int rate) {
  channels_ = channels;
  rate_ = rate;
}

void QueueSource::Push(const float* frames, int n) {
  std::lock_guard<std::mutex> l(mu_);
  if (head_ > (1u << 16) && head_ * 2 > q_.size()) {
    q_.erase(q_.begin(), q_.begin() + head_);
    head_ = 0;
  }
  q_.insert(q_.end(), frames, frames + size_t(n) * channels_);
}

void QueueSource::Clear() {
  std::lock_guard<std::mutex> l(mu_);
  q_.clear();
  head_ = 0;
}

int64_t QueueSource::Consumed() {
  std::lock_guard<std::mutex> l(mu_);
  return consumed_;
}

void QueueSource::Finish() {
  std::lock_guard<std::mutex> l(mu_);
  finished_ = true;
}

int QueueSource::Read(float* out, int n) {
  std::lock_guard<std::mutex> l(mu_);
  size_t avail = (q_.size() - head_) / channels_;
  size_t take = std::min<size_t>(avail, n);
  std::memcpy(out, q_.data() + head_, take * channels_ * sizeof(float));
  head_ += take * channels_;
  consumed_ += int64_t(take);
  if (finished_) return int(take);
  // Underrun: keep the voice alive with silence.
  std::fill(out + take * channels_, out + size_t(n) * channels_, 0.0f);
  return n;
}

}  // namespace recomp::audio
