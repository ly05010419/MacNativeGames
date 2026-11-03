/* eaxcap: records what a sound card makes of Miles' 3D providers (Creative
   EAX 1-3 on the card's hardware, Fast 2D, DirectSound3D) for comparison
   with x86recomp's EAX renderer. It drives the game-shipped mss32.dll and
   its plug-ins through the case list in cases.c, plays each stimulus as a
   3D sample and records the card's output ("What U Hear") with waveIn into
   a WAV file and a JSON manifest per case. See README.md.

   Win32, ANSI, Windows 98 SE or later: only kernel32, user32, winmm,
   dsound and the original msvcrt.dll, no C runtime startup code (the entry
   point is mainCRTStartup below; check_imports.py enforces this). */

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cases.h"

#define TOOL_VERSION "eaxcap 1"

typedef unsigned int U32;
typedef int S32;
typedef void *HSAMP; /* H3DSAMPLE, H3DPOBJECT */

/* ---- console and log ----------------------------------------------------- */

static HANDLE g_stdout = INVALID_HANDLE_VALUE, g_log = INVALID_HANDLE_VALUE;

static void out(const char *fmt, ...) {
  static char buf[4096];
  DWORD n;
  int len;
  va_list ap;
  va_start(ap, fmt);
  len = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
  va_end(ap);
  if (len < 0) len = (int)sizeof buf - 1;
  buf[len] = 0;
  if (g_stdout != INVALID_HANDLE_VALUE) WriteFile(g_stdout, buf, (DWORD)len, &n, NULL);
  if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, (DWORD)len, &n, NULL);
}

/* sprintf for paths: truncates instead of overflowing. */
static void path_fmt(char *dst, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf(dst, size - 1, fmt, ap);
  va_end(ap);
  dst[size - 1] = 0;
}

static int write_file(const char *path, const void *data, unsigned size) {
  DWORD n = 0;
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    out("error: cannot write %s (error %lu)\n", path, (unsigned long)GetLastError());
    return 0;
  }
  WriteFile(h, data, size, &n, NULL);
  CloseHandle(h);
  if (n != size) out("error: short write to %s\n", path);
  return n == size;
}

/* Reads up to `max` bytes of a file into a malloc'd buffer. */
static char *read_file(const char *path, unsigned max, unsigned *size) {
  DWORD n = 0;
  char *buf;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  buf = (char *)malloc(max + 1);
  if (buf) {
    ReadFile(h, buf, max, &n, NULL);
    buf[n] = 0;
  }
  CloseHandle(h);
  *size = n;
  return buf;
}

static void make_dirs(const char *path) {
  char p[MAX_PATH];
  size_t i;
  strncpy(p, path, sizeof p - 1);
  p[sizeof p - 1] = 0;
  for (i = 1; p[i]; i++) {
    if (p[i] == '\\' || p[i] == '/') {
      char ch = p[i];
      if (p[i - 1] == ':') continue;
      p[i] = 0;
      CreateDirectoryA(p, NULL);
      p[i] = ch;
    }
  }
  CreateDirectoryA(p, NULL);
}

/* ---- JSON ------------------------------------------------------------------ */

typedef struct {
  char *p;
  size_t len, cap;
  int need_comma;
} Json;

static void jraw(Json *j, const char *s) {
  size_t n = strlen(s);
  if (j->len + n + 1 > j->cap) {
    size_t cap = j->cap ? j->cap * 2 : 4096;
    while (cap < j->len + n + 1) cap *= 2;
    j->p = (char *)realloc(j->p, cap);
    j->cap = cap;
  }
  memcpy(j->p + j->len, s, n + 1);
  j->len += n;
}

static void jsep(Json *j) {
  if (j->need_comma) jraw(j, ",\n");
  j->need_comma = 1;
}

static void jstr_raw(Json *j, const char *s) {
  char e[8];
  jraw(j, "\"");
  for (; s && *s; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '"' || ch == '\\') {
      e[0] = '\\';
      e[1] = (char)ch;
      e[2] = 0;
    } else if (ch < 0x20 || ch >= 0x7F) {
      sprintf(e, "\\u%04x", ch);
    } else {
      e[0] = (char)ch;
      e[1] = 0;
    }
    jraw(j, e);
  }
  jraw(j, "\"");
}

static void jkey(Json *j, const char *k) {
  jsep(j);
  jstr_raw(j, k);
  jraw(j, ": ");
}

static void jopen(Json *j, const char *k, const char *bracket) {
  if (k)
    jkey(j, k);
  else
    jsep(j);
  jraw(j, bracket);
  j->need_comma = 0;
}

static void jclose(Json *j, const char *bracket) {
  jraw(j, bracket);
  j->need_comma = 1;
}

static void jstr(Json *j, const char *k, const char *v) {
  jkey(j, k);
  jstr_raw(j, v);
}

/* A number as JSON (NaN and infinities, which msvcrt prints as -1.#QNAN
   and the like, as null). */
static void fnum(char *b, double v) {
  if (v == v && v - v == 0)
    sprintf(b, "%.9g", v);
  else
    strcpy(b, "null");
}

static void jnum(Json *j, const char *k, double v) {
  char b[40];
  jkey(j, k);
  fnum(b, v);
  jraw(j, b);
}

static void jnull(Json *j, const char *k) {
  jkey(j, k);
  jraw(j, "null");
}

static void jvec(Json *j, const char *k, const float *v) {
  char b[100];
  jkey(j, k);
  sprintf(b, "[%.9g, %.9g, %.9g]", v[0], v[1], v[2]);
  jraw(j, b);
}

/* ---- Miles ------------------------------------------------------------------ */

#define MSS __stdcall

static struct {
  HMODULE dll;
  char path[MAX_PATH];
  char version[64];
  S32(MSS *startup)(void);
  void(MSS *shutdown)(void);
  char *(MSS *set_redist_directory)(const char *dir);
  char *(MSS *last_error)(void);
  void *(MSS *open_digital_driver)(U32 rate, S32 bits, S32 channels, U32 flags);
  void(MSS *close_digital_driver)(void *dig);
  void(MSS *set_DirectSound_HWND)(void *dig, HWND wnd);
  S32(MSS *enumerate_3D_providers)(U32 *next, U32 *dest, char **name);
  S32(MSS *open_3D_provider)(U32 lib);
  void(MSS *close_3D_provider)(U32 lib);
  HSAMP(MSS *open_3D_listener)(U32 lib);
  void(MSS *close_3D_listener)(HSAMP obj);
  void(MSS *set_3D_position)(HSAMP obj, float x, float y, float z);
  void(MSS *set_3D_orientation)(HSAMP obj, float fx, float fy, float fz, float ux, float uy,
                                float uz);
  HSAMP(MSS *allocate_3D_sample_handle)(U32 lib);
  void(MSS *release_3D_sample_handle)(HSAMP s);
  S32(MSS *set_3D_sample_file)(HSAMP s, const void *image);
  void(MSS *set_3D_sample_distances)(HSAMP s, float max_dist, float min_dist);
  void(MSS *set_3D_sample_loop_count)(HSAMP s, U32 loops);
  void(MSS *start_3D_sample)(HSAMP s);
  void(MSS *end_3D_sample)(HSAMP s);
  U32(MSS *sample_status_3D)(HSAMP s);
  void(MSS *set_3D_room_type)(U32 lib, S32 room);
  void(MSS *provider_attribute_3D)(U32 lib, const char *name, void *value);
  S32(MSS *set_3D_provider_preference)(U32 lib, const char *name, const void *value);
  S32(MSS *set_3D_sample_preference)(HSAMP s, const char *name, const void *value);
  /* Optional. */
  void(MSS *sample_attribute_3D)(HSAMP s, const char *name, void *value);
  S32(MSS *enumerate_3D_provider_attributes)(U32 lib, U32 *next, void *entry);
  S32(MSS *enumerate_3D_sample_attributes)(U32 lib, U32 *next, void *entry);
  void(MSS *lock)(void);
  void(MSS *unlock)(void);
} ail;

