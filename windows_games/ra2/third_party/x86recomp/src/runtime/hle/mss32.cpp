// mss32 (Miles Sound System 6.x): the entry points games import,
// implemented on the host mixer (runtime/audio). 2D samples (from WAV
// files or headerless PCM / IMA ADPCM in memory, and double-buffered
// streaming), 3D samples, MP3/WAV streams, timers and format conversion.
// 3D providers: "Miles Fast 2D Positional Audio" (distance attenuation and
// panning) and Miles' EAX providers (Msseax.m3d: EAX 1.0, 2.0 and 3.0, with
// its attribute names and struct layouts), which position sounds as
// DirectSound3D hardware does and add the listener's environment and
// per-sample EAX levels (runtime/audio/eax.h). Miles 6.5's digital master reverb (room type,
// reverb and levels, per-sample/stream wet levels) is our compatible
// implementation (runtime/audio/digital_reverb.h).
//
// Miles 6.5 changed some exports under the same names (3D sample volume
// from S32 0-127 to F32 0-1, returned in st(0)), and how volume, pan and
// a 3D position become channel gains (Voice::miles; runtime/audio/
// miles65.h; docs/miles.md); the generation is read from the mss32.dll
// that ships with the game (6.1c or 6.5e).
//
// Callbacks (end of sample/stream, timers) run on a Miles service thread,
// as with the real library: a guest thread created at driver open that
// polls every few milliseconds. No lock is held while guest code runs, so
// a callback may call back into Miles and may wait on the game's own
// locks. A sequence number per sample drops end-of-sample events that a
// restart on the game thread has made stale.
//
// Handles are small zeroed guest blocks, so they are real, distinct guest
// pointers; the game never looks inside them.

#include "runtime/hle/mss32.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/audio/eax.h"
#include "runtime/audio/miles65.h"
#include "runtime/audio/mixer.h"
#include "runtime/audio/source.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using audio::Voice;
using Clock = std::chrono::steady_clock;

// Sample/stream status values (mss.h).
constexpr uint32_t kSmpFree = 1, kSmpDone = 2, kSmpPlaying = 4, kSmpStopped = 8;
constexpr uint32_t kM3dNoErr = 0, kM3dNotEnabled = 1;
// 3D providers; HPROVIDER is an opaque U32.
enum class Prov { kFast2D, kEax1, kEax2, kEax3 };
struct ProviderDef {
  uint32_t handle;
  const char* name;
  Prov kind;
};
constexpr ProviderDef kProviders[] = {
    {0x3D0001, "Miles Fast 2D Positional Audio", Prov::kFast2D},
    {0x3D0002, "Creative Labs EAX (TM)", Prov::kEax1},
    {0x3D0003, "Creative Labs EAX 2 (TM)", Prov::kEax2},
    {0x3D0004, "Creative Labs EAX 3 (TM)", Prov::kEax3},
};
constexpr int kProviderCount = int(std::size(kProviders));
// The reverb bus of the open EAX provider's environment.
constexpr int kBusEax = 0;

struct Sample {
  uint32_t handle = 0;
  bool is3d = false;
  Voice v;
  int bytes_per_frame = 0;  // of the loaded WAV
  int64_t length = 0;       // frames
  uint32_t eos_cb = 0;
  uint32_t seq = 0;   // bumped whenever an old end-of-sample event goes stale
  float vol = 1.0f, pan = 0.5f;  // Miles 6.5 levels (S32 APIs map to them)
  float dry = 1.0f, wet = 0.0f;  // digital master reverb levels
  Prov prov = Prov::kFast2D;     // 3D samples: the allocating provider
  audio::eax::Source eax;        // 3D samples on EAX providers
  bool start_pos_set = false;  // set_sample_position before start
  int64_t start_pos = 0;
  // Headerless data (set_sample_type/address/adpcm_block_size), decoded
  // when first needed.
  audio::RawFormat raw;
  std::vector<uint8_t> raw_data;
  bool raw_dirty = false;
  // Double-buffered streaming (load_sample_buffer): the queue the voice
  // plays, and where each loaded buffer ends in it.
  audio::QueueSource* queue = nullptr;
  std::vector<int64_t> pending_ends;
  int64_t retired_end = 0;  // queue frame where the oldest pending buffer starts
  int64_t queued = 0;
  int next_buffer = 0;
};

struct Stream {
  uint32_t handle = 0;
  Voice v;
  uint32_t cb = 0;
  uint32_t seq = 0;
  float vol = 1.0f, pan = 0.5f, dry = 1.0f, wet = 0.0f;
};

struct Timer {
  uint32_t cb = 0, user = 0;
  int64_t period_us = 0;
  bool running = false;
  Clock::time_point next;
};

struct State {
  // Guarded by the mixer lock (one lock for Miles state and voices).
  std::map<uint32_t, uint32_t> prefs;
  std::string last_error;
  uint32_t error_buf = 0;
  uint32_t redist_buf = 0;
  uint32_t file_open = 0, file_close = 0, file_read = 0;  // no seek needed
  uint32_t driver = 0;  // HDIGDRIVER while open
  int master = 127;
  uint32_t listener = 0;
  audio::Vec3 lpos, lface{0, 0, 1}, lup{0, 1, 0};
  std::map<uint32_t, std::unique_ptr<Sample>> samples;  // by handle
  std::map<uint32_t, std::unique_ptr<Stream>> streams;
  std::vector<uint32_t> free_blocks;  // guest handle blocks to reuse
  std::map<uint32_t, Timer> timers;   // by HTIMER
  uint32_t next_timer = 1;  // 0 reads as "no timer" to the game
  bool service_started = false;
  uint32_t provider_names[kProviderCount] = {};
  // EAX listener per EAX provider; the open one drives kBusEax.
  audio::eax::Listener eax[kProviderCount];
  Prov active = Prov::kFast2D;
  uint32_t seq = 0;  // source of Sample/Stream::seq, unique across reuse
  bool float_api = false;  // Miles 6.5 or later (see the file comment)
  float distance_factor = 1.0f;
};

State& S() {
  static State s;
  return s;
}
std::mutex& Mu() { return audio::GetMixer().mu(); }
uint32_t NextSeq() { return ++S().seq; }

// Miles preference defaults (mss.h, MSS 6).
void InitPrefs(State& s) {
  if (!s.prefs.empty()) return;
  s.prefs[0] = 131;   // DIG_RESAMPLING_TOLERANCE
  s.prefs[1] = 64;    // DIG_MIXER_CHANNELS
  s.prefs[2] = 127;   // DIG_DEFAULT_VOLUME
  s.prefs[35] = 64;   // DIG_DS_FRAGMENT_CNT
  s.prefs[42] = 8;    // DIG_DS_MIX_FRAGMENT_CNT
}

uint32_t NewBlock(State& s) {
  if (!s.free_blocks.empty()) {
    uint32_t b = s.free_blocks.back();
    s.free_blocks.pop_back();
    std::memset(GPtr(b), 0, 32);
    return b;
  }
  return HeapAllocProcess(32, true);
}

float F(uint32_t bits) { return std::bit_cast<float>(bits); }

Sample* FindSample(uint32_t h, bool is3d, const char* fn) {
  auto& m = S().samples;
  auto it = m.find(h);
  if (it == m.end() || it->second->is3d != is3d) {
    WarnF("mss32: %s: unknown %s handle %08X\n", fn, is3d ? "3D sample" : "sample", h);
    return nullptr;
  }
  return it->second.get();
}

Stream* FindStream(uint32_t h, const char* fn) {
  auto it = S().streams.find(h);
  if (it == S().streams.end()) {
    WarnF("mss32: %s: unknown stream handle %08X\n", fn, h);
    return nullptr;
  }
  return it->second.get();
}

uint32_t StatusOf(const Voice& v) {
  switch (v.state) {
    case Voice::State::Playing: return kSmpPlaying;
    case Voice::State::Paused: return kSmpStopped;
    default: return kSmpDone;
  }
}

bool IsEax(Prov p) { return p != Prov::kFast2D; }

// The provider behind an HPROVIDER, if we offer it.
const ProviderDef* ProviderOf(uint32_t h) {
  for (const ProviderDef& d : kProviders)
    if (d.handle == h) return &d;
  return nullptr;
}

// Whose mixer a sample's gains follow (Voice::miles): Miles 6.5's for
// 2D samples, streams and Fast 2D 3D samples of a 6.5 game. EAX voices
// keep the 6.1c rule: on Miles' EAX providers the sound card places
// them, not Miles' mixer.
Voice::Miles MilesOf(bool is3d, Prov p) {
  return S().float_api && (!is3d || p == Prov::kFast2D) ? Voice::Miles::k65
                                                         : Voice::Miles::k61;
}

// Reverb levels on the direct path and the master reverb send; the
// driver's dry level scales every sample's (Miles 6.5 voices: in the
// mixer, with the gains).
void SetReverbLevels(Voice& v, float dry, float wet) {
  if (v.miles == Voice::Miles::k65)
    v.dry = dry;
  else
    v.direct_lf = v.direct_hf = dry * audio::GetMixer().digital_reverb().dry();
  v.wet = wet;
}

