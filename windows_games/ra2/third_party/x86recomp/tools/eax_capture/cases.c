#include "cases.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const float EC_SYNC_POS[3] = {0.0f, 0.0f, 1.0f};

/* ---- EAX environments ------------------------------------------------- */

static const char *const kEnvNames[26] = {
    "generic",    "paddedcell",      "room",      "bathroom",      "livingroom",
    "stoneroom",  "auditorium",      "concerthall", "cave",        "arena",
    "hangar",     "carpetedhallway", "hallway",   "stonecorridor", "alley",
    "forest",     "city",            "mountains", "quarry",        "plain",
    "parkinglot", "sewerpipe",       "underwater", "drugged",      "dizzy",
    "psychotic",
};
/* Upper bounds of the presets' decay times in EAX 1.0 and 2.0 (only for
   sizing recordings when the provider cannot report its own), and their
   environment sizes. */
static const float kEnvDecay[26] = {
    1.49f, 0.17f, 0.40f, 1.50f,  0.50f, 2.31f, 4.32f, 3.96f, 2.91f,
    7.28f, 10.05f, 0.30f, 1.49f, 2.70f, 1.75f, 3.15f, 2.77f, 7.84f,
    1.50f, 2.77f, 1.65f, 2.89f, 1.50f, 8.39f, 17.23f, 7.56f,
};
static const float kEnvSize[26] = {
    7.5f,  1.4f,  1.9f, 1.4f,  2.5f, 11.6f, 21.6f,  19.6f, 14.6f,
    36.2f, 50.3f, 1.9f, 1.8f,  13.5f, 7.5f, 38.0f, 7.5f,  100.0f,
    17.5f, 42.5f, 8.3f, 1.7f,  1.8f, 1.9f,  1.8f,  1.0f,
};

/* ---- attribute names --------------------------------------------------- */

const EcAttrName ec_listener_names[] = {
    {"EAX environment selection", 'u'},
    {"EAX effect volume", 'f'},
    {"EAX decay time", 'f'},
    {"EAX damping", 'f'},
    {"EAX2 room", 'i'},
    {"EAX2 room HF", 'i'},
    {"EAX2 room rolloff", 'f'},
    {"EAX2 decay HF ratio", 'f'},
    {"EAX2 reflections", 'i'},
    {"EAX2 reflections delay", 'f'},
    {"EAX2 reverb", 'i'},
    {"EAX2 reverb delay", 'f'},
    {"EAX2 environment size", 'f'},
    {"EAX2 environment diffusion", 'f'},
    {"EAX2 air absorption", 'f'},
    {"EAX2 flags", 'u'},
    {"EAX3 room LF", 'i'},
    {"EAX3 decay LF ratio", 'f'},
    {"EAX3 reflections pan", 'v'},
    {"EAX3 reverb pan", 'v'},
    {"EAX3 echo time", 'f'},
    {"EAX3 echo depth", 'f'},
    {"EAX3 modulation time", 'f'},
    {"EAX3 modulation depth", 'f'},
    {"EAX3 HF reference", 'f'},
    {"EAX3 LF reference", 'f'},
    {0, 0},
};

const EcAttrName ec_source_names[] = {
    {"EAX sample reverb mix", 'f'},
    {"EAX2 sample direct", 'i'},
    {"EAX2 sample direct HF", 'i'},
    {"EAX2 sample room", 'i'},
    {"EAX2 sample room HF", 'i'},
    {"EAX2 sample room rolloff", 'f'},
    {"EAX2 sample obstruction", 'i'},
    {"EAX2 sample obstruction LF ratio", 'f'},
    {"EAX2 sample occlusion", 'i'},
    {"EAX2 sample occlusion LF ratio", 'f'},
    {"EAX2 sample occlusion room ratio", 'f'},
    {"EAX2 sample outside volume HF", 'i'},
    {"EAX2 sample air absorption", 'f'},
    {"EAX2 sample flags", 'u'},
    {"EAX3 sample exclusion", 'i'},
    {"EAX3 sample exclusion LF ratio", 'f'},
    {"EAX3 sample occlusion direct ratio", 'f'},
    {"EAX3 sample doppler factor", 'f'},
    {0, 0},
};

