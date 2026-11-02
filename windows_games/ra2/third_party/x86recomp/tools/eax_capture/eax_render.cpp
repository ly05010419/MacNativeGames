// eax_render: renders the capture cases (cases.c) with x86recomp's own
// audio path, driven the way mss32.cpp drives it for Miles' Fast 2D and EAX
// providers: EAX properties -> per-voice direct and reverb-send levels ->
// the mixer and its reverb. Writes what eaxcap.exe records from
// real hardware (a WAV and a JSON manifest per case and stimulus, plus the
// stimuli) so analyze.py can compare the two. Offline: the mixer is pulled
// directly and no audio device is opened. "DirectSound3D Hardware Support"
// (ds3dhw), which mss32.cpp does not offer, is the mixer's DS3D pan and
// distance law alone, for comparison with that provider's recordings.
//
//   eax_render --out DIR [--provider fast2d,ds3dhw,eax1,eax2,eax3]
//              [--rate 48000] [--cases LIST] [--stim LIST]
//              [--miles-version 6.5|6.1] [--quiet]
//
// --miles-version picks whose Fast 2D the fast2d set renders: Miles 6.5's
// (msssoft.m3d, the default: eaxcap.exe records the Miles a game ships,
// e.g. 6.5e) or 6.1c's (Mssfast.m3d).

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cases.h"
#include "runtime/audio/eax.h"
#include "runtime/audio/mixer.h"
#include "runtime/audio/source.h"