static FARPROC miles_proc(const char *name, int required, int *ok) {
  FARPROC f = GetProcAddress(ail.dll, name);
  if (!f && required) {
    out("error: %s has no export %s\n", ail.path, name);
    *ok = 0;
  }
  return f;
}

#define REQ(field, name) *(FARPROC *)&ail.field = miles_proc(name, 1, &ok)
#define OPT(field, name) *(FARPROC *)&ail.field = miles_proc(name, 0, &ok)

static int load_miles(const char *dll) {
  int ok = 1;
  unsigned size;
  char *img;
  ail.dll = LoadLibraryA(dll);
  if (!ail.dll) {
    out("error: cannot load %s (error %lu). Copy the game's mss32.dll next to eaxcap.exe.\n",
        dll, (unsigned long)GetLastError());
    return 0;
  }
  GetModuleFileNameA(ail.dll, ail.path, sizeof ail.path);
  REQ(startup, "_AIL_startup@0");
  REQ(shutdown, "_AIL_shutdown@0");
  REQ(set_redist_directory, "_AIL_set_redist_directory@4");
  REQ(last_error, "_AIL_last_error@0");
  REQ(open_digital_driver, "_AIL_open_digital_driver@16");
  REQ(close_digital_driver, "_AIL_close_digital_driver@4");
  REQ(set_DirectSound_HWND, "_AIL_set_DirectSound_HWND@8");
  REQ(enumerate_3D_providers, "_AIL_enumerate_3D_providers@12");
  REQ(open_3D_provider, "_AIL_open_3D_provider@4");
  REQ(close_3D_provider, "_AIL_close_3D_provider@4");
  REQ(open_3D_listener, "_AIL_open_3D_listener@4");
  REQ(close_3D_listener, "_AIL_close_3D_listener@4");
  REQ(set_3D_position, "_AIL_set_3D_position@16");
  REQ(set_3D_orientation, "_AIL_set_3D_orientation@28");
  REQ(allocate_3D_sample_handle, "_AIL_allocate_3D_sample_handle@4");
  REQ(release_3D_sample_handle, "_AIL_release_3D_sample_handle@4");
  REQ(set_3D_sample_file, "_AIL_set_3D_sample_file@8");
  REQ(set_3D_sample_distances, "_AIL_set_3D_sample_distances@12");
  REQ(set_3D_sample_loop_count, "_AIL_set_3D_sample_loop_count@8");
  REQ(start_3D_sample, "_AIL_start_3D_sample@4");
  REQ(end_3D_sample, "_AIL_end_3D_sample@4");
  REQ(sample_status_3D, "_AIL_3D_sample_status@4");
  REQ(set_3D_room_type, "_AIL_set_3D_room_type@8");
  REQ(provider_attribute_3D, "_AIL_3D_provider_attribute@12");
  REQ(set_3D_provider_preference, "_AIL_set_3D_provider_preference@12");
  REQ(set_3D_sample_preference, "_AIL_set_3D_sample_preference@12");
  OPT(sample_attribute_3D, "_AIL_3D_sample_attribute@12");
  OPT(enumerate_3D_provider_attributes, "_AIL_enumerate_3D_provider_attributes@12");
  OPT(enumerate_3D_sample_attributes, "_AIL_enumerate_3D_sample_attributes@12");
  OPT(lock, "_AIL_lock@0");
  OPT(unlock, "_AIL_unlock@0");
  /* The version string in the DLL ("Version 6.5e"), as mss32.cpp reads it. */
  strcpy(ail.version, "unknown");
  img = read_file(ail.path, 1u << 20, &size);
  if (img) {
    unsigned i;
    for (i = 0; i + 12 < size; i++)
      if (!memcmp(img + i, "Version 6.", 10)) {
        unsigned k = 8; /* "Version " and then up to the next space */
        memcpy(ail.version, img + i, 8);
        while (k < sizeof ail.version - 1 && i + k < size && img[i + k] > ' ')
          ail.version[k] = img[i + k], k++;
        ail.version[k] = 0;
        break;
      }
    free(img);
  }
  return ok;
}

static const char *miles_error(void) {
  const char *e = ail.last_error ? ail.last_error() : NULL;
  return e && *e ? e : "(no error text)";
}

/* RIB_INTERFACE_ENTRY: type, name, token, subtype. */
static void log_attributes(Json *j, const char *key, U32 lib,
                           S32(MSS *en)(U32, U32 *, void *)) {
  U32 next = 0, entry[8];
  int n = 0;
  jopen(j, key, "[");
  if (en) {
    memset(entry, 0, sizeof entry);
    while (n < 256 && en(lib, &next, entry)) {
      const char *name = (const char *)(size_t)entry[1];
      char b[64];
      if (!name || IsBadStringPtrA(name, 256)) break;
      jopen(j, NULL, "{");
      jstr(j, "name", name);
      jnum(j, "type", entry[0]);
      sprintf(b, "0x%08X", entry[3]);
      jstr(j, "subtype", b);
      jclose(j, "}");
      out("      %-40s type %u subtype %s\n", name, entry[0], b);
      memset(entry, 0, sizeof entry);
      n++;
    }
  }
  jclose(j, "]");
}

/* ---- options ----------------------------------------------------------------- */

static struct {
  const char *providers;
  const char *cases;
  const char *outdir;
  const char *miles_dir;
  unsigned stimuli; /* 0: each case's own */
  int rate;
  int wavein;
  int list, yes, dry_run;
} opt;

static char g_cmdline[1024];
static char g_exe_dir[MAX_PATH];

static void usage(void) {
  out("usage: eaxcap [options]\n"
      "  --list              list 3D providers, recording devices and cases, then exit\n"
      "  --provider P[,P..]  providers to record: eax1 eax2 eax3 fast2d ds3dhw ds3dsw\n"
      "                      dx7full dx7light dx7pan a3d dolby, a full provider name, or all\n"
      "                      (default eax2,eax3)\n"
      "  --rate HZ           44100 or 48000 (default 48000, the Audigy's native rate)\n"
      "  --cases LIST        comma-separated case names, * wildcards (default all)\n"
      "  --stim LIST         impulse,sweep,noise,tone instead of each case's own set\n"
      "  --out DIR           output directory (default capture)\n"
      "  --in N              waveIn device number from --list (default: the mapper)\n"
      "  --miles DIR         Miles plug-in directory (default: miles next to the exe)\n"
      "  --dry-run           print the plan without opening any device\n"
      "  --yes               do not wait for Enter before recording\n");
}

static int parse_args(int argc, char **argv) {
  int i;
  opt.providers = "eax2,eax3";
  opt.outdir = "capture";
  opt.rate = 48000;
  opt.wavein = -1;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && (i++, 1))
    if (!strcmp(a, "--list")) {
      opt.list = 1;
    } else if (!strcmp(a, "--yes")) {
      opt.yes = 1;
    } else if (!strcmp(a, "--dry-run")) {
      opt.dry_run = 1;
    } else if (ARG("--provider")) {
      opt.providers = v;
    } else if (ARG("--rate")) {
      opt.rate = atoi(v);
    } else if (ARG("--cases")) {
      opt.cases = v;
    } else if (ARG("--stim")) {
      opt.stimuli = ec_stim_parse(v);
      if (!opt.stimuli) {
        out("error: unknown stimulus in '%s'\n", v);
        return 0;
      }
    } else if (ARG("--out")) {
      opt.outdir = v;
    } else if (ARG("--in")) {
      opt.wavein = atoi(v);
    } else if (ARG("--miles")) {
      opt.miles_dir = v;
    } else {
      usage();
      return 0;
    }