// Miles 6.5 wakes its digital master reverb when a sample with a wet level
// starts, resumes or gets new levels while playing.
void WakeReverb(const Voice& v) {
  audio::Mixer& m = audio::GetMixer();
  m.digital_reverb().Wake(v.wet, m.now_ms());
}

// Volume and pan; 2D sounds also take their reverb levels here, 3D samples
// in ApplyEax.
void ApplyVolume(Sample& s) {
  s.v.volume = s.vol;
  s.v.pan = s.pan;
  if (!s.is3d) SetReverbLevels(s.v, s.dry, s.wet);
}
void ApplyVolume(Stream& s) {
  s.v.volume = s.vol;
  s.v.pan = s.pan;
  SetReverbLevels(s.v, s.dry, s.wet);
}

// A 3D sample's direct and reverb-send levels. On an EAX provider they
// come from its EAX properties, the listener's environment and the
// distance (so they are redone when either moves); on Fast 2D the effects
// level goes to the master reverb, as Miles 6.5's digital reverb takes
// every sample of the driver (earlier Miles has no such reverb).
void ApplyEax(Sample& s) {
  State& st = S();
  Voice& v = s.v;
  if (!IsEax(s.prov)) {
    SetReverbLevels(v, 1.0f, st.float_api ? s.wet : 0.0f);
    return;
  }
  audio::eax::Listener l = st.eax[int(s.prov)];
  if (s.prov == Prov::kEax1) l.air_absorption_hf = 0;  // EAX 1.0 has none
  float dx = v.pos.x - st.lpos.x, dy = v.pos.y - st.lpos.y, dz = v.pos.z - st.lpos.z;
  float d = std::sqrt(dx * dx + dy * dy + dz * dz) * st.distance_factor;
  audio::eax::Levels lv =
      audio::eax::SourceLevels(s.eax, l, d, v.min_dist * st.distance_factor, int(s.prov));
  bool on = s.prov == st.active;
  v.direct_lf = lv.direct_lf;
  v.direct_hf = lv.direct_hf;
  v.send_lf[kBusEax] = on ? lv.room_lf : 0.0f;
  v.send_hf[kBusEax] = on ? lv.room_hf : 0.0f;
  v.wet = 0.0f;
}

void ApplyEaxAll() {
  for (auto& [h, s] : S().samples)
    if (s->is3d) ApplyEax(*s);
}

// The open EAX provider's listener on its reverb bus.
void ApplyEaxListener() {
  State& st = S();
  if (IsEax(st.active))
    audio::GetMixer().SetReverb(kBusEax,
                                audio::eax::ToReverb(st.eax[int(st.active)], int(st.active)));
  ApplyEaxAll();
}

// Puts a sample back to its just-initialised state.
void ResetSample(Sample& s) {
  s.v.miles = MilesOf(s.is3d, s.prov);
  s.v.state = Voice::State::Idle;
  s.v.eos = false;
  s.v.loops = 1;
  s.v.rate = 0;
  s.vol = 1.0f;
  s.pan = 0.5f;
  s.dry = 1.0f;
  s.wet = 0.0f;
  ApplyVolume(s);
  s.raw = {};
  s.raw_data.clear();
  s.raw_dirty = false;
  s.queue = nullptr;
  s.pending_ends.clear();
  s.retired_end = 0;
  s.queued = 0;
  s.next_buffer = 0;
  s.eos_cb = 0;
  s.seq = NextSeq();
  s.start_pos_set = false;
  s.start_pos = 0;
  if (s.is3d) {
    s.v.positional = true;
    s.v.ds3d = IsEax(s.prov);  // the EAX providers are DirectSound3D ones
    s.v.min_dist = 1.0f;
    if (s.v.miles == Voice::Miles::k65) {
      s.v.max_dist = 200.0f;  // msssoft.m3d's; it keeps the position
    } else {
      s.v.pos = {};
      s.v.max_dist = 1e30f;
    }
    ApplyEax(s);
  }
}

// Starts a sample's or stream's voice from frame `from`; `seq` is its
// end-of-sample sequence number.
void Start(Voice& v, uint32_t* seq, int64_t from) {
  v.Seek(from);
  v.passes = 0;
  v.eos = false;
  v.gains_valid_ = false;
  v.state = Voice::State::Playing;
  *seq = NextSeq();
  audio::GetMixer().Add(&v);
  WakeReverb(v);
}

// Builds the voice's source from headerless data once its format is known.
void EnsureSource(Sample& s) {
  if (!s.raw_dirty) return;
  s.raw_dirty = false;
  s.raw.rate = s.v.rate > 0 ? int(s.v.rate) : 22050;
  std::string err;
  s.v.src = audio::MakeRawSource(s.raw_data.data(), s.raw_data.size(), s.raw, &err);
  if (!s.v.src) {
    WarnF("mss32: sample %08X: %s\n", s.handle, err.c_str());
    return;
  }
  s.v.Seek(0);
  s.bytes_per_frame = s.raw.adpcm ? 0 : s.raw.channels * s.raw.bits / 8;
  s.length = s.v.src->Length();
}

void Play(Sample& s, int64_t from) {
  EnsureSource(s);
  if (!s.v.src) return;
  s.start_pos_set = false;
  Start(s.v, &s.seq, from);
}

// ---- service thread ------------------------------------------------------------

struct Event {
  uint32_t cb, arg;
  // For end-of-sample events: deliver only if still current.
  Sample* sample = nullptr;
  Stream* stream = nullptr;
  uint32_t seq = 0;
};

void ServiceLoop(Thread* t) {
  std::vector<Event> events;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    events.clear();
    {
      std::lock_guard<std::mutex> l(Mu());
      State& st = S();
      if (!st.driver) continue;
      auto now = Clock::now();
      for (auto& [h, tm] : st.timers) {
        if (!tm.running || !tm.cb || tm.period_us <= 0) continue;
        if (now < tm.next) continue;
        events.push_back({tm.cb, tm.user});
        tm.next += std::chrono::microseconds(tm.period_us);
        if (tm.next < now) tm.next = now;  // never burst to catch up
      }
      for (auto& [h, s] : st.samples) {
        if (!s->v.eos) continue;
        s->v.eos = false;
        if (s->eos_cb) events.push_back({s->eos_cb, h, s.get(), nullptr, s->seq});
      }
      for (auto& [h, s] : st.streams) {
        if (!s->v.eos) continue;
        s->v.eos = false;
        if (s->cb) events.push_back({s->cb, h, nullptr, s.get(), s->seq});
      }
    }
    for (const Event& e : events) {
      if (e.sample || e.stream) {
        std::lock_guard<std::mutex> l(Mu());
        // The game may have restarted or released it meanwhile.
        // Look up by handle first: e.sample may have been freed.
        if (e.sample) {
          auto it = S().samples.find(e.arg);
          if (it == S().samples.end() || it->second->seq != e.seq ||
              it->second->v.state != Voice::State::Done)
            continue;
        }
        if (e.stream) {
          auto it = S().streams.find(e.arg);
          if (it == S().streams.end() || it->second->seq != e.seq) continue;
        }
      }
      CallGuest(t->ctx, e.cb, {e.arg});
    }
  }
}

void StartService() {
  State& st = S();
  if (st.service_started) return;
  st.service_started = true;
  Thread* t = CreateGuestThread(0x100000);
  StartServiceThread(t, [t] { ServiceLoop(t); });
}

void SetError(const std::string& e) {
  std::lock_guard<std::mutex> l(Mu());
  S().last_error = e;
}