int ec_attr_versions(const char *name, int *min_eax, int *max_eax) {
  *min_eax = *max_eax = 0;
  if (!strncmp(name, "EAX3 ", 5)) {
    *min_eax = 3;
  } else if (!strncmp(name, "EAX2 ", 5)) {
    *min_eax = 2;
  } else if (!strcmp(name, "EAX damping") || !strcmp(name, "EAX effect volume")) {
    *min_eax = *max_eax = 1;
  } else if (!strncmp(name, "EAX ", 4)) {
    *min_eax = 1;
  } else {
    return 0;
  }
  return 1;
}

int ec_attr_applies(const char *name, int eax) {
  int lo, hi;
  if (!ec_attr_versions(name, &lo, &hi) || eax < 1) return 0;
  return eax >= lo && (!hi || eax <= hi);
}

int ec_case_applies(const EcCase *c, int eax) {
  if (c->min_eax && eax < c->min_eax) return 0;
  if (c->max_eax && eax > c->max_eax) return 0;
  return 1;
}

static const EcAttr kSourceDefaults2[] = {
    {"EAX2 sample direct", 'i', 0},
    {"EAX2 sample direct HF", 'i', 0},
    {"EAX2 sample room", 'i', 0},
    {"EAX2 sample room HF", 'i', 0},
    {"EAX2 sample room rolloff", 'f', 0.0},
    {"EAX2 sample obstruction", 'i', 0},
    {"EAX2 sample obstruction LF ratio", 'f', 0.0},
    {"EAX2 sample occlusion", 'i', 0},
    {"EAX2 sample occlusion LF ratio", 'f', 0.25},
    {"EAX2 sample occlusion room ratio", 'f', 0.5},
    {"EAX2 sample outside volume HF", 'i', 0},
    {"EAX2 sample air absorption", 'f', 1.0},
    {"EAX2 sample flags", 'u', 7},
};
static const EcAttr kSourceDefaults3[] = {
    {"EAX2 sample direct", 'i', 0},
    {"EAX2 sample direct HF", 'i', 0},
    {"EAX2 sample room", 'i', 0},
    {"EAX2 sample room HF", 'i', 0},
    {"EAX2 sample room rolloff", 'f', 0.0},
    {"EAX2 sample obstruction", 'i', 0},
    {"EAX2 sample obstruction LF ratio", 'f', 0.0},
    {"EAX2 sample occlusion", 'i', 0},
    {"EAX2 sample occlusion LF ratio", 'f', 0.25},
    {"EAX2 sample occlusion room ratio", 'f', 1.5},
    {"EAX2 sample outside volume HF", 'i', 0},
    {"EAX2 sample air absorption", 'f', 1.0},
    {"EAX2 sample flags", 'u', 7},
    {"EAX3 sample exclusion", 'i', 0},
    {"EAX3 sample exclusion LF ratio", 'f', 1.0},
    {"EAX3 sample occlusion direct ratio", 'f', 1.0},
    {"EAX3 sample doppler factor", 'f', 0.0},
};
/* EAX 1.0's default reverb mix: from the distance (EAX_REVERBMIX_USEDISTANCE). */
static const EcAttr kSourceDefaults1[] = {
    {"EAX sample reverb mix", 'f', -1.0},
};

int ec_source_defaults(int eax, const EcAttr **out) {
  if (eax == 1) {
    *out = kSourceDefaults1;
    return (int)(sizeof kSourceDefaults1 / sizeof kSourceDefaults1[0]);
  }
  if (eax == 2) {
    *out = kSourceDefaults2;
    return (int)(sizeof kSourceDefaults2 / sizeof kSourceDefaults2[0]);
  }
  if (eax >= 3) {
    *out = kSourceDefaults3;
    return (int)(sizeof kSourceDefaults3 / sizeof kSourceDefaults3[0]);
  }
  *out = 0;
  return 0;
}

int ec_attr_size_scaled(const char *name) {
  static const char *const scaled[] = {
      "EAX decay time",         "EAX2 reflections", "EAX2 reflections delay",
      "EAX2 reverb",            "EAX2 reverb delay", "EAX3 echo time",
      "EAX3 modulation time",
  };
  int i;
  for (i = 0; i < (int)(sizeof scaled / sizeof scaled[0]); i++)
    if (!strcmp(name, scaled[i])) return 1;
  return 0;
}

/* ---- the case list ----------------------------------------------------- */