#undef ARG
  }
  if (opt.rate != 44100 && opt.rate != 48000) {
    out("error: --rate must be 44100 or 48000\n");
    return 0;
  }
  return 1;
}

/* ---- system information --------------------------------------------------------- */

static OSVERSIONINFOA g_os;
static char g_os_text[256];

static void get_os(void) {
  memset(&g_os, 0, sizeof g_os);
  g_os.dwOSVersionInfoSize = sizeof g_os;
  GetVersionExA(&g_os);
  sprintf(g_os_text, "%s %lu.%lu build %lu %s",
          g_os.dwPlatformId == VER_PLATFORM_WIN32_NT ? "Windows NT" : "Windows 9x",
          (unsigned long)g_os.dwMajorVersion, (unsigned long)g_os.dwMinorVersion,
          (unsigned long)(g_os.dwBuildNumber & 0xFFFF), g_os.szCSDVersion);
}

static void json_os(Json *j) {
  jopen(j, "os", "{");
  jnum(j, "platform", g_os.dwPlatformId);
  jnum(j, "major", g_os.dwMajorVersion);
  jnum(j, "minor", g_os.dwMinorVersion);
  jnum(j, "build", g_os.dwBuildNumber & 0xFFFF);
  jstr(j, "csd", g_os.szCSDVersion);
  jstr(j, "text", g_os_text);
  jclose(j, "}");
}

/* DirectSound devices: description and driver module, for every manifest. */
#define MAX_DS 8
static char g_ds_desc[MAX_DS][128], g_ds_module[MAX_DS][128];
static int g_nds;

static BOOL CALLBACK ds_enum(LPGUID guid, LPCSTR desc, LPCSTR module, LPVOID ctx) {
  (void)guid;
  (void)ctx;
  out("  DirectSound: %s [%s]\n", desc ? desc : "", module ? module : "");
  if (g_nds < MAX_DS) {
    strncpy(g_ds_desc[g_nds], desc ? desc : "", sizeof g_ds_desc[0] - 1);
    strncpy(g_ds_module[g_nds], module ? module : "", sizeof g_ds_module[0] - 1);
    g_nds++;
  }
  return TRUE;
}

static void json_ds(Json *j) {
  int i;
  jopen(j, "directsound_devices", "[");
  for (i = 0; i < g_nds; i++) {
    jopen(j, NULL, "{");
    jstr(j, "description", g_ds_desc[i]);
    jstr(j, "module", g_ds_module[i]);
    jclose(j, "}");
  }
  jclose(j, "]");
}

static void list_devices(Json *j) {
  UINT i, n = waveInGetNumDevs();
  g_nds = 0;
  DirectSoundEnumerateA(ds_enum, NULL);
  if (j) json_ds(j);
  if (j) jopen(j, "wavein_devices", "[");
  for (i = 0; i < n; i++) {
    WAVEINCAPSA caps;
    memset(&caps, 0, sizeof caps);
    waveInGetDevCapsA(i, &caps, sizeof caps);
    out("  waveIn %u: %s (formats 0x%08lX)\n", i, caps.szPname, (unsigned long)caps.dwFormats);
    if (j) {
      jopen(j, NULL, "{");
      jnum(j, "index", i);
      jstr(j, "name", caps.szPname);
      jnum(j, "formats", caps.dwFormats);
      jclose(j, "}");
    }
  }
  if (j) jclose(j, "]");
}

/* ---- window (DirectSound needs one with the focus) ------------------------------ */

static HWND g_wnd, g_status;
static volatile int g_quit, g_key;

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CLOSE:
      g_quit = 1;
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_RETURN || wp == VK_SPACE) g_key = 1;
      return 0;
    case WM_SYSCOMMAND:
      /* No screen saver or monitor power-down while recording. */
      if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER) return 0;
      break;
  }
  return DefWindowProcA(w, m, wp, lp);
}

static void pump(void) {
  MSG m;
  while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
}

static void wait_ms(DWORD ms) {
  DWORD t0 = GetTickCount();
  while (GetTickCount() - t0 < ms && !g_quit) {
    pump();
    Sleep(10);
  }
}

/* The line of text in the window. */
static void status(const char *text) {
  if (g_status) SetWindowTextA(g_status, text);
  pump();
}

static void make_window(void) {
  WNDCLASSA wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = "eaxcap";
  RegisterClassA(&wc);
  g_wnd = CreateWindowExA(0, "eaxcap", "eaxcap", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                          CW_USEDEFAULT, CW_USEDEFAULT, 520, 120, NULL, NULL, wc.hInstance, NULL);
  g_status = CreateWindowExA(0, "STATIC", "Keep this window in front while recording.",
                             WS_CHILD | WS_VISIBLE, 10, 10, 490, 64, g_wnd, NULL, wc.hInstance,
                             NULL);
  ShowWindow(g_wnd, SW_SHOWNORMAL);
  UpdateWindow(g_wnd);
  SetForegroundWindow(g_wnd);
  pump();
}

/* Waits until the window has the focus again: DirectSound mutes programs
   without it. */
static void wait_focus(void) {
  if (GetForegroundWindow() == g_wnd || g_quit) return;
  out("  PAUSED: the eaxcap window lost the focus; click it to go on.\n");
  status("PAUSED: click this window to go on recording.");
  while (GetForegroundWindow() != g_wnd && !g_quit) {
    pump();
    Sleep(50);
  }
  status("Recording. Keep this window in front.");
  wait_ms(500); /* DirectSound unmutes */
}

/* Waits for Enter or Space in the window (the console would block the
   window's messages). */
static void wait_key(const char *text) {
  out("%s\n", text);
  status(text);
  g_key = 0;
  while (!g_key && !g_quit) {
    pump();
    Sleep(20);
  }
  status("Recording. Keep this window in front.");
}

/* ---- recording -------------------------------------------------------------------- */

#define NBUF 16
#define BUF_FRAMES 4096

typedef struct {
  HWAVEIN h;
  WAVEHDR hdr[NBUF];
  short *mem;
  short *data; /* captured frames, interleaved stereo */
  long frames, cap;
} Rec;

static int rec_open(Rec *r) {
  WAVEFORMATEX f;
  MMRESULT e;
  int i;
  memset(r, 0, sizeof *r);
  memset(&f, 0, sizeof f);
  f.wFormatTag = WAVE_FORMAT_PCM;
  f.nChannels = 2;
  f.nSamplesPerSec = (DWORD)opt.rate;
  f.wBitsPerSample = 16;
  f.nBlockAlign = 4;
  f.nAvgBytesPerSec = (DWORD)opt.rate * 4;
  e = waveInOpen(&r->h, opt.wavein < 0 ? WAVE_MAPPER : (UINT)opt.wavein, &f, 0, 0, CALLBACK_NULL);
  if (e != MMSYSERR_NOERROR) {
    char t[256];
    waveInGetErrorTextA(e, t, sizeof t);
    out("error: waveInOpen %d Hz 16-bit stereo: %s\n", opt.rate, t);
    return 0;
  }
  r->mem = (short *)malloc((size_t)NBUF * BUF_FRAMES * 4);
  for (i = 0; i < NBUF; i++) {
    WAVEHDR *h = &r->hdr[i];
    memset(h, 0, sizeof *h);
    h->lpData = (LPSTR)(r->mem + (size_t)i * BUF_FRAMES * 2);
    h->dwBufferLength = BUF_FRAMES * 4;
    waveInPrepareHeader(r->h, h, sizeof *h);
    waveInAddBuffer(r->h, h, sizeof *h);
  }
  return 1;
}