// Copies a WAV file image out of guest memory (paging it in through the
// game's handlers if needed) and makes a source from it.
std::unique_ptr<audio::Source> LoadImage(Ctx& c, uint32_t image, int* bpf,
                                         int64_t* len, const char* fn) {
  if (!image || !EnsureReadable(c, image, 12)) {
    WarnF("mss32: %s: unreadable file image %08X\n", fn, image);
    return nullptr;
  }
  uint32_t size = audio::RiffSize(GPtr(image));
  if (!size) {
    WarnF("mss32: %s: file image %08X is not a RIFF WAVE\n", fn, image);
    return nullptr;
  }
  if (!EnsureReadable(c, image, size)) {
    WarnF("mss32: %s: cannot page in %u bytes at %08X\n", fn, size, image);
    return nullptr;
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>(GPtr(image), GPtr(image) + size);
  std::string err;
  auto src = audio::MakeWavSource(bytes, &err);
  if (!src) {
    WarnF("mss32: %s: %s\n", fn, err.c_str());
    return nullptr;
  }
  *bpf = src->bytes_per_frame();
  *len = src->Length();
  return src;
}

// The format of the WAV file image at `data`; false if it is not one.
bool WavInfoOf(Ctx& c, uint32_t data, audio::WavInfo* w) {
  if (!data || !EnsureReadable(c, data, 12)) return false;
  uint32_t size = audio::RiffSize(GPtr(data));
  if (!size || !EnsureReadable(c, data, size)) return false;
  return audio::ParseWav(GPtr(data), size, w);
}

uint32_t SetFile(Ctx& c, uint32_t h, uint32_t image, bool is3d, const char* fn) {
  int bpf = 0;
  int64_t len = 0;
  auto src = LoadImage(c, image, &bpf, &len, fn);
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(h, is3d, fn);
  if (!s) return 0;
  s->v.state = Voice::State::Idle;
  s->v.eos = false;
  s->seq = NextSeq();
  s->v.src = std::move(src);
  s->v.Seek(0);
  s->v.rate = 0;
  s->bytes_per_frame = bpf;
  s->length = len;
  s->start_pos_set = false;
  return s->v.src ? 1 : 0;
}

// ---- library / driver ---------------------------------------------------------------

// "Version 6.5e" in the game's own mss32.dll: 6.5 or later uses the F32
// API variants.
bool GameMilesIsFloatApi() {
  std::string path = HostPath(GuestGameDir() + "\\mss32.dll");
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::vector<char> b(1 << 20);
  size_t n = std::fread(b.data(), 1, b.size(), f);
  std::fclose(f);
  std::string s(b.data(), n);
  size_t at = s.find("Version 6.");
  if (at == std::string::npos) return false;
  int minor = std::atoi(s.c_str() + at + 10);
  RECOMP_TRACE("  mss32: game ships Miles 6.%d\n", minor);
  return minor >= 5;
}

HLE_NAMED("mss32.dll", "_AIL_startup@0", AIL_startup, 0) {
  bool fl = GameMilesIsFloatApi();
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  S().float_api = fl;
  return 1;
}

void CloseDriver() {
  State& st = S();
  audio::Mixer& mx = audio::GetMixer();
  for (auto& [h, s] : st.samples) mx.Remove(&s->v);
  for (auto& [h, s] : st.streams) mx.Remove(&s->v);
  for (auto& [h, s] : st.samples) st.free_blocks.push_back(h);
  for (auto& [h, s] : st.streams) st.free_blocks.push_back(h);
  st.samples.clear();
  st.streams.clear();
  st.timers.clear();
  if (st.driver) st.free_blocks.push_back(st.driver);
  if (st.listener) st.free_blocks.push_back(st.listener);
  st.driver = st.listener = 0;
}

HLE_NAMED("mss32.dll", "_AIL_shutdown@0", AIL_shutdown, 0) {
  {
    std::lock_guard<std::mutex> l(Mu());
    CloseDriver();
  }
  audio::GetMixer().Close();  // waits for the audio thread; not under Mu
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_redist_directory@4", AIL_set_redist_directory, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.redist_buf) {
    st.redist_buf = HeapAllocProcess(260, true);
    PutStr(st.redist_buf, ".");
  }
  return st.redist_buf;  // previous directory
}

HLE_NAMED("mss32.dll", "_AIL_set_file_callbacks@16", AIL_set_file_callbacks, 4) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.file_open = Arg(c, 0);
  st.file_close = Arg(c, 1);
  st.file_read = Arg(c, 3);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_preference@8", AIL_set_preference, 2) {
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  uint32_t& p = S().prefs[Arg(c, 0)];
  uint32_t old = p;
  p = Arg(c, 1);
  return old;
}

HLE_NAMED("mss32.dll", "_AIL_get_preference@4", AIL_get_preference, 1) {
  std::lock_guard<std::mutex> l(Mu());
  InitPrefs(S());
  return S().prefs[Arg(c, 0)];
}

HLE_NAMED("mss32.dll", "_AIL_last_error@0", AIL_last_error, 0) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.error_buf) st.error_buf = HeapAllocProcess(256, true);
  PutStr(st.error_buf, st.last_error.substr(0, 255));
  return st.error_buf;
}

// AIL_open_digital_driver(frequency, bits, channels, flags) -> HDIGDRIVER.
HLE_NAMED("mss32.dll", "_AIL_open_digital_driver@16", AIL_open_digital_driver, 4) {
  uint32_t rate = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(Mu());
    if (S().driver) return S().driver;
  }
  if (!audio::GetMixer().Open(int(rate ? rate : 44100))) {
    SetError("No digital audio device available");
    return 0;
  }
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.driver = NewBlock(st);
  audio::GetMixer().set_master(st.master / 127.0f);
  StartService();
  return st.driver;
}

HLE_NAMED("mss32.dll", "_AIL_set_digital_master_volume@8", AIL_set_digital_master_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  st.master = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127);
  audio::GetMixer().set_master(st.master / 127.0f);
  return 0;
}

// ---- 2D samples ---------------------------------------------------------------------

HLE_NAMED("mss32.dll", "_AIL_allocate_sample_handle@4", AIL_allocate_sample_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!st.driver || Arg(c, 0) != st.driver) return 0;
  auto s = std::make_unique<Sample>();
  s->handle = NewBlock(st);
  ResetSample(*s);
  uint32_t h = s->handle;
  st.samples[h] = std::move(s);
  return h;
}

void Release(uint32_t h, bool is3d, const char* fn) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(h, is3d, fn);
  if (!s) return;
  audio::GetMixer().Remove(&s->v);
  S().samples.erase(h);
  S().free_blocks.push_back(h);
}

HLE_NAMED("mss32.dll", "_AIL_release_sample_handle@4", AIL_release_sample_handle, 1) {
  Release(Arg(c, 0), false, "AIL_release_sample_handle");
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_init_sample@4", AIL_init_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_init_sample")) {
    audio::GetMixer().Remove(&s->v);
    s->v.src.reset();
    ResetSample(*s);
  }
  return 0;
}

// AIL_set_sample_file(S, file_image, block) -> nonzero on success.
HLE_NAMED("mss32.dll", "_AIL_set_sample_file@12", AIL_set_sample_file, 3) {
  return SetFile(c, Arg(c, 0), Arg(c, 1), false, "AIL_set_sample_file");
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_loop_count@8", AIL_set_sample_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_volume@8", AIL_set_sample_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_volume")) {
    s->vol = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127) / 127.0f;
    ApplyVolume(*s);
  }
  return 0;
}

// Miles 6.5: AIL_set_sample_volume_pan(S, F32 volume 0..1, F32 pan 0..1),
// not clamped (runtime/audio/miles65.h).
HLE_NAMED("mss32.dll", "_AIL_set_sample_volume_pan@12", AIL_set_sample_volume_pan, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_volume_pan")) {
    s->vol = F(Arg(c, 1));
    s->pan = F(Arg(c, 2));
    ApplyVolume(*s);
  }
  return 0;
}

// Miles 6.5 reports the volume back through its level fields.
float ReportedVolume(float vol, float pan) {
  return audio::miles65::ReportedVolume(audio::miles65::FromVolumePan(vol, pan));
}

HLE_NAMED("mss32.dll", "_AIL_sample_volume_pan@12", AIL_sample_volume_pan, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_volume_pan");
  if (Arg(c, 1))
    Wr32(Arg(c, 1), std::bit_cast<uint32_t>(s ? ReportedVolume(s->vol, s->pan) : 0.0f));
  if (Arg(c, 2)) Wr32(Arg(c, 2), std::bit_cast<uint32_t>(s ? s->pan : 0.5f));
  return 0;
}

// AIL_set_sample_reverb_levels(S, F32 dry, F32 wet): the direct level and
// the send to the digital master reverb.
HLE_NAMED("mss32.dll", "_AIL_set_sample_reverb_levels@12", AIL_set_sample_reverb_levels, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_reverb_levels")) {
    s->dry = std::clamp(F(Arg(c, 1)), 0.0f, 1.0f);
    s->wet = std::clamp(F(Arg(c, 2)), 0.0f, 1.0f);
    ApplyVolume(*s);
    if (s->v.state == Voice::State::Playing) WakeReverb(s->v);
  }
  return 0;
}

// ---- headerless sample data (Miles 6.5 style) -----------------------------------------

// AIL_set_sample_type(S, format, flags): DIG_F_* (bit 0 16-bit, bit 1
// stereo, bit 2 IMA ADPCM), DIG_PCM_SIGN (1) for signed 8-bit.
HLE_NAMED("mss32.dll", "_AIL_set_sample_type@12", AIL_set_sample_type, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_type");
  if (!s) return 0;
  uint32_t fmt = Arg(c, 1), flags = Arg(c, 2);
  s->raw.channels = fmt & 2 ? 2 : 1;
  s->raw.adpcm = fmt & 4;
  s->raw.bits = s->raw.adpcm ? 4 : fmt & 1 ? 16 : 8;
  s->raw.pcm_signed = flags & 1;
  if (flags & 2) WarnF("mss32: DIG_PCM_ORDER (big-endian samples) not supported\n");
  s->raw_dirty = true;
  return 0;
}

