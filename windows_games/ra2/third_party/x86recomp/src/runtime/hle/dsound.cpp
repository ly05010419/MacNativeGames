// DirectSound 8 (dsound.dll) on the software mixer (runtime/audio/mixer.h),
// as Windows Vista and later implement it: in software, no hardware
// buffers.
//
// A secondary buffer's sample data lives in guest memory for Lock and in a
// host copy the mixer reads (the mixer never reads guest memory): Unlock
// copies what was written. Its voice plays the host copy as a ring (looping)
// or once. The play cursor is the voice's position; the write cursor is how
// far the mixer has read ahead of it, which is what DirectSound's write
// cursor means: data before it is committed. A one-shot buffer that played
// to its end stops with its position back at 0.
//
// DirectSound3D: the listener (from the primary buffer) and 3D buffers
// feed the mixer's DS3D model (pan by azimuth, distance attenuation with
// the listener's rolloff factor); DS3D_DEFERRED settings wait for
// CommitDeferredSettings. Cones and Doppler are stored, not applied.
// Position notifications (IDirectSoundNotify) and effects are not
// implemented: fatal and named.

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "runtime/audio/mixer.h"
#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057, DSERR_NODRIVER = 0x88780078,
                   DSERR_BADFORMAT = 0x88780064, DSERR_INVALIDCALL = 0x88780032,
                   DSERR_CONTROLUNAVAIL = 0x8878001E, DSERR_NOAGGREGATION = 0x80040110;

// DSBCAPS
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1, DSBCAPS_CTRL3D = 0x10,
                   DSBCAPS_CTRLFREQUENCY = 0x20, DSBCAPS_CTRLPAN = 0x40,
                   DSBCAPS_CTRLVOLUME = 0x80, DSBCAPS_CTRLPOSITIONNOTIFY = 0x100,
                   DSBCAPS_CTRLFX = 0x200, DSBCAPS_LOCSOFTWARE = 0x8;
// DSBSTATUS, DSBPLAY, DSBLOCK
constexpr uint32_t DSBSTATUS_PLAYING = 0x1, DSBSTATUS_LOOPING = 0x4, DSBSTATUS_LOCSOFTWARE = 0x10;
constexpr uint32_t DSBPLAY_LOOPING = 0x1;
constexpr uint32_t DSBLOCK_FROMWRITECURSOR = 0x1, DSBLOCK_ENTIREBUFFER = 0x2;
constexpr uint32_t DS3DMODE_HEADRELATIVE = 1, DS3DMODE_DISABLE = 2;
constexpr uint32_t DS3D_DEFERRED = 1;