/* Moves finished buffers into r->data and requeues them, in order.
   Returns how many were waiting (all of them: the input may have
   overrun). */
static int g_next_buf;
static char g_wavein[160];

static int rec_poll(Rec *r) {
  int waiting = 0;
  for (;;) {
    WAVEHDR *h = &r->hdr[g_next_buf];
    long n;
    if (!(h->dwFlags & WHDR_DONE) || waiting == NBUF) return waiting;
    waiting++;
    n = (long)(h->dwBytesRecorded / 4);
    if (r->frames + n > r->cap) {
      r->cap = (r->cap + n) * 2 + 65536;
      r->data = (short *)realloc(r->data, (size_t)r->cap * 4);
    }
    memcpy(r->data + r->frames * 2, h->lpData, (size_t)n * 4);
    r->frames += n;
    h->dwFlags &= ~(DWORD)WHDR_DONE;
    h->dwBytesRecorded = 0;
    waveInAddBuffer(r->h, h, sizeof *h);
    g_next_buf = (g_next_buf + 1) % NBUF;
  }
}

static void rec_close(Rec *r);

/* The device waveIn really records from (the mapper picks one). False if
   it cannot record our format. */
static int probe_wavein(void) {
  Rec r;
  UINT id = 0;
  WAVEINCAPSA caps;
  if (!rec_open(&r)) return 0;
  memset(&caps, 0, sizeof caps);
  /* A mapper handle may not tell its device; with one device it is that. */
  if (waveInGetID(r.h, &id) != MMSYSERR_NOERROR || id >= waveInGetNumDevs())
    id = waveInGetNumDevs() == 1 ? 0 : (UINT)-1;
  if (id != (UINT)-1 && waveInGetDevCapsA(id, &caps, sizeof caps) == MMSYSERR_NOERROR)
    sprintf(g_wavein, "%s (device %u%s)", caps.szPname, id,
            opt.wavein < 0 ? ", chosen by the wave mapper" : "");
  else
    strcpy(g_wavein, "the wave mapper's preferred recording device (--in N names one)");
  rec_close(&r);
  return 1;
}

static void rec_close(Rec *r) {
  int i;
  waveInReset(r->h);
  for (i = 0; i < NBUF; i++) waveInUnprepareHeader(r->h, &r->hdr[i], sizeof r->hdr[i]);
  waveInClose(r->h);
  free(r->mem);
  r->mem = NULL;
}

/* The sync click: the first sample after `start` above the noise of the
   0.2 s before it (as analyze.py finds it); -1 if none. */
static long find_sync(const short *x, long frames, long start, int rate, double *peak_db) {
  long i, n0 = start - rate / 5, peak = 0, noise = 0, thr;
  if (n0 < 0) n0 = 0;
  for (i = 0; i < frames * 2; i++) {
    long a = labs((long)x[i]);
    if (a > peak) peak = a;
    if (i >= n0 * 2 && i < start * 2 && a > noise) noise = a;
  }
  *peak_db = peak ? 20.0 * log10(peak / 32768.0) : -999.0;
  thr = noise * 8;
  if (thr < 33) thr = 33; /* -60 dBFS */
  for (i = start * 2; i < frames * 2; i++)
    if (labs((long)x[i]) > thr) return i / 2;
  return -1;
}

/* ---- one recording ----------------------------------------------------------------- */

typedef struct {
  U32 lib;
  const char *name;
  char tag[32];
  int eax;
  HSAMP listener;
} Prov;

typedef struct {
  unsigned bit;
  float *x; /* stimulus without the lead */
  int frames;
  unsigned char *image; /* WAV image: lead silence + stimulus */
  unsigned image_size;
} Stim;

static Stim g_stims[4];
static unsigned char *g_sync_image;

static void set_value(char type, double v, U32 *buf) {
  if (type == 'f') {
    float f = (float)v;
    memcpy(buf, &f, 4);
  } else if (type == 'u') {
    buf[0] = (U32)v;
  } else {
    S32 i = (S32)v;
    memcpy(buf, &i, 4);
  }
}

/* Sets attributes (listener: s == NULL) and logs each as applied or not. */
static void apply_attrs(Json *j, const char *key, const Prov *p, HSAMP s, const EcAttr *a,
                        int n) {
  int i;
  jopen(j, key, "[");
  for (i = 0; i < n; i++) {
    U32 buf[4];
    jopen(j, NULL, "{");
    jstr(j, "name", a[i].name);
    jnum(j, "value", a[i].value);
    if (ec_attr_applies(a[i].name, p->eax)) {
      S32 r;
      set_value(a[i].type, a[i].value, buf);
      r = s ? ail.set_3D_sample_preference(s, a[i].name, buf)
            : ail.set_3D_provider_preference(p->lib, a[i].name, buf);
      jnum(j, "result", r);
    } else {
      jstr(j, "result", "skipped");
    }
    jclose(j, "}");
  }
  jclose(j, "]");
}

#define SENTINEL 0x7F7F7F7Fu
#define MAX_NAMES 32
#define NENV 26

/* An attribute's value as read back (or intended), up to three words. */
typedef struct {
  U32 v[4];
  int ok;
} Val;

/* Reads attribute `name` (listener: s == NULL). Miles 6.5 stores -1 before
   the lookup, which is what a name the provider lacks leaves; our own
   sentinel covers Miles versions that do not. */
static int get_attr(const Prov *p, HSAMP s, const char *name, U32 *v) {
  v[0] = v[1] = v[2] = v[3] = SENTINEL;
  if (s) {
    if (!ail.sample_attribute_3D) return 0;
    ail.sample_attribute_3D(s, name, v);
  } else {
    ail.provider_attribute_3D(p->lib, name, v);
  }
  return v[0] != SENTINEL && v[0] != 0xFFFFFFFFu;
}

static void val_text(char type, const U32 *v, char *b) {
  float f[3];
  memcpy(f, v, sizeof f);
  if (type == 'i') {
    sprintf(b, "%d", (S32)v[0]);
  } else if (type == 'u') {
    sprintf(b, "%u", v[0]);
  } else if (type == 'v') {
    char x[40], y[40], z[40];
    fnum(x, f[0]);
    fnum(y, f[1]);
    fnum(z, f[2]);
    sprintf(b, "[%s, %s, %s]", x, y, z);
  } else {
    fnum(b, f[0]);
  }
}

/* Reads back the attributes this provider version has (listener: s ==
   NULL) into got[] and the manifest. */
static void read_back(Json *j, const char *key, const Prov *p, HSAMP s, const EcAttrName *names,
                      Val *got) {
  int i;
  jopen(j, key, "{");
  for (i = 0; names[i].name; i++) {
    char b[140];
    got[i].ok = 0;
    if (!ec_attr_applies(names[i].name, p->eax)) continue;
    if (s && !ail.sample_attribute_3D) continue;
    got[i].ok = get_attr(p, s, names[i].name, got[i].v);
    if (!got[i].ok) {
      jnull(j, names[i].name);
      continue;
    }
    val_text(names[i].type, got[i].v, b);
    jkey(j, names[i].name);
    jraw(j, b);
  }
  jclose(j, "}");
}

static int name_index(const EcAttrName *names, const char *name) {
  int i;
  for (i = 0; names[i].name; i++)
    if (!strcmp(names[i].name, name)) return i;
  return -1;
}

/* What each attribute should read back: `base` (NULL: unknown) with the
   applicable ones of `set` on top. */
static void intend(const EcAttrName *names, const Val *base, const EcAttr *set, int n, int eax,
                   Val *want) {
  int i;
  for (i = 0; names[i].name; i++) {
    if (!base)
      want[i].ok = 0;
    else if (base != want)
      want[i] = base[i];
  }
  for (i = 0; i < n; i++) {
    int k = name_index(names, set[i].name);
    if (k < 0 || !ec_attr_applies(set[i].name, eax)) continue;
    set_value(set[i].type, set[i].value, want[k].v);
    want[k].ok = 1;
  }
}