// AIL_set_sample_address(S, void const* start, U32 len): played from
// memory on Windows; copied here (the mixer never reads guest memory).
HLE_NAMED("mss32.dll", "_AIL_set_sample_address@12", AIL_set_sample_address, 3) {
  uint32_t ptr = Arg(c, 1), len = Arg(c, 2);
  if (len && !EnsureReadable(c, ptr, len)) {
    WarnF("mss32: AIL_set_sample_address: unreadable data %08X+%u\n", ptr, len);
    return 0;
  }
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_address");
  if (!s) return 0;
  audio::GetMixer().Remove(&s->v);
  s->v.state = Voice::State::Idle;
  s->v.src.reset();
  s->queue = nullptr;
  s->raw_data.assign(GPtr(ptr), GPtr(ptr) + len);
  s->raw_dirty = true;
  s->start_pos_set = false;
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_adpcm_block_size@8", AIL_set_sample_adpcm_block_size, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_adpcm_block_size")) {
    s->raw.block_size = Arg(c, 1);
    s->raw_dirty = true;
  }
  return 0;
}

// ---- double-buffered streaming ------------------------------------------------------

constexpr int kBytesPerFrame[8] = {1, 2, 2, 4, 1, 1, 1, 1};

// AIL_minimum_sample_buffer_size(dig, rate, format): bytes for 100 ms.
HLE_NAMED("mss32.dll", "_AIL_minimum_sample_buffer_size@12", AIL_minimum_sample_buffer_size, 3) {
  int32_t rate = int32_t(Arg(c, 1));
  return uint32_t(std::max(rate, 1) / 10 * kBytesPerFrame[Arg(c, 2) & 3]);
}

// Retires loaded buffers the mixer has consumed.
void RetireBuffers(Sample& s) {
  if (!s.queue) return;
  int64_t done = s.queue->Consumed();
  while (!s.pending_ends.empty() && s.pending_ends.front() <= done) {
    s.retired_end = s.pending_ends.front();
    s.pending_ends.erase(s.pending_ends.begin());
  }
}

// AIL_sample_buffer_ready(S): which of the two buffers to fill next, or -1.
HLE_NAMED("mss32.dll", "_AIL_sample_buffer_ready@4", AIL_sample_buffer_ready, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_buffer_ready");
  if (!s) return 0xFFFFFFFF;
  RetireBuffers(*s);
  return s->pending_ends.size() < 2 ? uint32_t(s->next_buffer) : 0xFFFFFFFF;
}

// AIL_load_sample_buffer(S, buff_num, void const* buffer, U32 len): queues
// PCM in the sample's format and starts it if it is not playing.
HLE_NAMED("mss32.dll", "_AIL_load_sample_buffer@16", AIL_load_sample_buffer, 4) {
  uint32_t ptr = Arg(c, 2), len = Arg(c, 3);
  if (len && !EnsureReadable(c, ptr, len)) return 0;
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_load_sample_buffer");
  if (!s) return 0;
  if (!s->queue) {
    audio::GetMixer().Remove(&s->v);
    int rate = s->v.rate > 0 ? int(s->v.rate) : 22050;
    auto q = std::make_unique<audio::QueueSource>(s->raw.channels, rate);
    s->queue = q.get();
    s->v.src = std::move(q);
    s->raw_dirty = false;
    s->pending_ends.clear();
    s->retired_end = 0;
    s->queued = 0;
  }
  std::vector<float> frames;
  std::string err;
  if (len && !audio::DecodeRaw(GPtr(ptr), len, s->raw, &frames, &err)) {
    WarnF("mss32: AIL_load_sample_buffer: %s\n", err.c_str());
    return 0;
  }
  int n = int(frames.size()) / s->raw.channels;
  s->queue->Push(frames.data(), n);
  s->queued += n;
  s->pending_ends.push_back(s->queued);
  s->next_buffer = (int(Arg(c, 1)) + 1) & 1;
  if (s->v.state != Voice::State::Playing) Start(s->v, &s->seq, 0);
  return 0;
}

uint32_t RateOf(const Sample& s) {
  if (s.v.rate > 0) return uint32_t(std::lround(s.v.rate));
  return s.v.src ? uint32_t(s.v.src->rate()) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_playback_rate@4", AIL_sample_playback_rate, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_playback_rate");
  return s ? RateOf(*s) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_playback_rate@8", AIL_set_sample_playback_rate, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_playback_rate"))
    s->v.rate = float(int32_t(Arg(c, 1)));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_EOS_callback@8", AIL_register_EOS_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_register_EOS_callback");
  if (!s) return 0;
  uint32_t old = s->eos_cb;
  s->eos_cb = Arg(c, 1);
  return old;
}

void SetPosition(Sample& s, uint32_t bytes) {
  int64_t f = s.bytes_per_frame ? bytes / s.bytes_per_frame : 0;
  if (s.v.state == Voice::State::Playing || s.v.state == Voice::State::Paused) {
    s.v.Seek(f);
  } else {
    s.start_pos_set = true;
    s.start_pos = f;
  }
}

uint32_t PositionOf(Sample& s) {
  // Double-buffered: the offset into the buffer playing now, 0 once all
  // have played. Games add it to the bytes they have loaded to clock
  // cinematics.
  if (s.queue) {
    RetireBuffers(s);
    if (s.pending_ends.empty() || s.raw.adpcm) return 0;
    int64_t f = std::max<int64_t>(s.queue->Consumed() - s.retired_end, 0);
    return uint32_t(f * s.raw.channels * s.raw.bits / 8);
  }
  int64_t f = s.start_pos_set ? s.start_pos : s.v.Position();
  if (s.v.state == Voice::State::Done) f = s.length;
  return uint32_t(f * s.bytes_per_frame);
}

HLE_NAMED("mss32.dll", "_AIL_set_sample_position@8", AIL_set_sample_position, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_position"))
    SetPosition(*s, Arg(c, 1));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_position@4", AIL_sample_position, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_position");
  return s ? PositionOf(*s) : 0;
}

// AIL_sample_ms_position(S, S32* total_ms, S32* current_ms).
HLE_NAMED("mss32.dll", "_AIL_sample_ms_position@12", AIL_sample_ms_position, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_sample_ms_position");
  if (s) EnsureSource(*s);
  uint32_t total = 0, cur = 0;
  if (s && s->v.src && s->v.src->rate()) {
    double r = s->v.src->rate();
    int64_t f = s->v.state == Voice::State::Done ? s->length : s->v.Position();
    total = uint32_t(s->length * 1000.0 / r);
    cur = uint32_t(f * 1000.0 / r);
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), total);
  if (Arg(c, 2)) Wr32(Arg(c, 2), cur);
  return 0;
}

// AIL_set_sample_ms_position(S, S32 ms).
HLE_NAMED("mss32.dll", "_AIL_set_sample_ms_position@8", AIL_set_sample_ms_position, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), false, "AIL_set_sample_ms_position");
  if (!s) return 0;
  EnsureSource(*s);
  if (!s->v.src) return 0;
  int64_t f = int64_t(int32_t(Arg(c, 1))) * s->v.src->rate() / 1000;
  if (s->v.state == Voice::State::Playing || s->v.state == Voice::State::Paused) {
    s->v.Seek(f);
  } else {
    s->start_pos_set = true;
    s->start_pos = f;
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_sample@4", AIL_start_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_start_sample"))
    Play(*s, s->start_pos_set ? s->start_pos : 0);
  return 0;
}

void Stop(Sample& s) {
  if (s.v.state == Voice::State::Playing) s.v.state = Voice::State::Paused;
}

void Resume(Sample& s) {
  if (s.v.state == Voice::State::Paused) {
    s.v.state = Voice::State::Playing;
    WakeReverb(s.v);
  } else if (s.v.state != Voice::State::Playing) {
    // Resuming a sample that was never started plays it from the offset
    // set with set_sample_position / set_3D_sample_offset.
    Play(s, s.start_pos_set ? s.start_pos : s.v.Position());
  }
}

void End(Sample& s) {
  if (s.v.state == Voice::State::Playing || s.v.state == Voice::State::Paused)
    s.v.state = Voice::State::Done;
  s.v.eos = false;  // ended by request: no callback
  s.seq = NextSeq();
}

HLE_NAMED("mss32.dll", "_AIL_stop_sample@4", AIL_stop_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_stop_sample")) Stop(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_resume_sample@4", AIL_resume_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_resume_sample")) Resume(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_end_sample@4", AIL_end_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), false, "AIL_end_sample")) End(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_sample_status@4", AIL_sample_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().samples.find(Arg(c, 0));
  if (it == S().samples.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// ---- 3D provider, listener and samples ---------------------------------------------

// AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name):
// *next is the index of the next provider to report.
HLE_NAMED("mss32.dll", "_AIL_enumerate_3D_providers@12", AIL_enumerate_3D_providers, 3) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  for (uint32_t i = Rd32(Arg(c, 0)); i < uint32_t(kProviderCount); i++) {
    const ProviderDef& d = kProviders[i];
    if (!ProviderOf(d.handle)) continue;
    if (!st.provider_names[i]) {
      st.provider_names[i] = HeapAllocProcess(64, true);
      PutStr(st.provider_names[i], d.name);
    }
    Wr32(Arg(c, 0), i + 1);
    Wr32(Arg(c, 1), d.handle);
    Wr32(Arg(c, 2), st.provider_names[i]);
    return 1;
  }
  return 0;
}

// Opening an EAX provider puts its environment on the EAX reverb bus.
HLE_NAMED("mss32.dll", "_AIL_open_3D_provider@4", AIL_open_3D_provider, 1) {
  std::lock_guard<std::mutex> l(Mu());
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (!d) return kM3dNotEnabled;
  if (IsEax(d->kind)) {
    S().active = d->kind;
    ApplyEaxListener();
  }
  return kM3dNoErr;
}

HLE_NAMED("mss32.dll", "_AIL_close_3D_provider@4", AIL_close_3D_provider, 1) {
  std::lock_guard<std::mutex> l(Mu());
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (d && d->kind == S().active) {
    S().active = Prov::kFast2D;
    ApplyEaxAll();
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_open_3D_listener@4", AIL_open_3D_listener, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (!ProviderOf(Arg(c, 0)) || !st.driver) return 0;
  if (!st.listener) st.listener = NewBlock(st);
  return st.listener;
}

HLE_NAMED("mss32.dll", "_AIL_close_3D_listener@4", AIL_close_3D_listener, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) == st.listener && st.listener) {
    st.free_blocks.push_back(st.listener);
    st.listener = 0;
  }
  return 0;
}