namespace recomp {

static bool g_quiet = false;

// recomp_audio logs through the runtime's WarnF; this one, which takes its
// place, honours --quiet.
void WarnF(const char* fmt, ...) {
  if (g_quiet) return;
  std::fprintf(stderr, "warning: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

}  // namespace recomp

namespace {

using namespace recomp;
namespace fs = std::filesystem;
namespace eax = audio::eax;

constexpr int kChunk = 1024;  // frames per Mix call, as the audio callback
constexpr int kBusEax = 0;    // mss32.cpp's EAX reverb bus

// The providers mss32.cpp offers, and DS3D hardware.
const char* const kProviders[] = {
    "Miles Fast 2D Positional Audio",
    "DirectSound3D Hardware Support",
    "Creative Labs EAX (TM)",
    "Creative Labs EAX 2 (TM)",
    "Creative Labs EAX 3 (TM)",
};

struct Options {
  std::string out = "render", providers = "fast2d,ds3dhw,eax1,eax2,eax3", cases;
  unsigned stimuli = 0;
  int rate = 48000;
  bool miles65 = true;
};

// ---- JSON, as eaxcap.c writes it ------------------------------------------------

class Json {
 public:
  void Open(const char* key, char bracket) {
    if (key)
      Key(key);
    else
      Sep();
    s_ += bracket;
    comma_ = false;
  }
  void Close(char bracket) {
    s_ += bracket;
    comma_ = true;
  }
  void Str(const char* key, const std::string& v) {
    Key(key);
    Quote(v);
  }
  void Num(const char* key, double v) {
    Key(key);
    Raw(v);
  }
  void Null(const char* key) {
    Key(key);
    s_ += "null";
  }
  void Vec(const char* key, const float* v) {
    Key(key);
    char b[100];
    std::snprintf(b, sizeof b, "[%.9g, %.9g, %.9g]", v[0], v[1], v[2]);
    s_ += b;
  }
  void Key(const char* k) {
    Sep();
    Quote(k);
    s_ += ": ";
  }
  void Raw(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", v);
    s_ += b;
  }
  void Text(const char* t) { s_ += t; }
  const std::string& str() const { return s_; }

 private:
  void Sep() {
    if (comma_) s_ += ",\n";
    comma_ = true;
  }
  void Quote(const std::string& v) {
    s_ += '"';
    for (char ch : v) {
      if (ch == '"' || ch == '\\') s_ += '\\';
      s_ += ch;
    }
    s_ += '"';
  }
  std::string s_;
  bool comma_ = false;
};

bool WriteFile(const fs::path& p, const void* data, size_t n) {
  FILE* f = std::fopen(p.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "error: cannot write %s\n", p.c_str());
    return false;
  }
  bool ok = std::fwrite(data, 1, n, f) == n;
  std::fclose(f);
  return ok;
}

// 32-bit float stereo WAV.
void WriteWavF32(const fs::path& p, const std::vector<float>& x, int rate) {
  std::vector<uint8_t> w(44 + x.size() * 4);
  auto put16 = [&](size_t o, uint32_t v) { w[o] = uint8_t(v), w[o + 1] = uint8_t(v >> 8); };
  auto put32 = [&](size_t o, uint32_t v) { put16(o, v & 0xFFFF), put16(o + 2, v >> 16); };
  uint32_t data = uint32_t(x.size() * 4);
  std::memcpy(&w[0], "RIFF", 4);
  put32(4, 36 + data);
  std::memcpy(&w[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 3);  // WAVE_FORMAT_IEEE_FLOAT
  put16(22, 2);
  put32(24, uint32_t(rate));
  put32(28, uint32_t(rate) * 8);
  put16(32, 8);
  put16(34, 32);
  std::memcpy(&w[36], "data", 4);
  put32(40, data);
  std::memcpy(&w[44], x.data(), x.size() * 4);
  WriteFile(p, w.data(), w.size());
}

audio::Bytes WavImage(const float* x, int frames, int rate) {
  unsigned size = 0;
  unsigned char* w = ec_wav16(x, frames, 1, rate, &size);
  auto b = std::make_shared<std::vector<uint8_t>>(w, w + size);
  std::free(w);
  return b;
}

void SetValue(char type, double v, uint8_t* buf) {
  if (type == 'f') {
    float f = float(v);
    std::memcpy(buf, &f, 4);
  } else if (type == 'u') {
    uint32_t u = uint32_t(v);
    std::memcpy(buf, &u, 4);
  } else {
    int32_t i = int32_t(v);
    std::memcpy(buf, &i, 4);
  }
}

// ---- one provider, one case, one stimulus ---------------------------------------------

struct Stim {
  unsigned bit;
  std::vector<float> x;  // without the lead
  audio::Bytes image;    // 16-bit WAV: lead silence + stimulus, as Miles plays it
};

// Applies attributes as AIL_set_3D_provider_preference /
// AIL_set_3D_sample_preference would and logs them as eaxcap.c does
// (result 0 = M3D_NOERR, 1 = not provided).
template <typename T, typename Set>
void ApplyAttrs(Json& j, const char* key, int ver, const EcAttr* a, int n, T* obj, Set set) {
  j.Open(key, '[');
  for (int i = 0; i < n; i++) {
    j.Open(nullptr, '{');
    j.Str("name", a[i].name);
    j.Num("value", a[i].value);
    if (ec_attr_applies(a[i].name, ver)) {
      uint8_t buf[12] = {};
      SetValue(a[i].type, a[i].value, buf);
      j.Num("result", set(obj, a[i].name, ver, buf, true) ? 0 : 1);
    } else {
      j.Str("result", "skipped");
    }
    j.Close('}');
  }
  j.Close(']');
}

template <typename T, typename Get>
void ReadBack(Json& j, const char* key, int ver, const EcAttrName* names, T* obj, Get get) {
  j.Open(key, '{');
  for (int i = 0; names[i].name; i++) {
    if (!ec_attr_applies(names[i].name, ver)) continue;
    uint8_t buf[12] = {};
    if (!get(obj, names[i].name, ver, buf, false)) {
      j.Null(names[i].name);
      continue;
    }
    j.Key(names[i].name);
    int32_t iv;
    uint32_t uv;
    float f[3];
    std::memcpy(&iv, buf, 4);
    std::memcpy(&uv, buf, 4);
    std::memcpy(f, buf, 12);
    switch (names[i].type) {
      case 'i': j.Raw(iv); break;
      case 'u': j.Raw(uv); break;
      case 'v': {
        char b[100];
        std::snprintf(b, sizeof b, "[%.9g, %.9g, %.9g]", f[0], f[1], f[2]);
        j.Text(b);
        break;
      }
      default: j.Raw(f[0]);
    }
  }
  j.Close('}');
}

// A 3D sample's direct and reverb-send levels, as mss32.cpp's ApplyEax
// sets them (the listener at the origin, distance factor 1).
void ApplyLevels(audio::Mixer& m, audio::Voice& v, const eax::Source& s,
                 const eax::Listener& listener, int ver) {
  if (!ver && v.ds3d) return;  // DS3D hardware: the direct path alone
  if (!ver) {
    // Fast 2D: the driver's dry level (a Miles 6.5 voice gets it in the
    // mixer); no master reverb wet level is set.
    if (v.miles == audio::Voice::Miles::k61)
      v.direct_lf = v.direct_hf = m.digital_reverb().dry();
    return;
  }
  eax::Listener l = listener;
  if (ver == 1) l.air_absorption_hf = 0;  // EAX 1.0 has none
  float d = std::sqrt(v.pos.x * v.pos.x + v.pos.y * v.pos.y + v.pos.z * v.pos.z);
  eax::Levels lv = eax::SourceLevels(s, l, d, v.min_dist, ver);
  v.direct_lf = lv.direct_lf;
  v.direct_hf = lv.direct_hf;
  v.send_lf[kBusEax] = lv.room_lf;
  v.send_hf[kBusEax] = lv.room_hf;
}

void InitVoice(audio::Voice& v, const audio::Bytes& image, const float* pos, const EcCase& c,
               audio::Voice::Miles miles, bool ds3d) {
  std::string err;
  v.src = audio::MakeWavSource(image, &err);
  if (!v.src) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    std::exit(1);
  }
  v.miles = miles;
  v.positional = true;
  v.ds3d = ds3d;
  v.pos = {pos[0], pos[1], pos[2]};
  v.min_dist = c.min_dist;
  v.max_dist = c.max_dist;
  v.Seek(0);
  v.passes = 0;
  v.gains_valid_ = false;
  v.state = audio::Voice::State::Playing;
}

void Render(const Options& o, const char* provider, const std::string& tag, int ver,
            const EcCase& c, const Stim& st, const audio::Bytes& sync_image,
            const fs::path& dir) {
  const int rate = o.rate;
  const int64_t preroll = int64_t(EC_PREROLL * rate), lead = int64_t(EC_LEAD * rate);
  Json j;
  j.Open(nullptr, '{');
  j.Str("tool", "eax_render");
  j.Str("case", c.name);
  j.Str("group", c.group);
  j.Str("stimulus", ec_stim_name(st.bit));
  j.Str("provider", provider);
  j.Str("provider_tag", tag);
  j.Num("eax_version", ver);
  if (!ver) j.Str("miles", o.miles65 ? "6.5" : "6.1c");
  j.Num("rate", rate);

  // The provider's listener: EAX's defaults, the environment, the case.
  eax::Listener l;
  if (ver && c.environment >= 0) {
    uint32_t env = uint32_t(c.environment);
    eax::ListenerAttribute(&l, "EAX environment selection", ver, &env, true);
    j.Num("environment", env);
    j.Num("environment_result", 0);
  } else {
    j.Null("environment");
  }
  ApplyAttrs(j, "listener_set", ver, c.l, c.nl, &l, eax::ListenerAttribute);

  // The stimulus sample and the sync click as the provider allocates them,
  // then every EAX property set explicitly, as eaxcap.exe does.
  eax::Source src, sync_src;
  if (ver == 2) src.occlusion_room_ratio = sync_src.occlusion_room_ratio = eax::kOcclusionRoomRatio2;
  const EcAttr* defs;
  int ndefs = ec_source_defaults(ver, &defs);
  ApplyAttrs(j, "source_defaults_set", ver, defs, ndefs, &src, eax::SourceAttribute);
  ApplyAttrs(j, "source_set", ver, c.s, c.ns, &src, eax::SourceAttribute);
  // The click: direct path only.
  EcAttr sync_attr = ver >= 2 ? EcAttr{"EAX2 sample room", 'i', -10000}
                              : EcAttr{"EAX sample reverb mix", 'f', 0.0};
  j.Open("sync", '{');
  j.Vec("pos", EC_SYNC_POS);
  ApplyAttrs(j, "defaults_set", ver, defs, ndefs, &sync_src, eax::SourceAttribute);
  ApplyAttrs(j, "set", ver, &sync_attr, ver ? 1 : 0, &sync_src, eax::SourceAttribute);
  j.Close('}');
  if (ver) {
    ReadBack(j, "listener_readback", ver, ec_listener_names, &l, eax::ListenerAttribute);
    ReadBack(j, "source_readback", ver, ec_source_names, &src, eax::SourceAttribute);
    ReadBack(j, "sync_readback", ver, ec_source_names, &sync_src, eax::SourceAttribute);
  }

  double decay = ver ? l.decay_time : 0.1;
  if (ec_case_dry(&c, ver)) decay = 0.1;
  double tail = ec_tail(decay, ver ? l.reflections_delay : 0.0, ver ? l.reverb_delay : 0.0);
  int64_t total = preroll + lead + int64_t(st.x.size()) + int64_t(tail * rate);

  // A fresh mixer per recording: nothing carries over between cases.
  audio::Mixer m;
  m.OpenOffline(rate);
  m.set_master(1.0f);
  m.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
  if (ver) m.SetReverb(kBusEax, eax::ToReverb(l, ver));

  // EAX voices take the 6.1c gains (DS3D positioning), as mss32.cpp does;
  // so does ds3dhw.
  auto miles = !ver && o.miles65 && tag != "ds3dhw" ? audio::Voice::Miles::k65
                                                     : audio::Voice::Miles::k61;
  audio::Voice stim, sync;
  bool ds3d = ver > 0 || tag == "ds3dhw";
  InitVoice(stim, st.image, c.pos, c, miles, ds3d);
  InitVoice(sync, sync_image, EC_SYNC_POS, c, miles, ds3d);
  ApplyLevels(m, stim, src, l, ver);
  ApplyLevels(m, sync, sync_src, l, ver);

  std::vector<float> out(size_t(total) * 2);
  for (int64_t pos = 0; pos < total;) {
    if (pos == preroll) {
      std::lock_guard<std::mutex> g(m.mu());
      m.Add(&sync);
      m.Add(&stim);
    }
    int64_t n = std::min<int64_t>(kChunk, total - pos);
    if (pos < preroll) n = std::min(n, preroll - pos);
    m.Mix(&out[size_t(pos) * 2], int(n));
    pos += n;
  }
  {
    std::lock_guard<std::mutex> g(m.mu());
    m.Remove(&sync);
    m.Remove(&stim);
  }
  m.Close();

  float peak = 0;
  for (float v : out) peak = std::max(peak, std::fabs(v));
  double peak_db = peak > 0 ? 20 * std::log10(peak) : -999.0;
  j.Vec("source_pos", c.pos);
  static const float zero[3] = {0, 0, 0}, face[3] = {0, 0, 1}, up[3] = {0, 1, 0};
  j.Vec("listener_pos", zero);
  j.Vec("listener_face", face);
  j.Vec("listener_up", up);
  j.Num("min_dist", c.min_dist);
  j.Num("max_dist", c.max_dist);
  j.Num("preroll_s", EC_PREROLL);
  j.Num("lead_s", EC_LEAD);
  j.Num("stim_frames", double(st.x.size()));
  j.Num("decay_for_tail_s", decay);
  j.Num("tail_s", tail);
  j.Num("record_frames", double(total));
  j.Num("started_at_frame", double(preroll));
  j.Num("peak_dbfs", peak_db);
  j.Str("renderer", "x86recomp recomp_audio");
  j.Close('}');
  j.Text("\n");

  std::string base = std::string(c.name) + "." + ec_stim_name(st.bit);
  WriteFile(dir / (base + ".json"), j.str().data(), j.str().size());
  WriteWavF32(dir / (base + ".wav"), out, rate);
  if (!g_quiet)
    std::printf("  %-28s %-7s %5.1f s peak %6.1f dBFS\n", c.name, ec_stim_name(st.bit),
                double(total) / rate, peak_db);
}

bool Selected(const std::string& list, const std::string& tag, const std::string& name) {
  if (list == "all") return true;
  size_t at = 0;
  while (at <= list.size()) {
    size_t e = list.find(',', at);
    std::string item = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
    if (item == tag || item == name) return true;
    if (e == std::string::npos) break;
    at = e + 1;
  }
  return false;
}

int Usage() {
  std::fprintf(stderr,
               "usage: eax_render [--out DIR] [--provider fast2d,ds3dhw,eax1,eax2,eax3|all]\n"
               "                  [--rate 44100|48000] [--cases LIST] [--stim LIST]\n"
               "                  [--miles-version 6.5|6.1] [--quiet]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    if (a == "--quiet") {
      g_quiet = true;
    } else if (!v) {
      return Usage();
    } else if (a == "--out") {
      o.out = argv[++i];
    } else if (a == "--provider") {
      o.providers = argv[++i];
    } else if (a == "--rate") {
      o.rate = std::atoi(argv[++i]);
    } else if (a == "--cases") {
      o.cases = argv[++i];
    } else if (a == "--miles-version") {
      std::string m = argv[++i];
      if (m != "6.5" && m != "6.1") return Usage();
      o.miles65 = m == "6.5";
    } else if (a == "--stim") {
      o.stimuli = ec_stim_parse(argv[++i]);
      if (!o.stimuli) return Usage();
    } else {
      return Usage();
    }
  }
  if (o.rate != 44100 && o.rate != 48000) return Usage();

  // The stimuli and the sync click, exactly as eaxcap.exe builds them.
  std::vector<Stim> stims;
  for (unsigned bit = 1; bit <= EC_STIM_TONE; bit <<= 1) {
    float* x;
    int n = ec_stimulus(bit, o.rate, &x);
    Stim s{bit, std::vector<float>(x, x + n), nullptr};
    std::free(x);
    std::vector<float> padded(size_t(EC_LEAD * o.rate), 0.0f);
    padded.insert(padded.end(), s.x.begin(), s.x.end());
    s.image = WavImage(padded.data(), int(padded.size()), o.rate);
    stims.push_back(std::move(s));
  }
  float* click;
  int nclick = ec_stimulus(EC_STIM_IMPULSE, o.rate, &click);
  audio::Bytes sync_image = WavImage(click, nclick, o.rate);
  std::free(click);

  int rendered = 0;
  for (const char* provider : kProviders) {
    char tag[32];
    ec_provider_tag(provider, tag, sizeof tag);
    if (!Selected(o.providers, tag, provider)) continue;
    int ver = ec_provider_eax(provider);
    fs::path dir = fs::path(o.out) / tag;
    fs::create_directories(dir);
    if (!g_quiet) std::printf("== %s (%s)\n", provider, tag);
    for (const Stim& s : stims) {
      unsigned size;
      unsigned char* w = ec_wav16(s.x.data(), int(s.x.size()), 1, o.rate, &size);
      WriteFile(dir / ("stim_" + std::string(ec_stim_name(s.bit)) + ".wav"), w, size);
      std::free(w);
    }
    for (int i = 0; i < ec_case_count(); i++) {
      const EcCase& c = *ec_case(i);
      if (!ec_case_applies(&c, ver) || !ec_match(o.cases.c_str(), c.name)) continue;
      unsigned stim = o.stimuli ? o.stimuli : c.stimuli;
      for (const Stim& s : stims) {
        if (!(stim & s.bit)) continue;
        Render(o, provider, tag, ver, c, s, sync_image, dir);
        rendered++;
      }
    }
  }
  if (!rendered) {
    std::fprintf(stderr, "error: nothing matched --provider/--cases\n");
    return 1;
  }
  if (!g_quiet) std::printf("%d recordings in %s\n", rendered, o.out.c_str());
  return 0;
}