static int same(char type, const U32 *a, const U32 *b) {
  int i, n = type == 'v' ? 3 : 1;
  if (type == 'i') return labs((long)(S32)a[0] - (long)(S32)b[0]) <= 1;
  if (type == 'u') return a[0] == b[0];
  for (i = 0; i < n; i++) {
    float x, y;
    memcpy(&x, &a[i], 4);
    memcpy(&y, &b[i], 4);
    if (!(fabs(x - y) <= 1e-4 + 5e-3 * fabs(y))) return 0;
  }
  return 1;
}

/* Logs (and prints) every attribute whose read-back differs from what the
   case intends. Returns the count. */
static int compare(Json *j, const char *case_name, const char *what, const EcAttrName *names,
                   const Val *want, const Val *got) {
  int i, n = 0;
  for (i = 0; names[i].name; i++) {
    char a[140], b[140];
    if (!want[i].ok || !got[i].ok || same(names[i].type, want[i].v, got[i].v)) continue;
    val_text(names[i].type, want[i].v, a);
    val_text(names[i].type, got[i].v, b);
    out("  MISMATCH %s: %s %s intended %s, read %s\n", case_name, what, names[i].name, a, b);
    jopen(j, NULL, "{");
    jstr(j, "what", what);
    jstr(j, "name", names[i].name);
    jkey(j, "intended");
    jraw(j, a);
    jkey(j, "readback");
    jraw(j, b);
    jclose(j, "}");
    n++;
  }
  return n;
}

static float val_float(const EcAttrName *names, const Val *got, const char *name, float dflt) {
  int k = name_index(names, name);
  float f;
  if (k < 0 || !got[k].ok) return dflt;
  memcpy(&f, got[k].v, 4);
  return f == f ? f : dflt;
}

/* Each environment preset as this provider reports it right after being
   selected: its "EAX all parameters" block (written back before every
   case, so no property of an earlier case carries over) and every single
   attribute (what a case's read-back is checked against). */
static unsigned char g_block[NENV][128];
static int g_block_ok[NENV];
static Val g_preset[NENV][MAX_NAMES];

static unsigned block_size(int eax) { return eax == 1 ? 16 : eax == 2 ? 56 : 112; }

/* Selects environment `env` by way of another one: selecting the current
   environment again need not reload its preset. */
static S32 select_env(const Prov *p, int env, int *via_room_type) {
  S32 r = 0;
  int pass;
  *via_room_type = 0;
  for (pass = 0; pass < 2; pass++) {
    U32 e = (U32)(pass ? env : (env + 1) % NENV), got[4];
    r = ail.set_3D_provider_preference(p->lib, "EAX environment selection", &e);
    if (!get_attr(p, NULL, "EAX environment selection", got) || got[0] != e) {
      /* The preference did not take: Miles' room type does the same. */
      ail.set_3D_room_type(p->lib, (S32)e);
      *via_room_type = 1;
    }
  }
  return r;
}

static void capture_presets(const Prov *p, Json *run) {
  static unsigned char buf[512];
  unsigned size = block_size(p->eax), k;
  int env, i, via;
  out("  reading the %d environment presets\n", NENV);
  jopen(run, "presets", "[");
  for (env = 0; env < NENV; env++) {
    char hex[2 * 128 + 1];
    select_env(p, env, &via);
    memset(buf, 0x7F, sizeof buf);
    ail.provider_attribute_3D(p->lib, "EAX all parameters", buf);
    g_block_ok[env] = buf[0] != 0x7F && memcmp(buf, "\xff\xff\xff\xff", 4) != 0;
    memcpy(g_block[env], buf, sizeof g_block[env]);
    for (i = 0; ec_listener_names[i].name; i++)
      g_preset[env][i].ok = ec_attr_applies(ec_listener_names[i].name, p->eax) &&
                            get_attr(p, NULL, ec_listener_names[i].name, g_preset[env][i].v);
    for (k = 0; k < size; k++) sprintf(hex + 2 * k, "%02x", buf[k]);
    jopen(run, NULL, "{");
    jnum(run, "environment", env);
    if (g_block_ok[env])
      jstr(run, "all_parameters_hex", hex);
    else
      jnull(run, "all_parameters_hex");
    jclose(run, "}");
    pump();
  }
  jclose(run, "]");
  if (!g_block_ok[0])
    out("  note: the provider does not report \"EAX all parameters\"; cases start from "
        "the selected environment only\n");
}

/* "EAX all parameters" as raw bytes in the provider's own layout. */
static void read_all_hex(Json *j, const Prov *p) {
  static unsigned char buf[512];
  char hex[2 * 112 + 1];
  unsigned size = block_size(p->eax), i;
  memset(buf, 0x7F, sizeof buf);
  ail.provider_attribute_3D(p->lib, "EAX all parameters", buf);
  for (i = 0; i < size; i++) sprintf(hex + 2 * i, "%02x", buf[i]);
  jstr(j, "listener_all_parameters_hex", hex);
}

enum { REC_FAIL, REC_OK, REC_RETRY, REC_NOSYNC };