const guest::GUID kIID_IDirectSound = {0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const guest::GUID kIID_IDirectSound8 = {0xC50A7E93, 0xF395, 0x4834, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
const guest::GUID kIID_IDirectSoundBuffer = {0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const guest::GUID kIID_IDirectSoundBuffer8 = {0x6825A449, 0x7524, 0x4D82, {0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E}};
const guest::GUID kIID_IDirectSound3DListener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const guest::GUID kIID_IDirectSound3DBuffer = {0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const guest::GUID kIID_IDirectSoundNotify = {0xB0210783, 0x89CD, 0x11D0, {0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16}};

audio::Mixer& M() { return audio::GetMixer(); }

float F(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
uint32_t Bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
audio::Vec3 ReadVec(uint32_t va) { return {F(Rd32(va)), F(Rd32(va + 4)), F(Rd32(va + 8))}; }
void WriteVec(uint32_t va, const audio::Vec3& v) {
  Wr32(va, Bits(v.x));
  Wr32(va + 4, Bits(v.y));
  Wr32(va + 8, Bits(v.z));
}

// ---- sample data and its source

struct Format {
  uint16_t channels = 2, bits = 16, block = 4;
  uint32_t rate = 44100;
};

// A buffer's samples, shared with its duplicates and the mixer: the guest
// copy for Lock and the host copy the mixer reads (written under the
// mixer's lock).
struct Data {
  Format fmt;
  std::vector<uint8_t> bytes;
  uint32_t mem = 0;
  ~Data() {
    if (mem) HeapFreeProcess(mem);
  }
};

// Plays a Data as a ring (looping) or once. Called with the mixer's lock
// held, as is everything that changes it.
class RingSource : public audio::Source {
 public:
  explicit RingSource(std::shared_ptr<Data> d) : d_(std::move(d)) {
    channels_ = d_->fmt.channels;
    rate_ = int(d_->fmt.rate);
  }
  bool loop = false;

  int64_t Frames() const { return int64_t(d_->bytes.size() / d_->fmt.block); }
  // The frame the next Read starts at, within the buffer.
  int64_t ReadFrame() const {
    int64_t n = Frames();
    return n ? next_ % n : 0;
  }

  int Read(float* out, int n) override {
    const int64_t frames = Frames();
    const Format& f = d_->fmt;
    int got = 0;
    for (; got < n; got++) {
      if (!frames || (!loop && next_ >= frames)) break;
      const uint8_t* p = d_->bytes.data() + size_t(next_ % frames) * f.block;
      for (int c = 0; c < f.channels; c++) {
        float s;
        if (f.bits == 8) {
          s = (float(p[c]) - 128.0f) / 128.0f;
        } else {
          int16_t v;
          std::memcpy(&v, p + 2 * c, 2);
          s = float(v) / 32768.0f;
        }
        *out++ = s;
      }
      next_++;
    }
    return got;
  }
  bool Seek(int64_t f) override {
    next_ = f;
    return true;
  }
  int64_t Length() const override { return loop ? -1 : Frames(); }

 private:
  std::shared_ptr<Data> d_;
  int64_t next_ = 0;
};

// ---- objects

struct Params3D {
  audio::Vec3 pos, vel, cone_orient{0, 0, 1};
  uint32_t inside_cone = 360, outside_cone = 360;
  int32_t cone_outside_volume = 0;
  float min_dist = 1.0f, max_dist = 1e9f;
  uint32_t mode = 0;
};

struct ListenerParams {
  audio::Vec3 pos, vel, front{0, 0, 1}, top{0, 1, 0};
  float distance_factor = 1.0f, rolloff = 1.0f, doppler = 1.0f;
};

struct SoundBuffer : com::Object {
  bool primary = false;
  uint32_t flags = 0;
  std::shared_ptr<Data> data;  // secondary buffers
  Format primary_format;       // primary buffer (SetFormat)
  std::unique_ptr<audio::Voice> voice;
  RingSource* src = nullptr;  // voice's source
  bool in_mixer = false, playing = false;
  int32_t volume = 0, pan = 0;
  uint32_t frequency = 0;  // 0: the format's rate
  Params3D p3d, pending3d;
  uint32_t buffer3d = 0, listener = 0;  // guest pointers of interfaces handed out
  ~SoundBuffer() override;
};

struct Buffer3D : com::Object {
  uint32_t buffer = 0;  // the IDirectSoundBuffer it belongs to (referenced)
  ~Buffer3D() override;
};

struct Listener : com::Object {
  uint32_t primary = 0;  // referenced
  ~Listener() override;
};

struct DirectSound : com::Object {
  uint32_t coop = 0;
  uint32_t primary = 0;  // the primary buffer, created once
};

// Deferred 3D settings, applied by CommitDeferredSettings.
std::mutex g_mu;
ListenerParams g_listener, g_listener_pending;
bool g_listener_dirty = false;
std::set<SoundBuffer*> g_dirty3d;

SoundBuffer::~SoundBuffer() {
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_dirty3d.erase(this);
  }
  if (in_mixer) {
    std::lock_guard<std::mutex> l(M().mu());
    M().Remove(voice.get());
  }
}

Buffer3D::~Buffer3D() {
  if (auto* b = com::As<SoundBuffer>(buffer)) b->buffer3d = 0;
  com::Release(buffer);
}

Listener::~Listener() {
  if (auto* b = com::As<SoundBuffer>(primary)) b->listener = 0;
  com::Release(primary);
}

uint32_t DirectSoundVtable() {
  return com::Vtable("dsound.dll", "IDirectSound8",
                     {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
                      "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact",
                      "GetSpeakerConfig", "SetSpeakerConfig", "Initialize",
                      "VerifyCertification"});
}

uint32_t BufferVtable() {
  return com::Vtable("dsound.dll", "IDirectSoundBuffer8",
                     {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
                      "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus",
                      "Initialize", "Lock", "Play", "SetCurrentPosition", "SetFormat",
                      "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore",
                      "SetFX", "AcquireResources", "GetObjectInPath"});
}

uint32_t Buffer3DVtable() {
  return com::Vtable("dsound.dll", "IDirectSound3DBuffer",
                     {"QueryInterface", "AddRef", "Release", "GetAllParameters",
                      "GetConeAngles", "GetConeOrientation", "GetConeOutsideVolume",
                      "GetMaxDistance", "GetMinDistance", "GetMode", "GetPosition",
                      "GetVelocity", "SetAllParameters", "SetConeAngles",
                      "SetConeOrientation", "SetConeOutsideVolume", "SetMaxDistance",
                      "SetMinDistance", "SetMode", "SetPosition", "SetVelocity"});
}

uint32_t ListenerVtable() {
  return com::Vtable("dsound.dll", "IDirectSound3DListener",
                     {"QueryInterface", "AddRef", "Release", "GetAllParameters",
                      "GetDistanceFactor", "GetDopplerFactor", "GetOrientation", "GetPosition",
                      "GetRolloffFactor", "GetVelocity", "SetAllParameters",
                      "SetDistanceFactor", "SetDopplerFactor", "SetOrientation", "SetPosition",
                      "SetRolloffFactor", "SetVelocity", "CommitDeferredSettings"});
}

template <typename T>
T* Self(Ctx& c, const char* iface) {
  T* o = com::As<T>(Arg(c, 0));
  if (!o) FatalF("%s call on unknown object %08X", iface, uint32_t(Arg(c, 0)));
  return o;
}

// ---- applying state to the voice (with the mixer's lock held)

float Amplitude(int32_t volume) {
  return volume <= -10000 ? 0.0f : std::pow(10.0f, float(volume) / 2000.0f);
}

void ApplyVoice(SoundBuffer& b) {
  audio::Voice& v = *b.voice;
  v.volume = Amplitude(b.volume);
  v.pan = float(b.pan);
  v.rate = float(b.frequency);
  bool is3d = (b.flags & DSBCAPS_CTRL3D) && b.p3d.mode != DS3DMODE_DISABLE;
  v.positional = is3d;
  v.head_relative = b.p3d.mode == DS3DMODE_HEADRELATIVE;
  v.pos = b.p3d.pos;
  v.min_dist = b.p3d.min_dist;
  v.max_dist = b.p3d.max_dist;
}

void ApplyListener(const ListenerParams& p) {
  std::lock_guard<std::mutex> l(M().mu());
  M().SetListener(p.pos, p.front, p.top);
  M().set_rolloff(p.rolloff);
}

// A 3D buffer's parameters changed: now, or at the next commit.
void Changed3D(SoundBuffer& b, uint32_t apply) {
  if (apply & DS3D_DEFERRED) {
    std::lock_guard<std::mutex> l(g_mu);
    g_dirty3d.insert(&b);
    return;
  }
  b.p3d = b.pending3d;
  std::lock_guard<std::mutex> l(M().mu());
  ApplyVoice(b);
}

void ChangedListener(uint32_t apply) {
  std::lock_guard<std::mutex> l(g_mu);
  if (apply & DS3D_DEFERRED) {
    g_listener_dirty = true;
    return;
  }
  g_listener = g_listener_pending;
  ApplyListener(g_listener);
}

// ---- formats

bool ReadFormat(uint32_t va, Format* f) {
  if (!va) return false;
  uint16_t tag = uint16_t(Rd32(va)), channels = uint16_t(Rd32(va) >> 16);
  uint32_t rate = Rd32(va + 4);
  uint16_t block = uint16_t(Rd32(va + 12)), bits = uint16_t(Rd32(va + 12) >> 16);
  if (tag != 1 /* WAVE_FORMAT_PCM */ || (channels != 1 && channels != 2) ||
      (bits != 8 && bits != 16) || block != channels * bits / 8 || rate < 100 || rate > 200000)
    return false;
  *f = {channels, bits, block, rate};
  return true;
}

void WriteFormat(uint32_t va, uint32_t size, const Format& f) {
  // WAVEFORMATEX, 18 bytes (cbSize 0); fewer if the caller's room is less.
  uint8_t w[18] = {};
  auto p16 = [&](int o, uint32_t v) { w[o] = uint8_t(v), w[o + 1] = uint8_t(v >> 8); };
  auto p32 = [&](int o, uint32_t v) { p16(o, v & 0xFFFF), p16(o + 2, v >> 16); };
  p16(0, 1);
  p16(2, f.channels);
  p32(4, f.rate);
  p32(8, f.rate * f.block);
  p16(12, f.block);
  p16(14, f.bits);
  std::memcpy(GPtr(va), w, std::min<uint32_t>(size, sizeof w));
}

// ---- DirectSoundCreate8 and IDirectSound8

// (lpcGuidDevice, ppDS8, pUnkOuter)
HLE_NAMED("dsound.dll", "#11", DirectSoundCreate8, 3) {
  if (!Arg(c, 1)) return DSERR_INVALIDPARAM;
  Wr32(Arg(c, 1), 0);
  if (Arg(c, 2)) return DSERR_NOAGGREGATION;
  if (!M().Open(48000)) return DSERR_NODRIVER;
  Wr32(Arg(c, 1), com::Publish(std::make_shared<DirectSound>(), DirectSoundVtable()));
  return DS_OK;
}

// #1 DirectSoundCreate(lpcGuidDevice, ppDS, pUnkOuter): the same object.
// IDirectSound's methods are the first of IDirectSound8's, and
// IDirectSoundBuffer's the first of IDirectSoundBuffer8's, so the version
// 8 vtables serve DirectSound 1 callers as they are.
static ::recomp::hle::Registrar reg_DirectSoundCreate_ord{"dsound.dll", "#1", 3,
                                                          &hle_DirectSoundCreate8, false};
static ::recomp::hle::Registrar reg_DirectSoundCreate{"dsound.dll", "DirectSoundCreate", 3,
                                                      &hle_DirectSoundCreate8, false};

HLE_NAMED("dsound.dll", "IDirectSound8::QueryInterface", DS8_QueryInterface, 3) {
  uint32_t riid = Arg(c, 1), out = Arg(c, 2);
  if (com::GuidEq(riid, kIID_IDirectSound8) || com::GuidEq(riid, kIID_IDirectSound)) {
    com::AddRef(Arg(c, 0));
    Wr32(out, Arg(c, 0));
    return com::kS_OK;
  }
  Wr32(out, 0);
  return com::kE_NOINTERFACE;
}
HLE_NAMED("dsound.dll", "IDirectSound8::AddRef", DS8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dsound.dll", "IDirectSound8::Release", DS8_Release, 1) { return com::Release(Arg(c, 0)); }

// (hwnd, dwLevel)
HLE_NAMED("dsound.dll", "IDirectSound8::SetCooperativeLevel", DS8_SetCooperativeLevel, 3) {
  Self<DirectSound>(c, "IDirectSound8")->coop = Arg(c, 2);
  return DS_OK;
}

// (pDSCaps): DSCAPS, 96 bytes. Software only, as Windows reports since
// Vista: no hardware mixing or 3D buffers.
HLE_NAMED("dsound.dll", "IDirectSound8::GetCaps", DS8_GetCaps, 2) {
  uint32_t p = Arg(c, 1);
  if (!p || Rd32(p) != 96) return DSERR_INVALIDPARAM;
  std::memset(GPtr(p + 4), 0, 92);
  Wr32(p + 4, 0xF1F);  // PRIMARY/SECONDARY MONO STEREO 8BIT 16BIT, CONTINUOUSRATE
  Wr32(p + 8, 100);    // dwMinSecondarySampleRate
  Wr32(p + 12, 200000);
  Wr32(p + 16, 1);  // dwPrimaryBuffers
  return DS_OK;
}

// (pdwSpeakerConfig)
HLE_NAMED("dsound.dll", "IDirectSound8::GetSpeakerConfig", DS8_GetSpeakerConfig, 2) {
  if (!Arg(c, 1)) return DSERR_INVALIDPARAM;
  Wr32(Arg(c, 1), 4);  // DSSPEAKER_STEREO
  return DS_OK;
}

// (pcDSBufferDesc, ppDSBuffer, pUnkOuter): DSBUFFERDESC {dwSize,
// dwFlags, dwBufferBytes, dwReserved, lpwfxFormat, guid3DAlgorithm}.
HLE_NAMED("dsound.dll", "IDirectSound8::CreateSoundBuffer", DS8_CreateSoundBuffer, 4) {
  DirectSound* ds = Self<DirectSound>(c, "IDirectSound8");
  uint32_t desc = Arg(c, 1), out = Arg(c, 2);
  if (!desc || !out || (Rd32(desc) != 36 && Rd32(desc) != 20)) return DSERR_INVALIDPARAM;
  Wr32(out, 0);
  if (Arg(c, 3)) return DSERR_NOAGGREGATION;
  uint32_t flags = Rd32(desc + 4), bytes = Rd32(desc + 8), fmt = Rd32(desc + 16);
  if (flags & DSBCAPS_CTRLFX) FatalF("dsound: buffers with effects (DSBCAPS_CTRLFX) are not supported");
  if (flags & DSBCAPS_PRIMARYBUFFER) {
    if (bytes || fmt) return DSERR_INVALIDPARAM;
    if (ds->primary) {  // one primary buffer per device
      com::AddRef(ds->primary);
      Wr32(out, ds->primary);
      return DS_OK;
    }
    auto b = std::make_shared<SoundBuffer>();
    b->primary = true;
    b->flags = flags;
    b->primary_format = {2, 16, 4, 22050};  // DirectSound's default
    ds->primary = com::Publish(b, BufferVtable());
    Wr32(out, ds->primary);
    return DS_OK;
  }
  auto d = std::make_shared<Data>();
  if (!ReadFormat(fmt, &d->fmt)) return DSERR_BADFORMAT;
  if (bytes < 4 || bytes > 0x0FFFFFFF) return DSERR_INVALIDPARAM;
  bytes -= bytes % d->fmt.block;
  if ((flags & DSBCAPS_CTRL3D) && d->fmt.channels != 1) return DSERR_INVALIDPARAM;
  d->bytes.assign(bytes, d->fmt.bits == 8 ? 0x80 : 0);
  d->mem = HeapAllocProcess(bytes, true);
  if (d->fmt.bits == 8) std::memset(GPtr(d->mem), 0x80, bytes);
  auto b = std::make_shared<SoundBuffer>();
  b->flags = flags;
  b->data = d;
  auto src = std::make_unique<RingSource>(d);
  b->src = src.get();
  b->voice = std::make_unique<audio::Voice>();
  b->voice->src = std::move(src);
  b->voice->dsound = true;
  b->voice->ds3d = true;
  b->voice->state = audio::Voice::State::Paused;
  b->pending3d = b->p3d;
  ApplyVoice(*b);
  Wr32(out, com::Publish(b, BufferVtable()));
  return DS_OK;
}

// (pDSBufferOriginal, ppDSBufferDuplicate): shares the samples.
HLE_NAMED("dsound.dll", "IDirectSound8::DuplicateSoundBuffer", DS8_DuplicateSoundBuffer, 3) {
  auto* o = com::As<SoundBuffer>(Arg(c, 1));
  uint32_t out = Arg(c, 2);
  if (!o || !out || o->primary) return DSERR_INVALIDPARAM;
  auto b = std::make_shared<SoundBuffer>();
  b->flags = o->flags;
  b->data = o->data;
  auto src = std::make_unique<RingSource>(o->data);
  b->src = src.get();
  b->voice = std::make_unique<audio::Voice>();
  b->voice->src = std::move(src);
  b->voice->dsound = true;
  b->voice->ds3d = true;
  b->voice->state = audio::Voice::State::Paused;
  b->volume = o->volume;
  b->pan = o->pan;
  b->frequency = o->frequency;
  b->p3d = b->pending3d = o->p3d;
  ApplyVoice(*b);
  Wr32(out, com::Publish(b, BufferVtable()));
  return DS_OK;
}

// ---- IDirectSoundBuffer8

SoundBuffer* Buf(Ctx& c) { return Self<SoundBuffer>(c, "IDirectSoundBuffer8"); }

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::QueryInterface", DSB8_QueryInterface, 3) {
  SoundBuffer* b = Buf(c);
  uint32_t riid = Arg(c, 1), out = Arg(c, 2);
  Wr32(out, 0);
  if (com::GuidEq(riid, kIID_IDirectSoundBuffer8) || com::GuidEq(riid, kIID_IDirectSoundBuffer)) {
    com::AddRef(Arg(c, 0));
    Wr32(out, Arg(c, 0));
    return com::kS_OK;
  }
  if (com::GuidEq(riid, kIID_IDirectSound3DListener) && b->primary &&
      (b->flags & DSBCAPS_CTRL3D)) {
    if (b->listener) {
      com::AddRef(b->listener);
    } else {
      auto l = std::make_shared<Listener>();
      l->primary = Arg(c, 0);
      com::AddRef(Arg(c, 0));
      b->listener = com::Publish(l, ListenerVtable());
    }
    Wr32(out, b->listener);
    return com::kS_OK;
  }
  if (com::GuidEq(riid, kIID_IDirectSound3DBuffer) && !b->primary &&
      (b->flags & DSBCAPS_CTRL3D)) {
    if (b->buffer3d) {
      com::AddRef(b->buffer3d);
    } else {
      auto t = std::make_shared<Buffer3D>();
      t->buffer = Arg(c, 0);
      com::AddRef(Arg(c, 0));
      b->buffer3d = com::Publish(t, Buffer3DVtable());
    }
    Wr32(out, b->buffer3d);
    return com::kS_OK;
  }
  if (com::GuidEq(riid, kIID_IDirectSoundNotify))
    FatalF("dsound: IDirectSoundNotify (position notifications) is not supported yet");
  return com::kE_NOINTERFACE;
}
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::AddRef", DSB8_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Release", DSB8_Release, 1) { return com::Release(Arg(c, 0)); }

// (pDSBufferCaps): DSBCAPS {dwSize, dwFlags, dwBufferBytes,
// dwUnlockTransferRate, dwPlayCpuOverhead}.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetCaps", DSB8_GetCaps, 2) {
  SoundBuffer* b = Buf(c);
  uint32_t p = Arg(c, 1);
  if (!p || Rd32(p) != 20) return DSERR_INVALIDPARAM;
  Wr32(p + 4, b->flags | DSBCAPS_LOCSOFTWARE);
  Wr32(p + 8, b->primary ? 0 : uint32_t(b->data->bytes.size()));
  Wr32(p + 12, 0);
  Wr32(p + 16, 0);
  return DS_OK;
}

// The play position in frames within the buffer (mixer lock held).
int64_t PlayFrame(const SoundBuffer& b) {
  int64_t n = b.src->Frames();
  if (!n) return 0;
  return ((b.voice->Position() % n) + n) % n;
}

// (pdwCurrentPlayCursor, pdwCurrentWriteCursor)
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetCurrentPosition", DSB8_GetCurrentPosition, 3) {
  SoundBuffer* b = Buf(c);
  if (b->primary) FatalF("dsound: GetCurrentPosition of the primary buffer");
  uint32_t play, write;
  {
    std::lock_guard<std::mutex> l(M().mu());
    if (b->voice->state == audio::Voice::State::Done) {
      b->voice->Seek(0);
      b->voice->state = audio::Voice::State::Paused;
      b->playing = false;
    }
    play = uint32_t(PlayFrame(*b)) * b->data->fmt.block;
    // Ahead of the play cursor by what the mixer has read: not playing,
    // the two are equal.
    write = b->playing && b->voice->state == audio::Voice::State::Playing
                ? uint32_t(b->src->ReadFrame()) * b->data->fmt.block
                : play;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), play);
  if (Arg(c, 2)) Wr32(Arg(c, 2), write);
  RECOMP_TRACE("  GetCurrentPosition -> play %u write %u\n", play, write);
  return DS_OK;
}