// AIL_set_3D_room_type(lib, room): an EAX provider's environment preset
// (EAX environment numbers). Fast 2D has no environment.
HLE_NAMED("mss32.dll", "_AIL_set_3D_room_type@8", AIL_set_3D_room_type, 2) {
  std::lock_guard<std::mutex> l(Mu());
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (d && IsEax(d->kind)) {
    S().eax[int(d->kind)] = audio::eax::Preset(Arg(c, 1));
    ApplyEaxListener();
  }
  return 0;
}

// Miles 6.5's Fast 2D (msssoft.m3d) takes only mono PCM; it refuses other
// data before touching the sample.
bool Fast2DRefuses(const Sample& s, uint32_t format, uint32_t channels) {
  if (s.v.miles != Voice::Miles::k65) return false;
  if (format != 1)
    SetError("Not linear PCM data");
  else if (channels != 1)
    SetError("Not mono data");
  else
    return false;
  return true;
}

HLE_NAMED("mss32.dll", "_AIL_allocate_3D_sample_handle@4", AIL_allocate_3D_sample_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (!d || !st.driver) return 0;
  auto s = std::make_unique<Sample>();
  s->handle = NewBlock(st);
  s->is3d = true;
  s->prov = d->kind;
  if (d->kind == Prov::kEax2) s->eax.occlusion_room_ratio = audio::eax::kOcclusionRoomRatio2;
  ResetSample(*s);
  uint32_t h = s->handle;
  st.samples[h] = std::move(s);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_release_3D_sample_handle@4", AIL_release_3D_sample_handle, 1) {
  Release(Arg(c, 0), true, "AIL_release_3D_sample_handle");
  return 0;
}

// AIL_set_3D_sample_file(S, file_image) -> nonzero on success. Like
// AIL_set_sample_file it also resets the sample.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_file@8", AIL_set_3D_sample_file, 2) {
  audio::WavInfo w;
  bool wav = WavInfoOf(c, Arg(c, 1), &w);
  {
    std::lock_guard<std::mutex> l(Mu());
    Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_file");
    if (!s) return 0;
    if (wav && Fast2DRefuses(*s, w.format, w.channels)) return 0;
    audio::GetMixer().Remove(&s->v);
    s->v.src.reset();
    uint32_t keep_cb = s->eos_cb;
    ResetSample(*s);
    s->eos_cb = keep_cb;
  }
  return SetFile(c, Arg(c, 0), Arg(c, 1), true, "AIL_set_3D_sample_file");
}

HLE_NAMED("mss32.dll", "_AIL_start_3D_sample@4", AIL_start_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_start_3D_sample")) Play(*s, 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_resume_3D_sample@4", AIL_resume_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_resume_3D_sample")) Resume(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stop_3D_sample@4", AIL_stop_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_stop_3D_sample")) Stop(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_end_3D_sample@4", AIL_end_3D_sample, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_end_3D_sample")) End(*s);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_offset@8", AIL_set_3D_sample_offset, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_offset"))
    SetPosition(*s, Arg(c, 1));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_offset@4", AIL_3D_sample_offset, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_offset");
  return s ? PositionOf(*s) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_length@4", AIL_3D_sample_length, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_length");
  return s ? uint32_t(s->length * s->bytes_per_frame) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_3D_EOS_callback@8", AIL_register_3D_EOS_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_register_3D_EOS_callback");
  if (!s) return 0;
  uint32_t old = s->eos_cb;
  s->eos_cb = Arg(c, 1);
  return old;
}

// AIL_set_3D_sample_effects_level(S, F32 level): the reverb send, 0..1;
// negative is EAX's default (full). Fast 2D (Miles 6.5's master reverb
// send) takes the magnitude.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_effects_level@8", AIL_set_3D_sample_effects_level, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_effects_level")) {
    float level = F(Arg(c, 1));
    s->eax.reverb_mix = level < 0 ? -1.0f : std::min(level, 1.0f);
    s->wet = std::fabs(level);
    ApplyEax(*s);
    if (s->v.state == Voice::State::Playing) WakeReverb(s->v);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_playback_rate@8", AIL_set_3D_sample_playback_rate, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_playback_rate"))
    s->v.rate = float(int32_t(Arg(c, 1)));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_playback_rate@4", AIL_3D_sample_playback_rate, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_playback_rate");
  return s ? RateOf(*s) : 0;
}

// AIL_set_3D_sample_distances(S, F32 max_dist, F32 min_dist).
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_distances@12", AIL_set_3D_sample_distances, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_distances")) {
    // Mss32 swaps the pair so the larger is max (the game passes min first).
    float a = F(Arg(c, 1)), b = F(Arg(c, 2));
    s->v.max_dist = std::max(a, b);
    s->v.min_dist = std::min(a, b);
    ApplyEax(*s);
  }
  return 0;
}

// S32 0..127 before Miles 6.5, F32 0..1 from 6.5 on.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_volume@8", AIL_set_3D_sample_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_volume")) {
    s->vol = S().float_api ? F(Arg(c, 1))  // msssoft.m3d keeps it as given
                           : std::clamp<int>(int32_t(Arg(c, 1)), 0, 127) / 127.0f;
    ApplyVolume(*s);
  }
  return 0;
}

// S32 in eax before Miles 6.5; from 6.5 on an F32, returned in st(0).
HLE_NAMED("mss32.dll", "_AIL_3D_sample_volume@4", AIL_3D_sample_volume, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_3D_sample_volume");
  float v = s ? s->vol : 0.0f;
  if (S().float_api) {
    x87::LoadF32(c.fpu, std::bit_cast<uint32_t>(v));
    return 0;
  }
  return uint32_t(std::lround(v * 127));
}

// AIL_set_3D_sample_info(S, AILSOUNDINFO const*): the sample plays the
// described data from memory (copied here). Like set_3D_sample_file it
// resets the sample.
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_info@8", AIL_set_3D_sample_info, 2) {
  uint32_t info = Arg(c, 1);
  uint32_t format = Rd32(info), data = Rd32(info + 4), len = Rd32(info + 8);
  if (len && !EnsureReadable(c, data, len)) return 0;
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_info");
  if (!s) return 0;
  if (Fast2DRefuses(*s, format, Rd32(info + 20))) return 0;
  audio::GetMixer().Remove(&s->v);
  s->v.src.reset();
  uint32_t keep_cb = s->eos_cb;
  ResetSample(*s);
  s->eos_cb = keep_cb;
  if (format != 1 && format != 0x11) {
    WarnF("mss32: AIL_set_3D_sample_info: wave format %u not supported\n", format);
    return 0;
  }
  s->raw.adpcm = format == 0x11;
  s->raw.rate = int(Rd32(info + 12));
  s->raw.bits = int(Rd32(info + 16));
  s->raw.channels = int(Rd32(info + 20));
  s->raw.block_size = Rd32(info + 28);
  s->raw_data.assign(GPtr(data), GPtr(data) + len);
  std::string err;
  s->v.src = audio::MakeRawSource(s->raw_data.data(), len, s->raw, &err);
  if (!s->v.src) {
    WarnF("mss32: AIL_set_3D_sample_info: %s\n", err.c_str());
    return 0;
  }
  s->bytes_per_frame = s->raw.adpcm ? 0 : s->raw.channels * s->raw.bits / 8;
  s->length = s->v.src->Length();
  return 1;
}

// AIL_3D_position(obj, F32* x, F32* y, F32* z): the listener or a sample.
HLE_NAMED("mss32.dll", "_AIL_3D_position@16", AIL_3D_position, 4) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  audio::Vec3 p{};
  uint32_t obj = Arg(c, 0);
  if (obj && obj == st.listener) {
    p = st.lpos;
  } else if (Sample* s = FindSample(obj, true, "AIL_3D_position")) {
    p = s->v.pos;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), std::bit_cast<uint32_t>(p.x));
  if (Arg(c, 2)) Wr32(Arg(c, 2), std::bit_cast<uint32_t>(p.y));
  if (Arg(c, 3)) Wr32(Arg(c, 3), std::bit_cast<uint32_t>(p.z));
  return 0;
}