#define EC_MAX_CASES 160
static EcCase g_cases[EC_MAX_CASES];
static int g_ncases = -1;

static EcCase *add(const char *group, int min_eax, int max_eax, int env,
                   unsigned stimuli, const char *fmt, double a, double b) {
  EcCase *c;
  if (g_ncases >= EC_MAX_CASES) return &g_cases[EC_MAX_CASES - 1]; /* never */
  c = &g_cases[g_ncases++];
  memset(c, 0, sizeof *c);
  sprintf(c->name, fmt, a, b);
  /* "-3000" reads badly in file names: m3000. */
  {
    char *p;
    for (p = c->name; *p; p++)
      if (*p == '-') *p = 'm';
  }
  c->group = group;
  c->min_eax = min_eax;
  c->max_eax = max_eax;
  c->environment = env;
  c->stimuli = stimuli;
  c->pos[2] = 1.0f; /* ahead, at the minimum distance: no attenuation */
  c->min_dist = 1.0f;
  c->max_dist = 10000.0f;
  return c;
}

static void lattr(EcCase *c, const char *name, char type, double v) {
  EcAttr *a = &c->l[c->nl++];
  a->name = name;
  a->type = type;
  a->value = v;
}

static void sattr(EcCase *c, const char *name, char type, double v) {
  EcAttr *a = &c->s[c->ns++];
  a->name = name;
  a->type = type;
  a->value = v;
}

/* No environmental reverb (each EAX version's way of turning it off). */
static void dry(EcCase *c) {
  lattr(c, "EAX2 room", 'i', -10000);
  lattr(c, "EAX effect volume", 'f', 0.0);
}