// (pwfxFormat, dwSizeAllocated, pdwSizeWritten)
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetFormat", DSB8_GetFormat, 4) {
  SoundBuffer* b = Buf(c);
  const Format& f = b->primary ? b->primary_format : b->data->fmt;
  if (Arg(c, 1)) WriteFormat(Arg(c, 1), Arg(c, 2), f);
  if (Arg(c, 3)) Wr32(Arg(c, 3), 18);
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetFormat", DSB8_SetFormat, 2) {
  SoundBuffer* b = Buf(c);
  if (!b->primary) return DSERR_INVALIDCALL;
  Format f;
  if (!ReadFormat(Arg(c, 1), &f)) return DSERR_BADFORMAT;
  b->primary_format = f;  // the mixer runs at its own rate
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetVolume", DSB8_GetVolume, 2) {
  SoundBuffer* b = Buf(c);
  if (!(b->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
  Wr32(Arg(c, 1), uint32_t(b->volume));
  return DS_OK;
}

// The primary buffer's volume is the master volume.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetVolume", DSB8_SetVolume, 2) {
  SoundBuffer* b = Buf(c);
  int32_t v = int32_t(Arg(c, 1));
  if (!(b->flags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
  if (v > 0 || v < -10000) return DSERR_INVALIDPARAM;
  b->volume = v;
  std::lock_guard<std::mutex> l(M().mu());
  if (b->primary)
    M().set_master(Amplitude(v));
  else
    ApplyVoice(*b);
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetPan", DSB8_GetPan, 2) {
  SoundBuffer* b = Buf(c);
  if (!(b->flags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
  Wr32(Arg(c, 1), uint32_t(b->pan));
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetPan", DSB8_SetPan, 2) {
  SoundBuffer* b = Buf(c);
  int32_t v = int32_t(Arg(c, 1));
  if (!(b->flags & DSBCAPS_CTRLPAN) || b->primary) return DSERR_CONTROLUNAVAIL;
  if (v > 10000 || v < -10000) return DSERR_INVALIDPARAM;
  b->pan = v;
  std::lock_guard<std::mutex> l(M().mu());
  ApplyVoice(*b);
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetFrequency", DSB8_GetFrequency, 2) {
  SoundBuffer* b = Buf(c);
  if (!(b->flags & DSBCAPS_CTRLFREQUENCY) || b->primary) return DSERR_CONTROLUNAVAIL;
  Wr32(Arg(c, 1), b->frequency ? b->frequency : b->data->fmt.rate);
  return DS_OK;
}

// (dwFrequency): 0 restores the format's rate.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetFrequency", DSB8_SetFrequency, 2) {
  SoundBuffer* b = Buf(c);
  uint32_t v = Arg(c, 1);
  if (!(b->flags & DSBCAPS_CTRLFREQUENCY) || b->primary) return DSERR_CONTROLUNAVAIL;
  if (v && (v < 100 || v > 200000)) return DSERR_INVALIDPARAM;
  b->frequency = v;
  std::lock_guard<std::mutex> l(M().mu());
  ApplyVoice(*b);
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::GetStatus", DSB8_GetStatus, 2) {
  SoundBuffer* b = Buf(c);
  uint32_t s = 0;
  if (b->primary) {
    s = b->playing ? DSBSTATUS_PLAYING | DSBSTATUS_LOOPING : 0;
  } else {
    std::lock_guard<std::mutex> l(M().mu());
    if (b->voice->state == audio::Voice::State::Done) {
      b->voice->Seek(0);
      b->voice->state = audio::Voice::State::Paused;
      b->playing = false;
    }
    if (b->playing) s = DSBSTATUS_PLAYING | (b->src->loop ? DSBSTATUS_LOOPING : 0);
    s |= DSBSTATUS_LOCSOFTWARE;
  }
  if (Arg(c, 1)) Wr32(Arg(c, 1), s);
  RECOMP_TRACE("  GetStatus -> %X\n", s);
  return DS_OK;
}

// (dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2,
// pdwAudioBytes2, dwFlags): the region in guest memory, in two parts if it
// wraps.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Lock", DSB8_Lock, 8) {
  SoundBuffer* b = Buf(c);
  if (b->primary) FatalF("dsound: Lock of the primary buffer (DSSCL_WRITEPRIMARY) is not supported");
  uint32_t size = uint32_t(b->data->bytes.size());
  uint32_t off = Arg(c, 1), bytes = Arg(c, 2), flags = Arg(c, 7);
  if (flags & DSBLOCK_FROMWRITECURSOR) {
    std::lock_guard<std::mutex> l(M().mu());
    off = b->playing ? uint32_t(b->src->ReadFrame()) * b->data->fmt.block
                     : uint32_t(PlayFrame(*b)) * b->data->fmt.block;
  }
  if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
  if (off >= size || !bytes || bytes > size || !Arg(c, 3) || !Arg(c, 4)) return DSERR_INVALIDPARAM;
  uint32_t first = std::min(bytes, size - off);
  Wr32(Arg(c, 3), b->data->mem + off);
  Wr32(Arg(c, 4), first);
  if (Arg(c, 5)) Wr32(Arg(c, 5), first < bytes ? b->data->mem : 0);
  if (Arg(c, 6)) Wr32(Arg(c, 6), bytes - first);
  if (first < bytes && !Arg(c, 5)) return DSERR_INVALIDPARAM;
  return DS_OK;
}

// (pvAudioPtr1, dwAudioBytes1, pvAudioPtr2, dwAudioBytes2): what was
// written goes to the mixer's copy.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Unlock", DSB8_Unlock, 5) {
  SoundBuffer* b = Buf(c);
  if (b->primary) return DSERR_INVALIDCALL;
  Data& d = *b->data;
  uint32_t size = uint32_t(d.bytes.size());
  std::lock_guard<std::mutex> l(M().mu());
  for (int part = 0; part < 2; part++) {
    uint32_t p = Arg(c, 1 + 2 * part), n = Arg(c, 2 + 2 * part);
    if (!p || !n) continue;
    if (p < d.mem || p - d.mem > size || n > size - (p - d.mem)) return DSERR_INVALIDPARAM;
    std::memcpy(d.bytes.data() + (p - d.mem), GPtr(p), n);
  }
  return DS_OK;
}

// (dwReserved1, dwPriority, dwFlags)
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Play", DSB8_Play, 4) {
  SoundBuffer* b = Buf(c);
  b->playing = true;
  if (b->primary) return DS_OK;  // the mixer always runs
  std::lock_guard<std::mutex> l(M().mu());
  if (b->voice->state == audio::Voice::State::Done) b->voice->Seek(0);
  b->src->loop = Arg(c, 3) & DSBPLAY_LOOPING;
  b->voice->loops = 1;  // the source loops itself
  b->voice->state = audio::Voice::State::Playing;
  if (!b->in_mixer) {
    M().Add(b->voice.get());
    b->in_mixer = true;
  }
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Stop", DSB8_Stop, 1) {
  SoundBuffer* b = Buf(c);
  b->playing = false;
  if (b->primary) return DS_OK;
  std::lock_guard<std::mutex> l(M().mu());
  if (b->voice->state == audio::Voice::State::Done) b->voice->Seek(0);
  b->voice->state = audio::Voice::State::Paused;
  // The mixer has read ahead; playing again continues from the play
  // cursor.
  b->voice->Seek(PlayFrame(*b));
  return DS_OK;
}

