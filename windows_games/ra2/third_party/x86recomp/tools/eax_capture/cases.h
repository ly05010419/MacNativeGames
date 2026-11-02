/* The capture cases and stimuli, shared by eaxcap.exe (real Miles on EAX
   hardware, Win32) and eax_render (x86recomp's renderer, Linux) so both
   record the same cases on the same timeline. Plain C; no OS calls. */
#ifndef EAX_CAPTURE_CASES_H
#define EAX_CAPTURE_CASES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Timeline of every recording, in seconds. The recorder runs for
   EC_PREROLL before anything plays (noise floor). Then the sync click (its
   own 3D sample at EC_SYNC_POS, direct path only) and the stimulus sample
   start back to back; the stimulus sample begins with EC_LEAD of silence,
   so the stimulus proper starts EC_LEAD after the click. */
#define EC_PREROLL 0.5
#define EC_LEAD 0.25
#define EC_MAX_TAIL 25.0

/* Stimuli (bit flags; a case lists its default set). */
#define EC_STIM_IMPULSE 1u /* one sample at 0.5 */
#define EC_STIM_SWEEP 2u   /* exponential sine sweep 20 Hz-20 kHz, 2 s, 0.5 */
#define EC_STIM_NOISE 4u   /* pink noise, 1 s, -15 dBFS RMS */
#define EC_STIM_TONE 8u    /* 1 kHz sine, 1 s, 0.5 (level check) */
#define EC_STIM_ALL 15u

/* A Miles 3D attribute (Msseax.m3d's names) set to a value. */
typedef struct {
  const char *name;
  char type; /* 'i' S32, 'u' U32, 'f' F32 */
  double value;
} EcAttr;

#define EC_MAX_ATTR 6

typedef struct {
  char name[48];
  const char *group; /* level, env, eax1, listener, source, distance, position */
  int min_eax;       /* 0 any provider; else the EAX version it needs ... */
  int max_eax;       /* ... up to this one (0 no limit) */
  int environment;   /* EAX environment set first; -1 none */
  int nl, ns;
  EcAttr l[EC_MAX_ATTR]; /* listener attributes, after the environment */
  EcAttr s[EC_MAX_ATTR]; /* stimulus sample attributes */
  float pos[3];          /* stimulus position (Miles: x right, y up, z ahead) */
  float min_dist, max_dist;
  unsigned stimuli;
} EcCase;

/* The listener sits at the origin facing +z with +y up. */
extern const float EC_SYNC_POS[3];

int ec_case_count(void);
const EcCase *ec_case(int i);

/* The EAX versions (min, max; max 0 = any later) having attribute `name`;
   0 if it is not one of Msseax.m3d's. */
int ec_attr_versions(const char *name, int *min_eax, int *max_eax);
/* Whether a provider with EAX version `eax` (0 = not EAX) has it. */
int ec_attr_applies(const char *name, int eax);
int ec_case_applies(const EcCase *c, int eax);

/* Listener and source attribute names to read back after setting, with
   their types ('i', 'u', 'f', 'v' = 3 x F32), terminated by a null name. */
typedef struct {
  const char *name;
  char type;
} EcAttrName;
extern const EcAttrName ec_listener_names[];
extern const EcAttrName ec_source_names[];

/* Every source attribute of EAX version `eax` at its EAX default (EAX 2.0
   and 3.0 differ in the occlusion room ratio; EAX 1.0 has only the reverb
   mix), so each case can start from a complete known state. Returns the
   count; *out points into a static table. */
int ec_source_defaults(int eax, const EcAttr **out);

/* Whether a listener attribute is one EAX rescales when the environment
   size changes (with the *SCALE flags set). */
int ec_attr_size_scaled(const char *name);

/* Seconds to record after the stimulus ends for a reverb decaying in
   `decay_time` seconds (60 dB) with the given delays. */
double ec_tail(double decay_time, double reflections_delay, double reverb_delay);
/* The decay time a case should produce, from its attributes and EAX's
   preset table (for when the provider cannot report it). */
double ec_decay_estimate(const EcCase *c);
/* Whether the case turns the environment off on EAX version `eax`. */
int ec_case_dry(const EcCase *c, int eax);

/* Stimulus `stim` (one EC_STIM_* bit) at `rate`: mono floats, malloc'd
   into *out (EC_LEAD of silence not included). Returns the frame count, 0
   on failure. */
int ec_stimulus(unsigned stim, int rate, float **out);
const char *ec_stim_name(unsigned stim);
/* "impulse,sweep" -> bits; 0 if a name is unknown. */
unsigned ec_stim_parse(const char *list);

/* Comma-separated patterns with '*' wildcards; an empty or null filter
   matches everything. */
int ec_match(const char *filter, const char *name);

/* EAX version of a Miles 3D provider name (0 for non-EAX ones) and a short
   tag for directory names ("eax2", "fast2d", "ds3dhw", ...). */
int ec_provider_eax(const char *provider);
void ec_provider_tag(const char *provider, char *tag, int size);

/* 16-bit PCM RIFF WAVE image of `frames` frames of `channels` interleaved
   floats (clipped), malloc'd; *size gets its length. */
unsigned char *ec_wav16(const float *x, int frames, int channels, int rate, unsigned *size);
/* Just the 44-byte header, for `frames` frames of 16-bit PCM. */
void ec_wav16_header(unsigned char *header, long frames, int channels, int rate);

#ifdef __cplusplus
}
#endif

#endif