static void sweep_l(const char *label, const char *attr, char type, int min_eax,
                    const double *v, int n) {
  int i;
  char fmt[64];
  sprintf(fmt, "%s_%%g", label);
  for (i = 0; i < n; i++) lattr(add("listener", min_eax, 0, 0, EC_STIM_SWEEP, fmt, v[i], 0), attr, type, v[i]);
}

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void build(void) {
  int i;
  EcCase *c;
  g_ncases = 0;

  /* Recording level check: a tone straight ahead, no reverb. */
  c = add("level", 0, 0, 0, EC_STIM_TONE, "level_tone", 0, 0);
  dry(c);

  /* EAX's environment presets as they are. */
  for (i = 0; i < 26; i++) {
    char fmt[48];
    sprintf(fmt, "env_%%02g_%s", kEnvNames[i]);
    add("env", 1, 0, i, EC_STIM_IMPULSE | EC_STIM_SWEEP, fmt, i, 0);
  }

  /* EAX 1.0's own controls. */
  {
    static const double vol[] = {0.0, 0.25, 1.0}, dec[] = {0.5, 5.0}, damp[] = {0.2, 2.0};
    for (i = 0; i < N(vol); i++)
      lattr(add("eax1", 1, 1, 0, EC_STIM_SWEEP, "eax1_volume_%g", vol[i], 0),
            "EAX effect volume", 'f', vol[i]);
    for (i = 0; i < N(dec); i++)
      lattr(add("eax1", 1, 1, 0, EC_STIM_SWEEP, "eax1_decay_%g", dec[i], 0),
            "EAX decay time", 'f', dec[i]);
    for (i = 0; i < N(damp); i++)
      lattr(add("eax1", 1, 1, 0, EC_STIM_SWEEP, "eax1_damping_%g", damp[i], 0),
            "EAX damping", 'f', damp[i]);
  }

  /* One listener property at a time from GENERIC. */
  {
    static const double room[] = {-10000, -3000, 0}, roomhf[] = {-3000, -1000, 0},
                        roomlf[] = {-3000, -1000}, decay[] = {0.3, 1.0, 3.0, 10.0},
                        decayhf[] = {0.2, 0.5, 1.0, 2.0}, decaylf[] = {0.3, 2.0},
                        refl[] = {-10000, -2000, 0, 1000}, refldelay[] = {0, 0.05, 0.2},
                        reverb[] = {-10000, -1000, 1000, 2000}, revdelay[] = {0, 0.05, 0.1},
                        size[] = {1, 3, 15, 50, 100}, diff[] = {0, 0.5},
                        echo[] = {0.5, 1.0}, mod[] = {0.5, 1.0};
    add("listener", 2, 0, 0, EC_STIM_SWEEP, "listener_base", 0, 0);
    sweep_l("room", "EAX2 room", 'i', 2, room, N(room));
    sweep_l("roomhf", "EAX2 room HF", 'i', 2, roomhf, N(roomhf));
    sweep_l("roomlf", "EAX3 room LF", 'i', 3, roomlf, N(roomlf));
    sweep_l("decay", "EAX decay time", 'f', 2, decay, N(decay));
    sweep_l("decayhf", "EAX2 decay HF ratio", 'f', 2, decayhf, N(decayhf));
    sweep_l("decaylf", "EAX3 decay LF ratio", 'f', 3, decaylf, N(decaylf));
    sweep_l("refl", "EAX2 reflections", 'i', 2, refl, N(refl));
    sweep_l("refldelay", "EAX2 reflections delay", 'f', 2, refldelay, N(refldelay));
    sweep_l("reverb", "EAX2 reverb", 'i', 2, reverb, N(reverb));
    sweep_l("revdelay", "EAX2 reverb delay", 'f', 2, revdelay, N(revdelay));
    sweep_l("envsize", "EAX2 environment size", 'f', 2, size, N(size));
    sweep_l("diffusion", "EAX2 environment diffusion", 'f', 2, diff, N(diff));
    sweep_l("echodepth", "EAX3 echo depth", 'f', 3, echo, N(echo));
    sweep_l("moddepth", "EAX3 modulation depth", 'f', 3, mod, N(mod));
  }

  /* Source properties. Direct-path ones are measured dry (the recording is
     then the direct path alone); room-path ones in GENERIC. */
  {
    static const double obst[] = {-1000, -3000, -6000, -10000}, occl[] = {-1000, -3000, -6000},
                        excl[] = {-1000, -3000};
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "src_dry_ref", 0, 0);
    dry(c);
    for (i = 0; i < N(obst); i++) {
      c = add("source", 2, 0, 0, EC_STIM_SWEEP, "obst_%g", obst[i], 0);
      dry(c);
      sattr(c, "EAX2 sample obstruction", 'i', obst[i]);
    }
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "obst_%g_lf%g", -3000, 0.5);
    dry(c);
    sattr(c, "EAX2 sample obstruction", 'i', -3000);
    sattr(c, "EAX2 sample obstruction LF ratio", 'f', 0.5);
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "obst_%g_lf%g", -3000, 1.0);
    dry(c);
    sattr(c, "EAX2 sample obstruction", 'i', -3000);
    sattr(c, "EAX2 sample obstruction LF ratio", 'f', 1.0);
    for (i = 0; i < N(occl); i++) {
      c = add("source", 2, 0, 0, EC_STIM_SWEEP, "occl_dry_%g", occl[i], 0);
      dry(c);
      sattr(c, "EAX2 sample occlusion", 'i', occl[i]);
    }
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "occl_dry_%g_lf%g", -3000, 0.75);
    dry(c);
    sattr(c, "EAX2 sample occlusion", 'i', -3000);
    sattr(c, "EAX2 sample occlusion LF ratio", 'f', 0.75);
    c = add("source", 3, 0, 0, EC_STIM_SWEEP, "occl_dry_%g_dr%g", -3000, 0.5);
    dry(c);
    sattr(c, "EAX2 sample occlusion", 'i', -3000);
    sattr(c, "EAX3 sample occlusion direct ratio", 'f', 0.5);
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "src_dry_direct_%g", -1000, 0);
    dry(c);
    sattr(c, "EAX2 sample direct", 'i', -1000);
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "src_dry_directhf_%g", -1000, 0);
    dry(c);
    sattr(c, "EAX2 sample direct HF", 'i', -1000);

    add("source", 2, 0, 0, EC_STIM_SWEEP, "src_wet_ref", 0, 0);
    sattr(add("source", 2, 0, 0, EC_STIM_SWEEP, "occl_wet_%g", -3000, 0),
          "EAX2 sample occlusion", 'i', -3000);
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "occl_wet_%g_rr%g", -3000, 0.5);
    sattr(c, "EAX2 sample occlusion", 'i', -3000);
    sattr(c, "EAX2 sample occlusion room ratio", 'f', 0.5);
    c = add("source", 2, 0, 0, EC_STIM_SWEEP, "occl_wet_%g_rr%g", -3000, 1.5);
    sattr(c, "EAX2 sample occlusion", 'i', -3000);
    sattr(c, "EAX2 sample occlusion room ratio", 'f', 1.5);
    for (i = 0; i < N(excl); i++)
      sattr(add("source", 3, 0, 0, EC_STIM_SWEEP, "excl_%g", excl[i], 0),
            "EAX3 sample exclusion", 'i', excl[i]);
    c = add("source", 3, 0, 0, EC_STIM_SWEEP, "excl_%g_lf%g", -3000, 0.5);
    sattr(c, "EAX3 sample exclusion", 'i', -3000);
    sattr(c, "EAX3 sample exclusion LF ratio", 'f', 0.5);
    sattr(add("source", 2, 0, 0, EC_STIM_SWEEP, "src_wet_room_%g", -1000, 0),
          "EAX2 sample room", 'i', -1000);
    sattr(add("source", 2, 0, 0, EC_STIM_SWEEP, "src_wet_roomhf_%g", -1000, 0),
          "EAX2 sample room HF", 'i', -1000);
  }

  /* Distance: air absorption and room rolloff, in GENERIC. */
  {
    static const double d[] = {1, 2, 4, 8, 16, 32}, d3[] = {1, 4, 16};
    for (i = 0; i < N(d); i++)
      add("distance", 2, 0, 0, EC_STIM_SWEEP, "dist_%g", d[i], 0)->pos[2] = (float)d[i];
    for (i = 0; i < N(d3); i++) {
      c = add("distance", 2, 0, 0, EC_STIM_SWEEP, "dist_air100_%g", d3[i], 0);
      c->pos[2] = (float)d3[i];
      lattr(c, "EAX2 air absorption", 'f', -100);
    }
    for (i = 0; i < N(d3); i++) {
      c = add("distance", 2, 0, 0, EC_STIM_SWEEP, "dist_rolloff1_%g", d3[i], 0);
      c->pos[2] = (float)d3[i];
      lattr(c, "EAX2 room rolloff", 'f', 1.0);
    }
  }

  /* Panning and distance attenuation without reverb, on every provider
     (the Fast 2D baseline). Azimuth in degrees, positive to the right. */
  {
    static const double az[] = {-90, -45, 0, 45, 90, 135, 180}, d[] = {1, 2, 4, 8};
    for (i = 0; i < N(az); i++) {
      double r = az[i] * 3.14159265358979 / 180.0;
      c = add("position", 0, 0, 0, EC_STIM_NOISE, "pan_az%g", az[i], 0);
      dry(c);
      c->pos[0] = (float)sin(r);
      c->pos[2] = (float)cos(r);
      if (fabs(c->pos[0]) < 1e-6) c->pos[0] = 0.0f;
      if (fabs(c->pos[2]) < 1e-6) c->pos[2] = 0.0f;
    }
    for (i = 0; i < N(d); i++) {
      c = add("position", 0, 0, 0, EC_STIM_NOISE, "dist_dry_%g", d[i], 0);
      dry(c);
      c->pos[2] = (float)d[i];
    }
  }
}