// (dwNewPosition): in bytes.
HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::SetCurrentPosition", DSB8_SetCurrentPosition, 2) {
  SoundBuffer* b = Buf(c);
  if (b->primary) return DSERR_INVALIDCALL;
  uint32_t pos = Arg(c, 1);
  if (pos >= b->data->bytes.size()) return DSERR_INVALIDPARAM;
  std::lock_guard<std::mutex> l(M().mu());
  b->voice->Seek(pos / b->data->fmt.block);
  if (b->voice->state == audio::Voice::State::Done)
    b->voice->state = b->playing ? audio::Voice::State::Playing : audio::Voice::State::Paused;
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSoundBuffer8::Restore", DSB8_Restore, 1) { return DS_OK; }

// ---- IDirectSound3DBuffer

SoundBuffer* Buf3D(Ctx& c) {
  Buffer3D* t = Self<Buffer3D>(c, "IDirectSound3DBuffer");
  auto* b = com::As<SoundBuffer>(t->buffer);
  if (!b) FatalF("dsound: IDirectSound3DBuffer of a released buffer");
  return b;
}

HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::QueryInterface", DS3DB_QueryInterface, 3) {
  Buffer3D* t = Self<Buffer3D>(c, "IDirectSound3DBuffer");
  if (com::GuidEq(Arg(c, 1), kIID_IDirectSound3DBuffer)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return com::kS_OK;
  }
  if (com::GuidEq(Arg(c, 1), kIID_IDirectSoundBuffer) || com::GuidEq(Arg(c, 1), kIID_IDirectSoundBuffer8)) {
    com::AddRef(t->buffer);
    Wr32(Arg(c, 2), t->buffer);
    return com::kS_OK;
  }
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::AddRef", DS3DB_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::Release", DS3DB_Release, 1) { return com::Release(Arg(c, 0)); }