static int record_case(const Prov *p, const EcCase *c, const Stim *st, const char *dir,
                       int *clipped) {
  Json j;
  Rec r;
  HSAMP smp, sync;
  long total, preroll = (long)(EC_PREROLL * opt.rate), lead = (long)(EC_LEAD * opt.rate);
  long started_at = -1, sync_at, max_lag = 0;
  double decay = -1, rdelay = 0.3, vdelay = 0.1, tail, peak_db;
  DWORD t_start = 0, t_rec0 = 0;
  char path[MAX_PATH];
  int ok = 1, dry = ec_case_dry(c, p->eax), focus_lost = 0, overrun = 0, mismatches = 0, i;
  int env = p->eax && c->environment >= 0 && c->environment < NENV ? c->environment : -1;
  const EcAttr *defs;
  int ndefs = ec_source_defaults(p->eax, &defs);
  EcAttr sync_attr;
  Val lgot[MAX_NAMES], lwant[MAX_NAMES], sgot[MAX_NAMES], swant[MAX_NAMES];
  Val ygot[MAX_NAMES], ywant[MAX_NAMES];
  unsigned char *wav;

  memset(&j, 0, sizeof j);
  memset(&r, 0, sizeof r);
  jopen(&j, NULL, "{");
  jstr(&j, "tool", TOOL_VERSION);
  jstr(&j, "case", c->name);
  jstr(&j, "group", c->group);
  jstr(&j, "stimulus", ec_stim_name(st->bit));
  jstr(&j, "provider", p->name);
  jstr(&j, "provider_tag", p->tag);
  jnum(&j, "eax_version", p->eax);
  jstr(&j, "miles", ail.version);
  jnum(&j, "rate", opt.rate);

  /* Listener: the environment's full preset, then the case's properties. */
  if (env >= 0) {
    int via;
    jnum(&j, "environment", env);
    jnum(&j, "environment_result", select_env(p, env, &via));
    if (via) jstr(&j, "environment_via", "AIL_set_3D_room_type");
    if (g_block_ok[env])
      jnum(&j, "all_parameters_result",
           ail.set_3D_provider_preference(p->lib, "EAX all parameters", g_block[env]));
  } else {
    jnull(&j, "environment");
  }
  apply_attrs(&j, "listener_set", p, NULL, c->l, c->nl);

  /* Samples: the stimulus and the sync click, every EAX property set. */
  smp = ail.allocate_3D_sample_handle(p->lib);
  sync = ail.allocate_3D_sample_handle(p->lib);
  if (!smp || !sync) {
    out("error: cannot allocate 3D samples: %s\n", miles_error());
    if (smp) ail.release_3D_sample_handle(smp);
    if (sync) ail.release_3D_sample_handle(sync);
    free(j.p);
    return REC_FAIL;
  }
  if (!ail.set_3D_sample_file(smp, st->image) || !ail.set_3D_sample_file(sync, g_sync_image)) {
    out("error: AIL_set_3D_sample_file: %s\n", miles_error());
    ok = 0;
  }
  ail.set_3D_sample_loop_count(smp, 1);
  ail.set_3D_sample_loop_count(sync, 1);
  ail.set_3D_sample_distances(smp, c->max_dist, c->min_dist);
  ail.set_3D_sample_distances(sync, c->max_dist, c->min_dist);
  ail.set_3D_position(smp, c->pos[0], c->pos[1], c->pos[2]);
  ail.set_3D_position(sync, EC_SYNC_POS[0], EC_SYNC_POS[1], EC_SYNC_POS[2]);
  apply_attrs(&j, "source_defaults_set", p, smp, defs, ndefs);
  apply_attrs(&j, "source_set", p, smp, c->s, c->ns);
  /* The click: direct path only. */
  sync_attr.name = p->eax >= 2 ? "EAX2 sample room" : "EAX sample reverb mix";
  sync_attr.type = p->eax >= 2 ? 'i' : 'f';
  sync_attr.value = p->eax >= 2 ? -10000 : 0;
  jopen(&j, "sync", "{");
  jvec(&j, "pos", EC_SYNC_POS);
  apply_attrs(&j, "defaults_set", p, sync, defs, ndefs);
  apply_attrs(&j, "set", p, sync, &sync_attr, p->eax ? 1 : 0);
  jclose(&j, "}");

  if (p->eax) {
    read_back(&j, "listener_readback", p, NULL, ec_listener_names, lgot);
    read_all_hex(&j, p);
    read_back(&j, "source_readback", p, smp, ec_source_names, sgot);
    read_back(&j, "sync_readback", p, sync, ec_source_names, ygot);
    intend(ec_listener_names, env >= 0 ? g_preset[env] : NULL, c->l, c->nl, p->eax, lwant);
    for (i = 0; i < c->nl; i++)
      if (!strcmp(c->l[i].name, "EAX2 environment size")) {
        /* EAX rescales these with the size: no fixed expectation. */
        int k;
        for (k = 0; ec_listener_names[k].name; k++)
          if (ec_attr_size_scaled(ec_listener_names[k].name)) lwant[k].ok = 0;
      }
    intend(ec_source_names, NULL, defs, ndefs, p->eax, swant);
    intend(ec_source_names, swant, c->s, c->ns, p->eax, swant);
    intend(ec_source_names, NULL, defs, ndefs, p->eax, ywant);
    intend(ec_source_names, ywant, &sync_attr, 1, p->eax, ywant);
    jopen(&j, "state_mismatch", "[");
    mismatches = compare(&j, c->name, "listener", ec_listener_names, lwant, lgot) +
                 compare(&j, c->name, "source", ec_source_names, swant, sgot) +
                 compare(&j, c->name, "sync", ec_source_names, ywant, ygot);
    jclose(&j, "]");
    decay = val_float(ec_listener_names, lgot, "EAX decay time", -1);
    rdelay = val_float(ec_listener_names, lgot, "EAX2 reflections delay", 0.3f);
    vdelay = val_float(ec_listener_names, lgot, "EAX2 reverb delay", 0.1f);
  }
  if (!(decay > 0)) decay = p->eax ? ec_decay_estimate(c) : !strcmp(p->tag, "fast2d") ? 0.1 : 2.0;
  if (dry) decay = 0.1;
  tail = ec_tail(decay, rdelay, vdelay);
  total = preroll + lead + st->frames + (long)(tail * opt.rate);

  jvec(&j, "source_pos", c->pos);
  {
    static const float zero[3] = {0, 0, 0}, face[3] = {0, 0, 1}, up[3] = {0, 1, 0};
    jvec(&j, "listener_pos", zero);
    jvec(&j, "listener_face", face);
    jvec(&j, "listener_up", up);
  }
  jnum(&j, "min_dist", c->min_dist);
  jnum(&j, "max_dist", c->max_dist);
  jnum(&j, "preroll_s", EC_PREROLL);
  jnum(&j, "lead_s", EC_LEAD);
  jnum(&j, "stim_frames", st->frames);
  jnum(&j, "decay_for_tail_s", decay);
  jnum(&j, "tail_s", tail);
  jnum(&j, "record_frames", total);

  out("  %-28s %-7s %5.1f s ", c->name, ec_stim_name(st->bit), (double)total / opt.rate);

  /* Record: preroll, start both samples together, then the rest. Watch
     the focus and whether the input keeps up. */
  if (ok && rec_open(&r)) {
    g_next_buf = 0;
    waveInStart(r.h);
    t_rec0 = timeGetTime();
    while (r.frames < total && !g_quit) {
      long lag;
      if (rec_poll(&r) >= NBUF) overrun = 1;
      lag = (long)((double)(timeGetTime() - t_rec0) * opt.rate / 1000.0) - r.frames;
      if (lag > max_lag) max_lag = lag;
      if (lag > NBUF * BUF_FRAMES + opt.rate / 4) overrun = 1;
      if (GetForegroundWindow() != g_wnd) focus_lost = 1;
      if (started_at < 0 && r.frames >= preroll) {
        if (ail.lock) ail.lock();
        ail.start_3D_sample(sync);
        ail.start_3D_sample(smp);
        if (ail.unlock) ail.unlock();
        started_at = r.frames;
        t_start = GetTickCount();
      }
      pump();
      Sleep(5);
    }
    rec_close(&r);
    if (r.frames > total) r.frames = total;
  } else {
    ok = 0;
  }
  ail.end_3D_sample(smp);
  ail.end_3D_sample(sync);
  ail.release_3D_sample_handle(smp);
  ail.release_3D_sample_handle(sync);
  if (!ok || g_quit) {
    out("failed\n");
    free(r.data);
    free(j.p);
    return REC_FAIL;
  }

  sync_at = find_sync(r.data, r.frames, started_at, opt.rate, &peak_db);
  jnum(&j, "started_at_frame", started_at);
  jnum(&j, "start_tick_ms", t_start);
  if (sync_at >= 0)
    jnum(&j, "sync_frame_found", sync_at);
  else
    jnull(&j, "sync_frame_found");
  jnum(&j, "peak_dbfs", peak_db);
  jkey(&j, "focus_lost");
  jraw(&j, focus_lost ? "true" : "false");
  jkey(&j, "wavein_overrun");
  jraw(&j, overrun ? "true" : "false");
  jnum(&j, "max_input_lag_ms", 1000.0 * max_lag / opt.rate);
  jstr(&j, "wavein", g_wavein);
  json_os(&j);
  json_ds(&j);
  jclose(&j, "}");
  jraw(&j, "\n");

  if (sync_at >= 0)
    out("peak %6.1f dBFS  latency %6.1f ms", peak_db,
        1000.0 * (double)(sync_at - started_at) / opt.rate);
  else
    out("peak %6.1f dBFS  NO SYNC CLICK FOUND", peak_db);
  if (peak_db > -0.1) {
    out("  CLIPPED");
    *clipped = 1;
  }
  if (mismatches) out("  STATE MISMATCH");
  if (focus_lost) out("  FOCUS LOST");
  if (overrun) out("  INPUT OVERRUN");
  out("\n");

  path_fmt(path, sizeof path, "%s\\%s.%s.json", dir, c->name, ec_stim_name(st->bit));
  write_file(path, j.p, (unsigned)j.len);
  path_fmt(path, sizeof path, "%s\\%s.%s.wav", dir, c->name, ec_stim_name(st->bit));
  /* 16-bit stereo as recorded. */
  wav = (unsigned char *)malloc(44 + (size_t)r.frames * 4);
  if (wav) {
    ec_wav16_header(wav, r.frames, 2, opt.rate);
    memcpy(wav + 44, r.data, (size_t)r.frames * 4);
    write_file(path, wav, (unsigned)(44 + r.frames * 4));
    free(wav);
  }
  free(r.data);
  free(j.p);
  if (focus_lost || overrun) return REC_RETRY;
  return sync_at < 0 ? REC_NOSYNC : REC_OK;
}