int ec_case_count(void) {
  if (g_ncases < 0) build();
  return g_ncases;
}

const EcCase *ec_case(int i) {
  if (i < 0 || i >= ec_case_count()) return 0;
  return &g_cases[i];
}

double ec_tail(double decay_time, double reflections_delay, double reverb_delay) {
  /* 1.5 decay times take a full-scale tail 90 dB down, below 16 bits. */
  double t = 1.5 * decay_time + reflections_delay + reverb_delay + 0.3;
  if (!(t >= 1.0)) t = 1.0;
  if (t > EC_MAX_TAIL) t = EC_MAX_TAIL;
  return t;
}

double ec_decay_estimate(const EcCase *c) {
  int env = c->environment >= 0 && c->environment < 26 ? c->environment : 0;
  double decay = kEnvDecay[env];
  int i;
  for (i = 0; i < c->nl; i++) {
    if (!strcmp(c->l[i].name, "EAX decay time")) decay = c->l[i].value;
    /* EAX scales the decay time with the environment size. */
    if (!strcmp(c->l[i].name, "EAX2 environment size"))
      decay *= c->l[i].value / kEnvSize[env];
  }
  if (decay > 20.0) decay = 20.0;
  return decay;
}

int ec_case_dry(const EcCase *c, int eax) {
  int i;
  for (i = 0; i < c->nl; i++) {
    const EcAttr *a = &c->l[i];
    if (!ec_attr_applies(a->name, eax)) continue;
    if (!strcmp(a->name, "EAX2 room") && a->value <= -10000) return 1;
    if (!strcmp(a->name, "EAX effect volume") && a->value <= 0) return 1;
  }
  return 0;
}