// DS3DBUFFER: dwSize, vPosition, vVelocity, dwInsideConeAngle,
// dwOutsideConeAngle, vConeOrientation, lConeOutsideVolume, flMinDistance,
// flMaxDistance, dwMode (64 bytes).
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetAllParameters", DS3DB_GetAllParameters, 2) {
  const Params3D& p = Buf3D(c)->pending3d;
  uint32_t a = Arg(c, 1);
  if (!a || Rd32(a) != 64) return DSERR_INVALIDPARAM;
  WriteVec(a + 4, p.pos);
  WriteVec(a + 16, p.vel);
  Wr32(a + 28, p.inside_cone);
  Wr32(a + 32, p.outside_cone);
  WriteVec(a + 36, p.cone_orient);
  Wr32(a + 48, uint32_t(p.cone_outside_volume));
  Wr32(a + 52, Bits(p.min_dist));
  Wr32(a + 56, Bits(p.max_dist));
  Wr32(a + 60, p.mode);
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetAllParameters", DS3DB_SetAllParameters, 3) {
  SoundBuffer* b = Buf3D(c);
  uint32_t a = Arg(c, 1);
  if (!a || Rd32(a) != 64) return DSERR_INVALIDPARAM;
  Params3D& p = b->pending3d;
  p.pos = ReadVec(a + 4);
  p.vel = ReadVec(a + 16);
  p.inside_cone = Rd32(a + 28);
  p.outside_cone = Rd32(a + 32);
  p.cone_orient = ReadVec(a + 36);
  p.cone_outside_volume = int32_t(Rd32(a + 48));
  p.min_dist = F(Rd32(a + 52));
  p.max_dist = F(Rd32(a + 56));
  p.mode = Rd32(a + 60);
  Changed3D(*b, Arg(c, 2));
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetPosition", DS3DB_GetPosition, 2) {
  WriteVec(Arg(c, 1), Buf3D(c)->pending3d.pos);
  return DS_OK;
}
// (x, y, z, dwApply)
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetPosition", DS3DB_SetPosition, 5) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.pos = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  Changed3D(*b, Arg(c, 4));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetVelocity", DS3DB_GetVelocity, 2) {
  WriteVec(Arg(c, 1), Buf3D(c)->pending3d.vel);
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetVelocity", DS3DB_SetVelocity, 5) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.vel = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  Changed3D(*b, Arg(c, 4));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetMinDistance", DS3DB_GetMinDistance, 2) {
  Wr32(Arg(c, 1), Bits(Buf3D(c)->pending3d.min_dist));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetMinDistance", DS3DB_SetMinDistance, 3) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.min_dist = F(Arg(c, 1));
  Changed3D(*b, Arg(c, 2));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetMaxDistance", DS3DB_GetMaxDistance, 2) {
  Wr32(Arg(c, 1), Bits(Buf3D(c)->pending3d.max_dist));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetMaxDistance", DS3DB_SetMaxDistance, 3) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.max_dist = F(Arg(c, 1));
  Changed3D(*b, Arg(c, 2));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::GetMode", DS3DB_GetMode, 2) {
  Wr32(Arg(c, 1), Buf3D(c)->pending3d.mode);
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetMode", DS3DB_SetMode, 3) {
  SoundBuffer* b = Buf3D(c);
  if (Arg(c, 1) > DS3DMODE_DISABLE) return DSERR_INVALIDPARAM;
  b->pending3d.mode = Arg(c, 1);
  Changed3D(*b, Arg(c, 2));
  return DS_OK;
}
// Cones are stored, not applied.
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetConeAngles", DS3DB_SetConeAngles, 4) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.inside_cone = Arg(c, 1);
  b->pending3d.outside_cone = Arg(c, 2);
  Changed3D(*b, Arg(c, 3));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetConeOrientation", DS3DB_SetConeOrientation, 5) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.cone_orient = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  Changed3D(*b, Arg(c, 4));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DBuffer::SetConeOutsideVolume", DS3DB_SetConeOutsideVolume, 3) {
  SoundBuffer* b = Buf3D(c);
  b->pending3d.cone_outside_volume = int32_t(Arg(c, 1));
  Changed3D(*b, Arg(c, 2));
  return DS_OK;
}