// Distances are in game units; the factor converts them to metres for
// EAX (air absorption, room rolloff). Doppler is not modelled.
HLE_NAMED("mss32.dll", "_AIL_set_3D_distance_factor@8", AIL_set_3D_distance_factor, 2) {
  std::lock_guard<std::mutex> l(Mu());
  S().distance_factor = F(Arg(c, 1));
  ApplyEaxAll();
  return 0;
}

// ---- EAX attributes ---------------------------------------------------------------------
//
// Msseax.m3d's attribute names; the single properties are in
// runtime/audio/eax.h. "EAX all parameters" and "EAX sample all
// parameters" take the whole property set in the provider's EAX version's
// layout (a 3.0 listener for the EAX 3 provider).

// Listener attribute `name` of EAX provider p: reads it into, or sets it
// from, guest memory at `guest`. False if p has no such attribute.
bool ListenerAttribute(Prov p, const std::string& name, uint32_t guest, bool set) {
  audio::eax::Listener& l = S().eax[int(p)];
  if (name == "EAX all parameters") {
    uint8_t* q = GPtr(guest);
    if (p == Prov::kEax1) {
      set ? audio::eax::ReadListener1(q, &l) : audio::eax::WriteListener1(l, q);
    } else if (p == Prov::kEax2) {
      set ? audio::eax::ReadListener2(q, &l) : audio::eax::WriteListener2(l, q);
    } else {
      set ? audio::eax::ReadListener3(q, &l) : audio::eax::WriteListener3(l, q);
    }
    return true;
  }
  return audio::eax::ListenerAttribute(&l, name, int(p), GPtr(guest), set);
}

// The same for a sample's EAX source properties.
bool SampleAttribute(Sample& s, const std::string& name, uint32_t guest, bool set) {
  if (name == "EAX sample all parameters") {
    uint8_t* q = GPtr(guest);
    if (s.prov == Prov::kEax1) {
      // EAXBUFFER_REVERBPROPERTIES: the reverb mix.
      if (set)
        s.eax.reverb_mix = F(Rd32(guest));
      else
        Wr32(guest, std::bit_cast<uint32_t>(s.eax.reverb_mix));
    } else if (s.prov == Prov::kEax2) {
      set ? audio::eax::ReadSource2(q, &s.eax) : audio::eax::WriteSource2(s.eax, q);
    } else {
      set ? audio::eax::ReadSource3(q, &s.eax) : audio::eax::WriteSource3(s.eax, q);
    }
    return true;
  }
  return audio::eax::SourceAttribute(&s.eax, name, int(s.prov), GPtr(guest), set);
}

uint32_t ListenerSize(Prov p, const std::string& name) {
  if (name != "EAX all parameters") return 12;  // at most a pan vector
  return p == Prov::kEax1   ? audio::eax::kListener1Size
         : p == Prov::kEax2 ? audio::eax::kListener2Size
                            : audio::eax::kListener3Size;
}

// AIL_3D_provider_attribute(lib, name, void* value). Miles 6.5 first
// stores -1, which is what a name the provider lacks leaves; games detect
// missing features that way (e.g. EAX 3 by whether "EAX3 room LF" reads
// -1).
HLE_NAMED("mss32.dll", "_AIL_3D_provider_attribute@12", AIL_3D_provider_attribute, 3) {
  std::string name = GStr(Arg(c, 1));
  uint32_t out = Arg(c, 2);
  if (!out || !EnsureReadable(c, out, 4)) return 0;
  Wr32(out, 0xFFFFFFFF);
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (!d || !EnsureReadable(c, out, ListenerSize(d->kind, name))) return 0;
  std::lock_guard<std::mutex> l(Mu());
  if (name == "Maximum supported samples") {
    Wr32(out, S().prefs[1]);  // DIG_MIXER_CHANNELS
    return 0;
  }
  if (IsEax(d->kind) && ListenerAttribute(d->kind, name, out, false)) return 0;
  RECOMP_TRACE("  AIL_3D_provider_attribute(%s, %s): not provided\n", d->name, name.c_str());
  return 0;
}

// AIL_set_3D_provider_preference(lib, name, void const* value).
HLE_NAMED("mss32.dll", "_AIL_set_3D_provider_preference@12", AIL_set_3D_provider_preference, 3) {
  std::string name = GStr(Arg(c, 1));
  uint32_t in = Arg(c, 2);
  const ProviderDef* d = ProviderOf(Arg(c, 0));
  if (!d || !in || !EnsureReadable(c, in, ListenerSize(d->kind, name))) return kM3dNotEnabled;
  std::lock_guard<std::mutex> l(Mu());
  if (IsEax(d->kind) && ListenerAttribute(d->kind, name, in, true)) {
    if (d->kind == S().active) ApplyEaxListener();
    return kM3dNoErr;
  }
  RECOMP_TRACE("  AIL_set_3D_provider_preference(%s): not provided\n", name.c_str());
  return kM3dNotEnabled;
}

// AIL_set_3D_sample_preference(S, name, void const* value).
HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_preference@12", AIL_set_3D_sample_preference, 3) {
  std::string name = GStr(Arg(c, 1));
  uint32_t in = Arg(c, 2);
  uint32_t size = name == "EAX sample all parameters" ? audio::eax::kSource3Size : 4;
  if (!in || !EnsureReadable(c, in, size)) return kM3dNotEnabled;
  std::lock_guard<std::mutex> l(Mu());
  Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_preference");
  if (s && IsEax(s->prov) && SampleAttribute(*s, name, in, true)) {
    ApplyEax(*s);
    return kM3dNoErr;
  }
  RECOMP_TRACE("  AIL_set_3D_sample_preference(%s): not provided\n", name.c_str());
  return kM3dNotEnabled;
}