/* ---- stimuli ----------------------------------------------------------- */

static void fade(float *x, int n, int in, int out) {
  int i;
  for (i = 0; i < in && i < n; i++) x[i] *= (float)(0.5 - 0.5 * cos(3.14159265358979 * i / in));
  for (i = 0; i < out && i < n; i++)
    x[n - 1 - i] *= (float)(0.5 - 0.5 * cos(3.14159265358979 * i / out));
}

int ec_stimulus(unsigned stim, int rate, float **out) {
  int n, i;
  float *x;
  double pi = 3.14159265358979;
  switch (stim) {
    case EC_STIM_IMPULSE: n = rate / 20; break;
    case EC_STIM_SWEEP: n = 2 * rate; break;
    case EC_STIM_NOISE: n = rate; break;
    case EC_STIM_TONE: n = rate; break;
    default: return 0;
  }
  x = (float *)calloc((size_t)n, sizeof(float));
  if (!x) return 0;
  if (stim == EC_STIM_IMPULSE) {
    x[0] = 0.5f;
  } else if (stim == EC_STIM_SWEEP) {
    /* Exponential sweep (Farina): f(t) = f1 * exp(t / T * ln(f2 / f1)). */
    double f1 = 20.0, f2 = 0.45 * rate < 20000.0 ? 0.45 * rate : 20000.0;
    double T = (double)n / rate, k = log(f2 / f1);
    for (i = 0; i < n; i++) {
      double t = (double)i / rate;
      x[i] = (float)(0.5 * sin(2 * pi * f1 * T / k * (exp(t / T * k) - 1.0)));
    }
    fade(x, n, rate / 50, rate / 200);
  } else if (stim == EC_STIM_NOISE) {
    /* Pink: Paul Kellet's filter on a fixed-seed white noise. */
    unsigned long seed = 12345;
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0, g;
    for (i = 0; i < n; i++) {
      double w;
      seed = (seed * 1103515245ul + 12345ul) & 0xFFFFFFFFul;
      w = ((double)(seed >> 8) / 8388608.0) - 1.0;
      b0 = 0.99886 * b0 + w * 0.0555179;
      b1 = 0.99332 * b1 + w * 0.0750759;
      b2 = 0.96900 * b2 + w * 0.1538520;
      b3 = 0.86650 * b3 + w * 0.3104856;
      b4 = 0.55000 * b4 + w * 0.5329522;
      b5 = -0.7616 * b5 - w * 0.0168980;
      x[i] = (float)(b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
      b6 = w * 0.115926;
      sum += (double)x[i] * x[i];
    }
    g = 0.17783 / sqrt(sum / n); /* -15 dBFS RMS */
    for (i = 0; i < n; i++) {
      double v = x[i] * g;
      x[i] = (float)(v > 0.95 ? 0.95 : v < -0.95 ? -0.95 : v);
    }
    fade(x, n, rate / 200, rate / 200);
  } else {
    for (i = 0; i < n; i++) x[i] = (float)(0.5 * sin(2 * pi * 1000.0 * i / rate));
    fade(x, n, rate / 200, rate / 200);
  }
  *out = x;
  return n;
}

static const struct {
  unsigned bit;
  const char *name;
} kStimNames[] = {
    {EC_STIM_IMPULSE, "impulse"},
    {EC_STIM_SWEEP, "sweep"},
    {EC_STIM_NOISE, "noise"},
    {EC_STIM_TONE, "tone"},
};

const char *ec_stim_name(unsigned stim) {
  int i;
  for (i = 0; i < N(kStimNames); i++)
    if (kStimNames[i].bit == stim) return kStimNames[i].name;
  return "?";
}

unsigned ec_stim_parse(const char *list) {
  unsigned bits = 0;
  while (*list) {
    const char *e = strchr(list, ',');
    size_t len = e ? (size_t)(e - list) : strlen(list);
    int i, found = 0;
    for (i = 0; i < N(kStimNames); i++)
      if (strlen(kStimNames[i].name) == len && !strncmp(kStimNames[i].name, list, len)) {
        bits |= kStimNames[i].bit;
        found = 1;
      }
    if (!found && len) return 0;
    list += len + (e ? 1 : 0);
  }
  return bits;
}

/* ---- names ------------------------------------------------------------- */

static int glob(const char *p, const char *pe, const char *s) {
  for (; p < pe; p++, s++) {
    if (*p == '*') {
      for (;; s++) {
        if (glob(p + 1, pe, s)) return 1;
        if (!*s) return 0;
      }
    }
    if (*p != *s) return 0;
  }
  return !*s;
}

int ec_match(const char *filter, const char *name) {
  if (!filter || !*filter) return 1;
  while (*filter) {
    const char *e = strchr(filter, ',');
    const char *pe = e ? e : filter + strlen(filter);
    if (pe > filter && glob(filter, pe, name)) return 1;
    filter = e ? e + 1 : pe;
  }
  return 0;
}

int ec_provider_eax(const char *p) {
  const char *e = strstr(p, "EAX");
  if (!e) return 0;
  if (e[3] == ' ' && e[4] >= '2' && e[4] <= '9') return e[4] - '0';
  return 1;
}

void ec_provider_tag(const char *p, char *tag, int size) {
  static const struct {
    const char *name, *tag;
  } known[] = {
      {"Miles Fast 2D Positional Audio", "fast2d"},
      {"DirectSound3D Hardware Support", "ds3dhw"},
      {"DirectSound3D Software Emulation", "ds3dsw"},
      {"DirectSound3D 7+ Software - Full HRTF", "dx7full"},
      {"DirectSound3D 7+ Software - Light HRTF", "dx7light"},
      {"DirectSound3D 7+ Software - Pan and Volume", "dx7pan"},
      {"Aureal A3D Interactive (TM)", "a3d"},
      {"Aureal A3D 2.0 (TM)", "a3d2"},
      {"RAD Game Tools RSX 3D Audio", "rsx"},
      {"Dolby Surround", "dolby"},
  };
  int i, n = 0;
  for (i = 0; i < N(known); i++)
    if (!strcmp(p, known[i].name)) {
      strncpy(tag, known[i].tag, (size_t)size - 1);
      tag[size - 1] = 0;
      return;
    }
  if (ec_provider_eax(p) && strstr(p, "Creative")) {
    sprintf(tag, "eax%d", ec_provider_eax(p));
    return;
  }
  for (; *p && n < size - 1 && n < 24; p++) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) tag[n++] = ch;
  }
  tag[n] = 0;
}