// ---- IDirectSound3DListener

HLE_NAMED("dsound.dll", "IDirectSound3DListener::QueryInterface", DS3DL_QueryInterface, 3) {
  Listener* t = Self<Listener>(c, "IDirectSound3DListener");
  if (com::GuidEq(Arg(c, 1), kIID_IDirectSound3DListener)) {
    com::AddRef(Arg(c, 0));
    Wr32(Arg(c, 2), Arg(c, 0));
    return com::kS_OK;
  }
  if (com::GuidEq(Arg(c, 1), kIID_IDirectSoundBuffer) || com::GuidEq(Arg(c, 1), kIID_IDirectSoundBuffer8)) {
    com::AddRef(t->primary);
    Wr32(Arg(c, 2), t->primary);
    return com::kS_OK;
  }
  Wr32(Arg(c, 2), 0);
  return com::kE_NOINTERFACE;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::AddRef", DS3DL_AddRef, 1) { return com::AddRef(Arg(c, 0)); }
HLE_NAMED("dsound.dll", "IDirectSound3DListener::Release", DS3DL_Release, 1) { return com::Release(Arg(c, 0)); }

ListenerParams& Pending(Ctx& c) {
  Self<Listener>(c, "IDirectSound3DListener");
  return g_listener_pending;
}

// DS3DLISTENER: dwSize, vPosition, vVelocity, vOrientFront, vOrientTop,
// flDistanceFactor, flRolloffFactor, flDopplerFactor (64 bytes).
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetAllParameters", DS3DL_GetAllParameters, 2) {
  const ListenerParams& p = Pending(c);
  uint32_t a = Arg(c, 1);
  if (!a || Rd32(a) != 64) return DSERR_INVALIDPARAM;
  WriteVec(a + 4, p.pos);
  WriteVec(a + 16, p.vel);
  WriteVec(a + 28, p.front);
  WriteVec(a + 40, p.top);
  Wr32(a + 52, Bits(p.distance_factor));
  Wr32(a + 56, Bits(p.rolloff));
  Wr32(a + 60, Bits(p.doppler));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetAllParameters", DS3DL_SetAllParameters, 3) {
  ListenerParams& p = Pending(c);
  uint32_t a = Arg(c, 1);
  if (!a || Rd32(a) != 64) return DSERR_INVALIDPARAM;
  p.pos = ReadVec(a + 4);
  p.vel = ReadVec(a + 16);
  p.front = ReadVec(a + 28);
  p.top = ReadVec(a + 40);
  p.distance_factor = F(Rd32(a + 52));
  p.rolloff = F(Rd32(a + 56));
  p.doppler = F(Rd32(a + 60));
  ChangedListener(Arg(c, 2));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetPosition", DS3DL_GetPosition, 2) {
  WriteVec(Arg(c, 1), Pending(c).pos);
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetPosition", DS3DL_SetPosition, 5) {
  Pending(c).pos = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  ChangedListener(Arg(c, 4));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetVelocity", DS3DL_GetVelocity, 2) {
  WriteVec(Arg(c, 1), Pending(c).vel);
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetVelocity", DS3DL_SetVelocity, 5) {
  Pending(c).vel = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  ChangedListener(Arg(c, 4));
  return DS_OK;
}
// (pvOrientFront, pvOrientTop)
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetOrientation", DS3DL_GetOrientation, 3) {
  WriteVec(Arg(c, 1), Pending(c).front);
  WriteVec(Arg(c, 2), Pending(c).top);
  return DS_OK;
}
// (xFront, yFront, zFront, xTop, yTop, zTop, dwApply)
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetOrientation", DS3DL_SetOrientation, 8) {
  ListenerParams& p = Pending(c);
  p.front = {F(Arg(c, 1)), F(Arg(c, 2)), F(Arg(c, 3))};
  p.top = {F(Arg(c, 4)), F(Arg(c, 5)), F(Arg(c, 6))};
  ChangedListener(Arg(c, 7));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetRolloffFactor", DS3DL_GetRolloffFactor, 2) {
  Wr32(Arg(c, 1), Bits(Pending(c).rolloff));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetRolloffFactor", DS3DL_SetRolloffFactor, 3) {
  float r = F(Arg(c, 1));
  if (!(r >= 0.0f && r <= 10.0f)) return DSERR_INVALIDPARAM;
  Pending(c).rolloff = r;
  ChangedListener(Arg(c, 2));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetDistanceFactor", DS3DL_GetDistanceFactor, 2) {
  Wr32(Arg(c, 1), Bits(Pending(c).distance_factor));
  return DS_OK;
}
// Distance and Doppler factors only scale Doppler, which is not applied.
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetDistanceFactor", DS3DL_SetDistanceFactor, 3) {
  Pending(c).distance_factor = F(Arg(c, 1));
  ChangedListener(Arg(c, 2));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::GetDopplerFactor", DS3DL_GetDopplerFactor, 2) {
  Wr32(Arg(c, 1), Bits(Pending(c).doppler));
  return DS_OK;
}
HLE_NAMED("dsound.dll", "IDirectSound3DListener::SetDopplerFactor", DS3DL_SetDopplerFactor, 3) {
  Pending(c).doppler = F(Arg(c, 1));
  ChangedListener(Arg(c, 2));
  return DS_OK;
}

HLE_NAMED("dsound.dll", "IDirectSound3DListener::CommitDeferredSettings", DS3DL_CommitDeferredSettings, 1) {
  Self<Listener>(c, "IDirectSound3DListener");
  std::lock_guard<std::mutex> l(g_mu);
  if (g_listener_dirty) {
    g_listener = g_listener_pending;
    ApplyListener(g_listener);
    g_listener_dirty = false;
  }
  std::lock_guard<std::mutex> ml(M().mu());
  for (SoundBuffer* b : g_dirty3d) {
    b->p3d = b->pending3d;
    ApplyVoice(*b);
  }
  g_dirty3d.clear();
  return DS_OK;
}

}  // namespace
}  // namespace recomp::hle