/* ---- providers -------------------------------------------------------------------- */

#define MAX_PROV 32
static Prov g_prov[MAX_PROV];
static int g_nprov;

static void enumerate_providers(void) {
  U32 next = 0, lib;
  char *name;
  g_nprov = 0;
  while (g_nprov < MAX_PROV && ail.enumerate_3D_providers(&next, &lib, &name)) {
    Prov *p = &g_prov[g_nprov++];
    memset(p, 0, sizeof *p);
    p->lib = lib;
    p->name = name ? name : "";
    p->eax = ec_provider_eax(p->name);
    ec_provider_tag(p->name, p->tag, sizeof p->tag);
  }
}

static int provider_selected(const Prov *p) {
  const char *s = opt.providers;
  if (!strcmp(s, "all")) return 1;
  while (*s) {
    const char *e = strchr(s, ',');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if ((strlen(p->tag) == n && !strncmp(p->tag, s, n)) ||
        (strlen(p->name) == n && !strncmp(p->name, s, n)))
      return 1;
    s += n + (e ? 1 : 0);
  }
  return 0;
}

static int build_stimuli(void) {
  int i;
  float *x;
  int n;
  unsigned size;
  for (i = 0; i < 4; i++) {
    Stim *s = &g_stims[i];
    float *padded;
    long lead = (long)(EC_LEAD * opt.rate);
    s->bit = 1u << i;
    s->frames = ec_stimulus(s->bit, opt.rate, &s->x);
    if (!s->frames) return 0;
    padded = (float *)calloc((size_t)(lead + s->frames), sizeof(float));
    memcpy(padded + lead, s->x, (size_t)s->frames * sizeof(float));
    s->image = ec_wav16(padded, (int)(lead + s->frames), 1, opt.rate, &s->image_size);
    free(padded);
  }
  n = ec_stimulus(EC_STIM_IMPULSE, opt.rate, &x);
  if (!n) return 0;
  g_sync_image = ec_wav16(x, n, 1, opt.rate, &size);
  free(x);
  return g_sync_image != NULL;
}

static void write_stimuli(const char *dir) {
  int i;
  char path[MAX_PATH];
  for (i = 0; i < 4; i++) {
    unsigned size;
    unsigned char *w = ec_wav16(g_stims[i].x, g_stims[i].frames, 1, opt.rate, &size);
    path_fmt(path, sizeof path, "%s\\stim_%s.wav", dir, ec_stim_name(g_stims[i].bit));
    if (w) write_file(path, w, size);
    free(w);
  }
}

static int count_recordings(const Prov *p) {
  int i, k, n = 0;
  for (i = 0; i < ec_case_count(); i++) {
    const EcCase *c = ec_case(i);
    unsigned stim = opt.stimuli ? opt.stimuli : c->stimuli;
    if (!ec_case_applies(c, p->eax) || !ec_match(opt.cases, c->name)) continue;
    for (k = 0; k < 4; k++)
      if (stim & (1u << k)) n++;
  }
  return n;
}

/* Adds a case name to a comma-separated list once. */
static void add_name(char *list, size_t size, const char *name) {
  const char *at = list;
  size_t n = strlen(name);
  while ((at = strstr(at, name)) != NULL) {
    if ((at == list || at[-1] == ',') && (at[n] == ',' || !at[n])) return;
    at += n;
  }
  if (strlen(list) + n + 2 > size) return;
  if (*list) strcat(list, ",");
  strcat(list, name);
}

static void run_provider(Prov *p, Json *run) {
  static char failed[4096];
  char dir[MAX_PATH];
  int i, k, done = 0, total = count_recordings(p), clipped = 0, nosync = 0;
  S32 r;
  DWORD t0 = GetTickCount();

  out("\n== %s (%s), %d recordings\n", p->name, p->tag, total);
  if (!total) return;
  r = ail.open_3D_provider(p->lib);
  jopen(run, NULL, "{");
  jstr(run, "provider", p->name);
  jstr(run, "tag", p->tag);
  jnum(run, "open_result", r);
  if (r != 0) {
    out("  cannot open: %s\n", miles_error());
    jstr(run, "error", miles_error());
    jclose(run, "}");
    return;
  }
  out("  provider attributes:\n");
  log_attributes(run, "provider_attributes", p->lib, ail.enumerate_3D_provider_attributes);
  out("  sample attributes:\n");
  log_attributes(run, "sample_attributes", p->lib, ail.enumerate_3D_sample_attributes);
  p->listener = ail.open_3D_listener(p->lib);
  if (p->listener) {
    ail.set_3D_position(p->listener, 0, 0, 0);
    ail.set_3D_orientation(p->listener, 0, 0, 1, 0, 1, 0);
  }
  memset(g_block_ok, 0, sizeof g_block_ok);
  if (p->eax) capture_presets(p, run);

  path_fmt(dir, sizeof dir, "%s\\%s", opt.outdir, p->tag);
  make_dirs(dir);
  write_stimuli(dir);

  failed[0] = 0;
  for (i = 0; i < ec_case_count() && !g_quit; i++) {
    const EcCase *c = ec_case(i);
    unsigned stim = opt.stimuli ? opt.stimuli : c->stimuli;
    if (!ec_case_applies(c, p->eax) || !ec_match(opt.cases, c->name)) continue;
    for (k = 0; k < 4 && !g_quit; k++) {
      char title[128];
      int rc = REC_FAIL, attempt;
      if (!(stim & (1u << k))) continue;
      sprintf(title, "eaxcap %s %d/%d", p->tag, done + 1, total);
      SetWindowTextA(g_wnd, title);
      for (attempt = 0;; attempt++) {
        wait_focus();
        if (g_quit) break;
        rc = record_case(p, c, &g_stims[k], dir, &clipped);
        if (rc == REC_RETRY && attempt < 2) {
          out("  recording it again\n");
          continue;
        }
        if (rc == REC_NOSYNC && ++nosync >= 2) {
          nosync = 0;
          wait_key("Two recordings in a row had no sync click: check the recording source "
                   "and level, then press Enter or Space in the eaxcap window.");
          if (attempt == 0) continue;
        } else if (rc != REC_NOSYNC) {
          nosync = 0;
        }
        break;
      }
      if (g_quit) break;
      if (rc != REC_OK) add_name(failed, sizeof failed, c->name);
      done++;
    }
  }
  jnum(run, "recordings", done);
  jnum(run, "seconds", (GetTickCount() - t0) / 1000.0);
  jstr(run, "failed_cases", failed);
  jclose(run, "}");
  if (clipped)
    out("  WARNING: some recordings clipped; lower the recording level and record again.\n");
  if (*failed)
    out("  Some recordings failed (no sync click, focus lost or input overrun). Again with:\n"
        "    eaxcap --provider %s --cases %s --out %s\n",
        p->tag, failed, opt.outdir);
  if (p->listener) ail.close_3D_listener(p->listener);
  ail.close_3D_provider(p->lib);
}

/* ---- main ---------------------------------------------------------------------------- */