HLE_NAMED("mss32.dll", "_AIL_set_3D_sample_loop_count@8", AIL_set_3D_sample_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Sample* s = FindSample(Arg(c, 0), true, "AIL_set_3D_sample_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_3D_sample_status@4", AIL_3D_sample_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().samples.find(Arg(c, 0));
  if (it == S().samples.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// AIL_set_3D_position(H3DPOBJECT, F32 x, F32 y, F32 z): the listener or a
// 3D sample.
HLE_NAMED("mss32.dll", "_AIL_set_3D_position@16", AIL_set_3D_position, 4) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  audio::Vec3 p{F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  uint32_t obj = Arg(c, 0);
  if (obj && obj == st.listener) {
    st.lpos = p;
    audio::GetMixer().SetListener(st.lpos, st.lface, st.lup);
    ApplyEaxAll();
  } else if (Sample* s = FindSample(obj, true, "AIL_set_3D_position")) {
    s->v.pos = p;
    ApplyEax(*s);
  }
  return 0;
}

// AIL_set_3D_orientation(obj, face xyz, up xyz). Sample orientation only
// matters for sound cones, which the game does not set.
HLE_NAMED("mss32.dll", "_AIL_set_3D_orientation@28", AIL_set_3D_orientation, 7) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  if (Arg(c, 0) && Arg(c, 0) == st.listener) {
    st.lface = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
    st.lup = {F(Arg(c, 4)), F(Arg(c, 5)), F(Arg(c, 6))};
    audio::GetMixer().SetListener(st.lpos, st.lface, st.lup);
  }
  return 0;
}

// ---- streams ---------------------------------------------------------------------------

// Reads a whole file through the game's file callbacks (its own file
// system), on the calling guest thread.
bool ReadThroughCallbacks(Ctx& c, uint32_t name, std::vector<uint8_t>* out) {
  uint32_t open_cb, close_cb, read_cb;
  {
    std::lock_guard<std::mutex> l(Mu());
    open_cb = S().file_open;
    close_cb = S().file_close;
    read_cb = S().file_read;
  }
  if (!open_cb || !read_cb) return false;
  // Guest scratch: the handle out-parameter and a read buffer.
  static uint32_t scratch = 0;
  constexpr uint32_t kBuf = 0x10000;
  if (!scratch) scratch = HeapAllocProcess(kBuf + 16, true);
  uint32_t hptr = scratch, buf = scratch + 16;
  Wr32(hptr, 0);
  if (!CallGuest(c, open_cb, {name, hptr})) return false;
  uint32_t fh = Rd32(hptr);
  for (;;) {
    uint32_t got = CallGuest(c, read_cb, {fh, buf, kBuf});
    if (got > kBuf) got = 0;
    out->insert(out->end(), GPtr(buf), GPtr(buf) + got);
    if (got < kBuf) break;
  }
  if (close_cb) CallGuest(c, close_cb, {fh});
  return true;
}

// AIL_open_stream(HDIGDRIVER, char const* filename, S32 stream_mem).
HLE_NAMED("mss32.dll", "_AIL_open_stream@12", AIL_open_stream, 3) {
  std::string name = GStr(Arg(c, 1));
  {
    std::lock_guard<std::mutex> l(Mu());
    if (!S().driver || Arg(c, 0) != S().driver) return 0;
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  if (!ReadThroughCallbacks(c, Arg(c, 1), bytes.get()) || bytes->empty()) {
    WarnF("mss32: AIL_open_stream(%s): cannot read the file\n", name.c_str());
    SetError("Unable to open file " + name);
    return 0;
  }
  std::string err;
  auto src = audio::MakeSource(bytes, &err);
  if (!src) {
    WarnF("mss32: AIL_open_stream(%s): %s\n", name.c_str(), err.c_str());
    SetError("Unsupported stream format: " + err);
    return 0;
  }
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  auto s = std::make_unique<Stream>();
  s->handle = NewBlock(st);
  s->v.src = std::move(src);
  s->v.state = Voice::State::Paused;  // opened, not started
  s->v.miles = MilesOf(false, Prov::kFast2D);
  ApplyVolume(*s);
  uint32_t h = s->handle;
  RECOMP_TRACE("  AIL_open_stream(%s) -> %08X (%d Hz, %d ch)\n", name.c_str(), h,
            s->v.src->rate(), s->v.src->channels());
  st.streams[h] = std::move(s);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_close_stream@4", AIL_close_stream, 1) {
  std::lock_guard<std::mutex> l(Mu());
  uint32_t h = Arg(c, 0);
  Stream* s = FindStream(h, "AIL_close_stream");
  if (!s) return 0;
  audio::GetMixer().Remove(&s->v);
  S().streams.erase(h);
  S().free_blocks.push_back(h);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_stream@4", AIL_start_stream, 1) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_start_stream")) Start(s->v, &s->seq, 0);
  return 0;
}

// AIL_pause_stream(S, onoff).
HLE_NAMED("mss32.dll", "_AIL_pause_stream@8", AIL_pause_stream, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_pause_stream")) {
    if (Arg(c, 1)) {
      if (s->v.state == Voice::State::Playing) s->v.state = Voice::State::Paused;
    } else if (s->v.state == Voice::State::Paused) {
      s->v.state = Voice::State::Playing;
      audio::GetMixer().Add(&s->v);
      WakeReverb(s->v);
    }
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_loop_count@8", AIL_set_stream_loop_count, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_loop_count"))
    s->v.loops = std::max<int32_t>(int32_t(Arg(c, 1)), 0);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_volume@8", AIL_set_stream_volume, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_volume")) {
    s->vol = std::clamp<int>(int32_t(Arg(c, 1)), 0, 127) / 127.0f;
    ApplyVolume(*s);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stream_volume@4", AIL_stream_volume, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_stream_volume");
  return s ? uint32_t(std::lround(s->vol * 127)) : 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_volume_pan@12", AIL_set_stream_volume_pan, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_volume_pan")) {
    s->vol = F(Arg(c, 1));
    s->pan = F(Arg(c, 2));
    ApplyVolume(*s);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stream_volume_pan@12", AIL_stream_volume_pan, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_stream_volume_pan");
  if (Arg(c, 1))
    Wr32(Arg(c, 1), std::bit_cast<uint32_t>(s ? ReportedVolume(s->vol, s->pan) : 0.0f));
  if (Arg(c, 2)) Wr32(Arg(c, 2), std::bit_cast<uint32_t>(s ? s->pan : 0.5f));
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_reverb_levels@12", AIL_set_stream_reverb_levels, 3) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_reverb_levels")) {
    s->dry = std::clamp(F(Arg(c, 1)), 0.0f, 1.0f);
    s->wet = std::clamp(F(Arg(c, 2)), 0.0f, 1.0f);
    ApplyVolume(*s);
    if (s->v.state == Voice::State::Playing) WakeReverb(s->v);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_stream_playback_rate@4", AIL_stream_playback_rate, 1) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_stream_playback_rate");
  if (!s || !s->v.src) return 0;
  return uint32_t(s->v.rate > 0 ? std::lround(s->v.rate) : s->v.src->rate());
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_playback_rate@8", AIL_set_stream_playback_rate, 2) {
  std::lock_guard<std::mutex> l(Mu());
  if (Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_playback_rate"))
    s->v.rate = float(int32_t(Arg(c, 1)));
  return 0;
}

// AIL_stream_ms_position(S, S32* total_ms, S32* current_ms); the total is
// 0 when the length is unknown without decoding (MP3).
HLE_NAMED("mss32.dll", "_AIL_stream_ms_position@12", AIL_stream_ms_position, 3) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_stream_ms_position");
  uint32_t total = 0, cur = 0;
  if (s && s->v.src && s->v.src->rate()) {
    double r = s->v.src->rate();
    int64_t len = s->v.src->Length();
    if (len > 0) total = uint32_t(len * 1000.0 / r);
    cur = uint32_t(s->v.Position() * 1000.0 / r);
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), total);
  if (Arg(c, 2)) Wr32(Arg(c, 2), cur);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_stream_ms_position@8", AIL_set_stream_ms_position, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_set_stream_ms_position");
  if (!s || !s->v.src) return 0;
  int64_t f = int64_t(int32_t(Arg(c, 1))) * s->v.src->rate() / 1000;
  if (!s->v.src->Seek(f)) {
    WarnF("mss32: AIL_set_stream_ms_position(%d): this stream can only restart\n",
          int32_t(Arg(c, 1)));
    f = 0;
  }
  s->v.Seek(f);
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_register_stream_callback@8", AIL_register_stream_callback, 2) {
  std::lock_guard<std::mutex> l(Mu());
  Stream* s = FindStream(Arg(c, 0), "AIL_register_stream_callback");
  if (!s) return 0;
  uint32_t old = s->cb;
  s->cb = Arg(c, 1);
  return old;
}

HLE_NAMED("mss32.dll", "_AIL_stream_status@4", AIL_stream_status, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().streams.find(Arg(c, 0));
  if (it == S().streams.end()) return kSmpFree;
  return StatusOf(it->second->v);
}

// ---- timers --------------------------------------------------------------------------------

// AIL_register_timer(AILTIMERCB) -> HTIMER (-1 on failure).
HLE_NAMED("mss32.dll", "_AIL_register_timer@4", AIL_register_timer, 1) {
  std::lock_guard<std::mutex> l(Mu());
  State& st = S();
  uint32_t h = st.next_timer++;
  st.timers[h].cb = Arg(c, 0);
  return h;
}

HLE_NAMED("mss32.dll", "_AIL_set_timer_user@8", AIL_set_timer_user, 2) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it == S().timers.end()) return 0;
  uint32_t old = it->second.user;
  it->second.user = Arg(c, 1);
  return old;
}

// AIL_set_timer_period(HTIMER, U32 microseconds).
HLE_NAMED("mss32.dll", "_AIL_set_timer_period@8", AIL_set_timer_period, 2) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it != S().timers.end()) {
    it->second.period_us = Arg(c, 1);
    it->second.next = Clock::now() + std::chrono::microseconds(Arg(c, 1));
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_start_timer@4", AIL_start_timer, 1) {
  std::lock_guard<std::mutex> l(Mu());
  auto it = S().timers.find(Arg(c, 0));
  if (it != S().timers.end() && !it->second.running) {
    it->second.running = true;
    it->second.next = Clock::now() + std::chrono::microseconds(it->second.period_us);
  }
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_release_timer_handle@4", AIL_release_timer_handle, 1) {
  std::lock_guard<std::mutex> l(Mu());
  S().timers.erase(Arg(c, 0));
  return 0;
}

// ---- misc ----------------------------------------------------------------------------------

// Miles 6.5's digital master reverb. The driver's dry level scales every
// sample's direct path, so a change reapplies all levels.
void ApplyLevelsAll() {
  for (auto& [h, s] : S().samples) {
    if (s->is3d)
      ApplyEax(*s);
    else
      ApplyVolume(*s);
  }
  for (auto& [h, s] : S().streams) ApplyVolume(*s);
}

// AIL_set_digital_master_room_type(dig, room): an entry of Miles' room
// table (the EAX environment numbers) as reverb time, predelay, damping
// and wet level, with dry level 1. Miles does not range-check; rooms past
// its table are refused here.
HLE_NAMED("mss32.dll", "_AIL_set_digital_master_room_type@8", AIL_set_digital_master_room_type, 2) {
  std::lock_guard<std::mutex> l(Mu());
  int32_t room = int32_t(Arg(c, 1));
  if (room < 0 || room >= audio::DigitalReverb::kRooms) {
    WarnF("mss32: AIL_set_digital_master_room_type: room %d out of range\n", room);
    return 0;
  }
  audio::Mixer& m = audio::GetMixer();
  m.digital_reverb().SetRoomType(room, m.now_ms());
  ApplyLevelsAll();
  return 0;
}

// AIL_set_digital_master_reverb(dig, F32 time, F32 predelay, F32 damping).
HLE_NAMED("mss32.dll", "_AIL_set_digital_master_reverb@16", AIL_set_digital_master_reverb, 4) {
  std::lock_guard<std::mutex> l(Mu());
  audio::Mixer& m = audio::GetMixer();
  m.digital_reverb().SetReverb(F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3)), m.now_ms());
  return 0;
}