/* ---- WAV --------------------------------------------------------------- */

static void put16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *p, unsigned long v) {
  put16(p, (unsigned)(v & 0xFFFF));
  put16(p + 2, (unsigned)(v >> 16));
}

void ec_wav16_header(unsigned char *w, long frames, int channels, int rate) {
  unsigned long data = (unsigned long)frames * channels * 2;
  memcpy(w, "RIFF", 4);
  put32(w + 4, 36 + data);
  memcpy(w + 8, "WAVEfmt ", 8);
  put32(w + 16, 16);
  put16(w + 20, 1);
  put16(w + 22, (unsigned)channels);
  put32(w + 24, (unsigned long)rate);
  put32(w + 28, (unsigned long)rate * channels * 2);
  put16(w + 32, (unsigned)channels * 2);
  put16(w + 34, 16);
  memcpy(w + 36, "data", 4);
  put32(w + 40, data);
}

unsigned char *ec_wav16(const float *x, int frames, int channels, int rate, unsigned *size) {
  unsigned long data = (unsigned long)frames * channels * 2;
  unsigned char *w = (unsigned char *)malloc(44 + data);
  unsigned long i;
  if (!w) return 0;
  ec_wav16_header(w, frames, channels, rate);
  for (i = 0; i < (unsigned long)frames * channels; i++) {
    double v = x[i] * 32768.0;
    long s = (long)floor(v + 0.5);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    put16(w + 44 + 2 * i, (unsigned)(s & 0xFFFF));
  }
  *size = (unsigned)(44 + data);
  return w;
}