/* The run manifest so far, with `closers` completing its JSON. */
static void write_run(const char *path, const Json *j, const char *closers) {
  size_t n = strlen(closers);
  char *buf = (char *)malloc(j->len + n);
  if (!buf) return;
  memcpy(buf, j->p, j->len);
  memcpy(buf + j->len, closers, n);
  write_file(path, buf, (unsigned)(j->len + n));
  free(buf);
}

static void instructions(void) {
  out("\nBefore recording:\n"
      "  - Control Panel > Multimedia (or the volume control, Options > Properties >\n"
      "    Recording): select \"What U Hear\" as the only recording source and set its\n"
      "    level so level_tone peaks around -12 dBFS (it is printed for every case).\n"
      "  - Creative's speaker settings: 2 speakers (or 2.1), CMSS / 3D upmix off, no\n"
      "    EAX console effects (Creative's mixer / Surround Mixer \"EAX\" tab: off).\n"
      "  - Close other programs that play sound. Keep the eaxcap window in front:\n"
      "    recording pauses while it is not, and a case that lost it is recorded again.\n");
}

static int run(int argc, char **argv) {
  void *dig;
  char redist[MAX_PATH], path[MAX_PATH];
  Json run;
  int i;

  get_os();
  if (!parse_args(argc, argv)) return 2;

  if (opt.dry_run) {
    int n = 0;
    out("%s: plan for providers %s at %d Hz into %s\n", TOOL_VERSION, opt.providers, opt.rate,
        opt.outdir);
    for (i = 0; i < ec_case_count(); i++) {
      const EcCase *c = ec_case(i);
      unsigned stim = opt.stimuli ? opt.stimuli : c->stimuli, k;
      if (!ec_match(opt.cases, c->name)) continue;
      double decay = ec_case_dry(c, c->max_eax ? c->max_eax : 3) ? 0.1 : ec_decay_estimate(c);
      if (!c->min_eax)
        out("  %-28s %-8s any provider   ", c->name, c->group);
      else if (c->max_eax)
        out("  %-28s %-8s EAX %d only     ", c->name, c->group, c->max_eax);
      else
        out("  %-28s %-8s EAX %d or later ", c->name, c->group, c->min_eax);
      for (k = 0; k < 4; k++)
        if (stim & (1u << k)) out(" %s", ec_stim_name(1u << k));
      out("  (tail about %.1f s)\n", ec_tail(decay, 0.3, 0.1));
      n++;
    }
    out("%d cases\n", n);
    return 0;
  }

  out("%s on %s\n", TOOL_VERSION, g_os_text);
  if (!load_miles("mss32.dll")) return 1;
  out("Miles: %s (%s)\n", ail.path, ail.version);
  if (opt.miles_dir)
    strncpy(redist, opt.miles_dir, sizeof redist - 1);
  else
    path_fmt(redist, sizeof redist, "%s\\miles", g_exe_dir);
  ail.set_redist_directory(redist);
  if (!ail.startup()) {
    out("error: AIL_startup failed: %s\n", miles_error());
    return 1;
  }
  make_window();
  dig = ail.open_digital_driver((U32)opt.rate, 16, 2, 0);
  if (!dig) {
    out("error: AIL_open_digital_driver(%d, 16, 2): %s\n", opt.rate, miles_error());
    ail.shutdown();
    return 1;
  }
  ail.set_DirectSound_HWND(dig, g_wnd);
  enumerate_providers();
  out("3D providers (plug-ins from %s):\n", redist);
  for (i = 0; i < g_nprov; i++)
    out("  %-10s %s%s\n", g_prov[i].tag, g_prov[i].name,
        provider_selected(&g_prov[i]) ? "  [selected]" : "");
  out("Devices:\n");

  memset(&run, 0, sizeof run);
  jopen(&run, NULL, "{");
  jstr(&run, "tool", TOOL_VERSION);
  jstr(&run, "command_line", g_cmdline);
  jstr(&run, "miles", ail.version);
  jstr(&run, "miles_path", ail.path);
  jstr(&run, "redist", redist);
  jnum(&run, "rate", opt.rate);
  json_os(&run);
  list_devices(&run);
  if (!probe_wavein()) {
    strcpy(g_wavein, "cannot record");
    if (!opt.list) {
      ail.close_digital_driver(dig);
      ail.shutdown();
      return 1;
    }
  }
  jstr(&run, "wavein", g_wavein);
  jopen(&run, "providers_enumerated", "[");
  for (i = 0; i < g_nprov; i++) {
    jsep(&run);
    jstr_raw(&run, g_prov[i].name);
  }
  jclose(&run, "]");

  if (opt.list) {
    out("Cases (%d):\n", ec_case_count());
    for (i = 0; i < ec_case_count(); i++)
      out("  %-28s %s\n", ec_case(i)->name, ec_case(i)->group);
    free(run.p);
    ail.close_digital_driver(dig);
    ail.shutdown();
    return 0;
  }

  out("Recording from waveIn: %s at %d Hz into %s\n", g_wavein, opt.rate, opt.outdir);
  instructions();
  if (!build_stimuli()) {
    out("error: out of memory\n");
    return 1;
  }
  /* The run manifest is written now and after every provider, so a run
     that is stopped still has one. */
  make_dirs(opt.outdir);
  path_fmt(path, sizeof path, "%s\\run_%lu.json", opt.outdir, (unsigned long)GetTickCount());
  jopen(&run, "runs", "[");
  write_run(path, &run, "]}\n");
  if (!opt.yes) {
    SetForegroundWindow(g_wnd);
    wait_key("Press Enter or Space in the eaxcap window to start (closing it stops).");
  }
  for (i = 0; i < g_nprov && !g_quit; i++)
    if (provider_selected(&g_prov[i])) {
      run_provider(&g_prov[i], &run);
      write_run(path, &run, "]}\n");
    }
  jclose(&run, "]");
  jclose(&run, "}");
  jraw(&run, "\n");
  write_file(path, run.p, (unsigned)run.len);
  free(run.p);

  ail.close_digital_driver(dig);
  ail.shutdown();
  out("\nDone%s. Recordings are in %s\n", g_quit ? " (stopped)" : "", opt.outdir);
  return g_quit ? 1 : 0;
}

/* No C runtime startup: split the command line ourselves. */
static int split_args(char *s, char **argv, int max) {
  int argc = 0;
  while (*s && argc < max) {
    char *d;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    argv[argc++] = d = s;
    if (*s == '"') {
      argv[argc - 1] = d = ++s;
      while (*s && *s != '"') s++;
    } else {
      while (*s && *s != ' ' && *s != '\t') s++;
    }
    if (*s) *s++ = 0;
  }
  return argc;
}

void __cdecl mainCRTStartup(void) {
  static char *argv[64];
  int argc, rc;
  char *slash, logpath[MAX_PATH];
  g_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
  strncpy(g_cmdline, GetCommandLineA(), sizeof g_cmdline - 1);
  {
    static char copy[1024];
    strcpy(copy, g_cmdline);
    argc = split_args(copy, argv, 64);
  }
  GetModuleFileNameA(NULL, g_exe_dir, sizeof g_exe_dir);
  slash = strrchr(g_exe_dir, '\\');
  if (slash) *slash = 0;
  timeBeginPeriod(1);
  /* A log of everything printed, next to the exe. */
  path_fmt(logpath, sizeof logpath, "%s\\eaxcap.log", g_exe_dir);
  g_log = CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
  if (g_log != INVALID_HANDLE_VALUE) SetFilePointer(g_log, 0, NULL, FILE_END);
  out("\n> %s\n", g_cmdline);
  rc = run(argc, argv);
  if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
  timeEndPeriod(1);
  ExitProcess((UINT)rc);
}