// AIL_digital_master_reverb(dig, F32* time, F32* predelay, F32* damping).
HLE_NAMED("mss32.dll", "_AIL_digital_master_reverb@16", AIL_digital_master_reverb, 4) {
  std::lock_guard<std::mutex> l(Mu());
  const audio::DigitalReverb& r = audio::GetMixer().digital_reverb();
  if (Arg(c, 1)) Wr32(Arg(c, 1), std::bit_cast<uint32_t>(r.time()));
  if (Arg(c, 2)) Wr32(Arg(c, 2), std::bit_cast<uint32_t>(r.predelay()));
  if (Arg(c, 3)) Wr32(Arg(c, 3), std::bit_cast<uint32_t>(r.damping()));
  return 0;
}

// AIL_set_digital_master_reverb_levels(dig, F32 dry, F32 wet).
HLE_NAMED("mss32.dll", "_AIL_set_digital_master_reverb_levels@12",
          AIL_set_digital_master_reverb_levels, 3) {
  std::lock_guard<std::mutex> l(Mu());
  audio::Mixer& m = audio::GetMixer();
  m.digital_reverb().SetLevels(F(Arg(c, 1)), F(Arg(c, 2)), m.now_ms());
  ApplyLevelsAll();
  return 0;
}

// AIL_digital_master_reverb_levels(dig, F32* dry, F32* wet).
HLE_NAMED("mss32.dll", "_AIL_digital_master_reverb_levels@12", AIL_digital_master_reverb_levels,
          3) {
  std::lock_guard<std::mutex> l(Mu());
  const audio::DigitalReverb& r = audio::GetMixer().digital_reverb();
  if (Arg(c, 1)) Wr32(Arg(c, 1), std::bit_cast<uint32_t>(r.dry()));
  if (Arg(c, 2)) Wr32(Arg(c, 2), std::bit_cast<uint32_t>(r.wet()));
  return 0;
}

// The DirectSound focus window: SDL's audio is not tied to a window.
HLE_NAMED("mss32.dll", "_AIL_set_DirectSound_HWND@8", AIL_set_DirectSound_HWND, 2) {
  return 0;
}

// The mixer's CPU load is not measured; report none.
HLE_NAMED("mss32.dll", "_AIL_digital_CPU_percent@4", AIL_digital_CPU_percent, 1) { return 0; }

// ---- format conversion ---------------------------------------------------------------

// AILMIXINFO starts with the source's AILSOUNDINFO; the rest is Miles'
// own decoder state.
bool ReadSoundInfo(uint32_t info, audio::RawFormat* f, uint32_t* data, uint32_t* len) {
  uint32_t format = Rd32(info);
  if (format != 1 && format != 0x11) return false;
  f->adpcm = format == 0x11;
  *data = Rd32(info + 4);
  *len = Rd32(info + 8);
  f->rate = int(Rd32(info + 12));
  f->bits = int(Rd32(info + 16));
  f->channels = int(Rd32(info + 20));
  f->block_size = Rd32(info + 28);
  return f->rate > 0;
}

// Frames out of a source of `in_frames` frames at `in_rate`, at `out_rate`.
int64_t ConvertedFrames(int64_t in_frames, int in_rate, uint32_t out_rate) {
  return (in_frames * int64_t(out_rate) + in_rate - 1) / in_rate;
}

int64_t SourceFrames(const audio::RawFormat& f, uint32_t len) {
  if (!f.adpcm) return len / uint32_t(f.channels * f.bits / 8);
  uint32_t spb = (f.block_size - 4 * uint32_t(f.channels)) * 2 / uint32_t(f.channels) + 1;
  uint32_t blocks = len / f.block_size, rest = len % f.block_size;
  int64_t n = int64_t(blocks) * spb;
  if (rest > 4u * uint32_t(f.channels))
    n += (rest - 4 * uint32_t(f.channels)) * 2 / uint32_t(f.channels) + 1;
  return n;
}

// Only one source is supported: the size of AILMIXINFO (and so where a
// second one starts) is Miles-internal.
bool OneSource(uint32_t n, const char* fn) {
  if (n == 1) return true;
  WarnF("mss32: %s with %u sources: only one is supported\n", fn, n);
  return n > 1;  // convert the first
}

// AIL_size_processed_digital_audio(dest_rate, dest_format, num_srcs,
// AILMIXINFO const* src) -> bytes AIL_process_digital_audio will write.
HLE_NAMED("mss32.dll", "_AIL_size_processed_digital_audio@16", AIL_size_processed_digital_audio, 4) {
  uint32_t rate = Arg(c, 0), fmt = Arg(c, 1), src = Arg(c, 3);
  audio::RawFormat f;
  uint32_t data, len;
  if (!OneSource(Arg(c, 2), "AIL_size_processed_digital_audio") ||
      !ReadSoundInfo(src, &f, &data, &len))
    return 0;
  return uint32_t(ConvertedFrames(SourceFrames(f, len), f.rate, rate) * kBytesPerFrame[fmt & 3]);
}

// AIL_process_digital_audio(dest, dest_size, dest_rate, dest_format,
// num_srcs, AILMIXINFO* src): decodes the source, resamples it (linear)
// and writes dest_format into dest; returns the bytes written.
HLE_NAMED("mss32.dll", "_AIL_process_digital_audio@24", AIL_process_digital_audio, 6) {
  uint32_t dest = Arg(c, 0), size = Arg(c, 1), rate = Arg(c, 2), fmt = Arg(c, 3) & 3,
           src = Arg(c, 5);
  audio::RawFormat f;
  uint32_t data, len;
  if (!OneSource(Arg(c, 4), "AIL_process_digital_audio") || !ReadSoundInfo(src, &f, &data, &len) ||
      !EnsureReadable(c, data, len)) {
    WarnF("mss32: AIL_process_digital_audio: source not handled\n");
    return 0;
  }
  std::vector<float> in;
  std::string err;
  if (!audio::DecodeRaw(GPtr(data), len, f, &in, &err)) {
    WarnF("mss32: AIL_process_digital_audio: %s\n", err.c_str());
    return 0;
  }
  int out_ch = fmt & 2 ? 2 : 1, bpf = kBytesPerFrame[fmt];
  int64_t in_frames = int64_t(in.size()) / f.channels;
  if (!in_frames) return 0;
  int64_t frames = std::min<int64_t>(size / uint32_t(bpf),
                                     ConvertedFrames(in_frames, f.rate, rate));
  double step = double(f.rate) / rate;
  uint8_t* d = GPtr(dest);
  auto at = [&](int64_t frame, int ch) { return in[size_t(frame) * f.channels + size_t(ch)]; };
  for (int64_t o = 0; o < frames; o++) {
    double pos = double(o) * step;
    int64_t a = std::min<int64_t>(int64_t(pos), in_frames - 1);
    int64_t b = std::min<int64_t>(a + 1, in_frames - 1);
    float t = float(pos - double(a));
    for (int ch = 0; ch < out_ch; ch++) {
      float v;
      if (out_ch == 1 && f.channels == 2) {  // stereo to mono: the average
        float l = at(a, 0) + (at(b, 0) - at(a, 0)) * t;
        float r = at(a, 1) + (at(b, 1) - at(a, 1)) * t;
        v = 0.5f * (l + r);
      } else {
        int sc = std::min(ch, f.channels - 1);
        v = at(a, sc) + (at(b, sc) - at(a, sc)) * t;
      }
      v = std::clamp(v, -1.0f, 1.0f);
      size_t i = size_t(o) * size_t(out_ch) + size_t(ch);
      if (fmt & 1) {
        int16_t s16 = int16_t(std::lround(v * 32767.0f));
        std::memcpy(d + 2 * i, &s16, 2);
      } else {
        d[i] = uint8_t(std::lround(v * 127.0f) + 128);
      }
    }
  }
  return uint32_t(frames * bpf);
}

// AIL_WAV_info(void const* data, AILSOUNDINFO* info) -> nonzero if a WAV.
HLE_NAMED("mss32.dll", "_AIL_WAV_info@8", AIL_WAV_info, 2) {
  uint32_t data = Arg(c, 0), info = Arg(c, 1);
  audio::WavInfo w;
  if (!WavInfoOf(c, data, &w)) return 0;
  // AILSOUNDINFO {format, data_ptr, data_len, rate, bits, channels,
  // samples, block_size, initial_ptr}
  Wr32(info + 0x00, w.format);
  Wr32(info + 0x04, data + w.data_offset);
  Wr32(info + 0x08, w.data_len);
  Wr32(info + 0x0C, w.rate);
  Wr32(info + 0x10, w.bits);
  Wr32(info + 0x14, w.channels);
  uint32_t frames = w.block_align ? w.data_len / w.block_align : 0;
  if (w.format == 0x11 && w.block_align > 4u * w.channels && w.channels) {
    audio::RawFormat f;
    f.adpcm = true;
    f.channels = w.channels;
    f.block_size = w.block_align;
    frames = uint32_t(SourceFrames(f, w.data_len));
  }
  Wr32(info + 0x18, frames);  // sample frames
  Wr32(info + 0x1C, w.block_align);
  Wr32(info + 0x20, data + w.data_offset);
  return 1;
}

}  // namespace

bool MilesDriverOpen() {
  std::lock_guard<std::mutex> l(Mu());
  return S().driver != 0;
}

}  // namespace recomp::hle
