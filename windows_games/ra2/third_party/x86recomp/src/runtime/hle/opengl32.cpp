// opengl32 (GL 1.1, wgl) and the gdi32 pixel-format/SwapBuffers calls,
// on gl::Context and gfx::Device (never the host's OpenGL: the same path
// serves the planned Metal backend). Handlers only marshal guest
// arguments; the GL semantics live in runtime/gl/context.cpp.
//
// Engines may GetProcAddress all 336 GL 1.1 exports up front,
// so the full export table is declared: an entry point without a handler
// is a fatal error naming it when it is first called. Extensions come
// from wglGetProcAddress and exist only for what GL_EXTENSIONS lists.
//
// One pixel format (32-bit colour without alpha, 24-bit depth, 8-bit
// stencil, double-buffered); one gfx::Device per window, created at the
// first wglMakeCurrent on it.

#include <SDL3/SDL.h>

#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/gfx/device.h"
#include "runtime/gl/context.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/present.h"
#include "runtime/hle/user32_window.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

Exports g_gl_exports{"opengl32.dll", {
    "glAccum", "glAlphaFunc", "glAreTexturesResident", "glArrayElement",
    "glBegin", "glBindTexture", "glBitmap", "glBlendFunc", "glCallList",
    "glCallLists", "glClear", "glClearAccum", "glClearColor", "glClearDepth",
    "glClearIndex", "glClearStencil", "glClipPlane", "glColor3b", "glColor3bv",
    "glColor3d", "glColor3dv", "glColor3f", "glColor3fv", "glColor3i",
    "glColor3iv", "glColor3s", "glColor3sv", "glColor3ub", "glColor3ubv",
    "glColor3ui", "glColor3uiv", "glColor3us", "glColor3usv", "glColor4b",
    "glColor4bv", "glColor4d", "glColor4dv", "glColor4f", "glColor4fv",
    "glColor4i", "glColor4iv", "glColor4s", "glColor4sv", "glColor4ub",
    "glColor4ubv", "glColor4ui", "glColor4uiv", "glColor4us", "glColor4usv",
    "glColorMask", "glColorMaterial", "glColorPointer", "glCopyPixels",
    "glCopyTexImage1D", "glCopyTexImage2D", "glCopyTexSubImage1D",
    "glCopyTexSubImage2D", "glCullFace", "glDeleteLists", "glDeleteTextures",
    "glDepthFunc", "glDepthMask", "glDepthRange", "glDisable",
    "glDisableClientState", "glDrawArrays", "glDrawBuffer", "glDrawElements",
    "glDrawPixels", "glEdgeFlag", "glEdgeFlagPointer", "glEdgeFlagv",
    "glEnable", "glEnableClientState", "glEnd", "glEndList", "glEvalCoord1d",
    "glEvalCoord1dv", "glEvalCoord1f", "glEvalCoord1fv", "glEvalCoord2d",
    "glEvalCoord2dv", "glEvalCoord2f", "glEvalCoord2fv", "glEvalMesh1",
    "glEvalMesh2", "glEvalPoint1", "glEvalPoint2", "glFeedbackBuffer",
    "glFinish", "glFlush", "glFogf", "glFogfv", "glFogi", "glFogiv",
    "glFrontFace", "glFrustum", "glGenLists", "glGenTextures", "glGetBooleanv",
    "glGetClipPlane", "glGetDoublev", "glGetError", "glGetFloatv",
    "glGetIntegerv", "glGetLightfv", "glGetLightiv", "glGetMapdv", "glGetMapfv",
    "glGetMapiv", "glGetMaterialfv", "glGetMaterialiv", "glGetPixelMapfv",
    "glGetPixelMapuiv", "glGetPixelMapusv", "glGetPointerv",
    "glGetPolygonStipple", "glGetString", "glGetTexEnvfv", "glGetTexEnviv",
    "glGetTexGendv", "glGetTexGenfv", "glGetTexGeniv", "glGetTexImage",
    "glGetTexLevelParameterfv", "glGetTexLevelParameteriv",
    "glGetTexParameterfv", "glGetTexParameteriv", "glHint", "glIndexMask",
    "glIndexPointer", "glIndexd", "glIndexdv", "glIndexf", "glIndexfv",
    "glIndexi", "glIndexiv", "glIndexs", "glIndexsv", "glIndexub", "glIndexubv",
    "glInitNames", "glInterleavedArrays", "glIsEnabled", "glIsList",
    "glIsTexture", "glLightModelf", "glLightModelfv", "glLightModeli",
    "glLightModeliv", "glLightf", "glLightfv", "glLighti", "glLightiv",
    "glLineStipple", "glLineWidth", "glListBase", "glLoadIdentity",
    "glLoadMatrixd", "glLoadMatrixf", "glLoadName", "glLogicOp", "glMap1d",
    "glMap1f", "glMap2d", "glMap2f", "glMapGrid1d", "glMapGrid1f",
    "glMapGrid2d", "glMapGrid2f", "glMaterialf", "glMaterialfv", "glMateriali",
    "glMaterialiv", "glMatrixMode", "glMultMatrixd", "glMultMatrixf",
    "glNewList", "glNormal3b", "glNormal3bv", "glNormal3d", "glNormal3dv",
    "glNormal3f", "glNormal3fv", "glNormal3i", "glNormal3iv", "glNormal3s",
    "glNormal3sv", "glNormalPointer", "glOrtho", "glPassThrough",
    "glPixelMapfv", "glPixelMapuiv", "glPixelMapusv", "glPixelStoref",
    "glPixelStorei", "glPixelTransferf", "glPixelTransferi", "glPixelZoom",
    "glPointSize", "glPolygonMode", "glPolygonOffset", "glPolygonStipple",
    "glPopAttrib", "glPopClientAttrib", "glPopMatrix", "glPopName",
    "glPrioritizeTextures", "glPushAttrib", "glPushClientAttrib",
    "glPushMatrix", "glPushName", "glRasterPos2d", "glRasterPos2dv",
    "glRasterPos2f", "glRasterPos2fv", "glRasterPos2i", "glRasterPos2iv",
    "glRasterPos2s", "glRasterPos2sv", "glRasterPos3d", "glRasterPos3dv",
    "glRasterPos3f", "glRasterPos3fv", "glRasterPos3i", "glRasterPos3iv",
    "glRasterPos3s", "glRasterPos3sv", "glRasterPos4d", "glRasterPos4dv",
    "glRasterPos4f", "glRasterPos4fv", "glRasterPos4i", "glRasterPos4iv",
    "glRasterPos4s", "glRasterPos4sv", "glReadBuffer", "glReadPixels",
    "glRectd", "glRectdv", "glRectf", "glRectfv", "glRecti", "glRectiv",
    "glRects", "glRectsv", "glRenderMode", "glRotated", "glRotatef", "glScaled",
    "glScalef", "glScissor", "glSelectBuffer", "glShadeModel", "glStencilFunc",
    "glStencilMask", "glStencilOp", "glTexCoord1d", "glTexCoord1dv",
    "glTexCoord1f", "glTexCoord1fv", "glTexCoord1i", "glTexCoord1iv",
    "glTexCoord1s", "glTexCoord1sv", "glTexCoord2d", "glTexCoord2dv",
    "glTexCoord2f", "glTexCoord2fv", "glTexCoord2i", "glTexCoord2iv",
    "glTexCoord2s", "glTexCoord2sv", "glTexCoord3d", "glTexCoord3dv",
    "glTexCoord3f", "glTexCoord3fv", "glTexCoord3i", "glTexCoord3iv",
    "glTexCoord3s", "glTexCoord3sv", "glTexCoord4d", "glTexCoord4dv",
    "glTexCoord4f", "glTexCoord4fv", "glTexCoord4i", "glTexCoord4iv",
    "glTexCoord4s", "glTexCoord4sv", "glTexCoordPointer", "glTexEnvf",
    "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexGend", "glTexGendv",
    "glTexGenf", "glTexGenfv", "glTexGeni", "glTexGeniv", "glTexImage1D",
    "glTexImage2D", "glTexParameterf", "glTexParameterfv", "glTexParameteri",
    "glTexParameteriv", "glTexSubImage1D", "glTexSubImage2D", "glTranslated",
    "glTranslatef", "glVertex2d", "glVertex2dv", "glVertex2f", "glVertex2fv",
    "glVertex2i", "glVertex2iv", "glVertex2s", "glVertex2sv", "glVertex3d",
    "glVertex3dv", "glVertex3f", "glVertex3fv", "glVertex3i", "glVertex3iv",
    "glVertex3s", "glVertex3sv", "glVertex4d", "glVertex4dv", "glVertex4f",
    "glVertex4fv", "glVertex4i", "glVertex4iv", "glVertex4s", "glVertex4sv",
    "glVertexPointer", "glViewport",
    "wglChoosePixelFormat", "wglCopyContext", "wglCreateContext",
    "wglCreateLayerContext", "wglDeleteContext", "wglDescribeLayerPlane",
    "wglDescribePixelFormat", "wglGetCurrentContext", "wglGetCurrentDC",
    "wglGetLayerPaletteEntries", "wglGetPixelFormat", "wglGetProcAddress",
    "wglMakeCurrent", "wglRealizeLayerPalette", "wglSetLayerPaletteEntries",
    "wglSetPixelFormat", "wglShareLists", "wglSwapBuffers",
    "wglSwapLayerBuffers", "wglUseFontBitmapsA", "wglUseFontBitmapsW",
    "wglUseFontOutlinesA", "wglUseFontOutlinesW",
}};

float F(const Ctx& c, int i) { return std::bit_cast<float>(uint32_t(Arg(c, i))); }
double D(const Ctx& c, int i) {
  return std::bit_cast<double>(uint64_t(Arg(c, i)) | uint64_t(Arg(c, i + 1)) << 32);
}
// A guest pointer argument as a host pointer (null stays null).
const void* P(const Ctx& c, int i) { return Arg(c, i) ? GPtr(Arg(c, i)) : nullptr; }
float Fv(uint32_t va, int i) { return std::bit_cast<float>(Rd32(va + 4 * uint32_t(i))); }

// ---- contexts ---------------------------------------------------------------

struct GlContext {
  uint32_t hwnd = 0;
  std::unique_ptr<gfx::Device> dev;
  std::unique_ptr<gl::Context> gl;
};

std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<GlContext>> g_contexts;  // by HGLRC
uint32_t g_next_hglrc = 0x00C00010;
thread_local GlContext* t_current = nullptr;
thread_local uint32_t t_current_hglrc = 0, t_current_dc = 0;

// The default framebuffer has the window's client size as the game sees
// it (present fits it into the host window).
void FramebufferSize(uint32_t hwnd, int* w, int* h) {
  ClientAreaSize(hwnd, w, h);
  *w = std::max(*w, 1);
  *h = std::max(*h, 1);
}

// The current GL; a GL call without one is a game bug on Windows too
// (it does nothing there), so it is reported and ignored.
gl::Context* Gl() {
  if (t_current && t_current->gl) return t_current->gl.get();
  static bool warned = false;
  if (!warned) WarnF("opengl32: GL call without a current context\n");
  warned = true;
  return nullptr;
}

#define GL_OR_RETURN(v)          \
  gl::Context* g = Gl();         \
  if (!g) return v

// ---- pixel formats (gdi32 and the wgl* duplicates) ------------------------------

uint32_t DescribePixelFormat(uint32_t index, uint32_t size, uint32_t pfd) {
  if (pfd && size >= 40 && index == 1) {
    std::memset(GPtr(pfd), 0, 40);
    Wr16(pfd + 0, 40);
    Wr16(pfd + 2, 1);
    Wr32(pfd + 4, 0x4 | 0x20 | 0x1);  // DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLEBUFFER
    Wr8(pfd + 8, 0);                  // PFD_TYPE_RGBA
    Wr8(pfd + 9, 32);                 // cColorBits
    Wr8(pfd + 10, 8);  Wr8(pfd + 11, 16);  // red bits, shift
    Wr8(pfd + 12, 8);  Wr8(pfd + 13, 8);   // green
    Wr8(pfd + 14, 8);  Wr8(pfd + 15, 0);   // blue
    Wr8(pfd + 23, 24);                // cDepthBits
    Wr8(pfd + 24, 8);                 // cStencilBits
  }
  return 1;  // the number of formats
}

HLE("gdi32.dll", ChoosePixelFormat, 2) { return 1; }
HLE("gdi32.dll", DescribePixelFormat, 4) {
  return DescribePixelFormat(Arg(c, 1), Arg(c, 2), Arg(c, 3));
}
HLE("gdi32.dll", SetPixelFormat, 3) { return Arg(c, 1) == 1 ? 1 : 0; }
HLE("gdi32.dll", GetPixelFormat, 1) { return 1; }
HLE("opengl32.dll", wglChoosePixelFormat, 2) { return 1; }
HLE("opengl32.dll", wglDescribePixelFormat, 4) {
  return DescribePixelFormat(Arg(c, 1), Arg(c, 2), Arg(c, 3));
}
HLE("opengl32.dll", wglSetPixelFormat, 3) { return Arg(c, 1) == 1 ? 1 : 0; }
HLE("opengl32.dll", wglGetPixelFormat, 1) { return 1; }

// No hardware gamma ramp: games fall back to their own brightness
// handling, as on drivers without one.
HLE("gdi32.dll", GetDeviceGammaRamp, 2) { return 0; }
HLE("gdi32.dll", SetDeviceGammaRamp, 2) { return 0; }

HLE("opengl32.dll", wglCreateContext, 1) {
  uint32_t hwnd = WindowForDc(Arg(c, 0));
  if (!hwnd || !SdlWindowFor(hwnd)) {
    SetLastError(2000);  // ERROR_INVALID_PIXEL_FORMAT
    return 0;
  }
  std::lock_guard<std::mutex> l(g_mu);
  auto ctx = std::make_unique<GlContext>();
  ctx->hwnd = hwnd;
  uint32_t h = g_next_hglrc;
  g_next_hglrc += 0x10;
  g_contexts[h] = std::move(ctx);
  return h;
}

HLE("opengl32.dll", wglMakeCurrent, 2) {
  uint32_t dc = Arg(c, 0), h = Arg(c, 1);
  if (!h) {
    t_current = nullptr;
    t_current_hglrc = t_current_dc = 0;
    return 1;
  }
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_contexts.find(h);
  if (it == g_contexts.end()) return 0;
  GlContext* ctx = it->second.get();
  if (!ctx->gl) {
    SDL_Window* w = SdlWindowFor(ctx->hwnd);
    if (!w) return 0;
    ctx->dev = CreateGfxDevice(w);
    int pw, ph;
    FramebufferSize(ctx->hwnd, &pw, &ph);
    ctx->dev->ResetSwapChain(pw, ph, false);
    ctx->gl = std::make_unique<gl::Context>(ctx->dev.get(), pw, ph);
  }
  t_current = ctx;
  t_current_hglrc = h;
  t_current_dc = dc;
  return 1;
}

HLE("opengl32.dll", wglDeleteContext, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_contexts.find(Arg(c, 0));
  if (it == g_contexts.end()) return 0;
  if (t_current == it->second.get()) {
    t_current = nullptr;
    t_current_hglrc = t_current_dc = 0;
  }
  it->second->gl.reset();
  it->second->dev.reset();
  g_contexts.erase(it);
  return 1;
}

HLE("opengl32.dll", wglGetCurrentContext, 0) { return t_current_hglrc; }
HLE("opengl32.dll", wglGetCurrentDC, 0) { return t_current_dc; }

// Presents the window's context, following the window's size.
uint32_t Swap(uint32_t dc) {
  uint32_t hwnd = WindowForDc(dc);
  GlContext* ctx = nullptr;
  {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [h, x] : g_contexts)
      if (x->hwnd == hwnd && x->gl) ctx = x.get();
  }
  if (!ctx) return 0;
  ctx->gl->EndFrame();
  PresentFrame(*ctx->dev, ctx->gl->Width(), ctx->gl->Height());
  int fw, fh;
  FramebufferSize(hwnd, &fw, &fh);
  ctx->gl->Resize(fw, fh);
  return 1;
}

HLE("gdi32.dll", SwapBuffers, 1) { return Swap(Arg(c, 0)); }
HLE("opengl32.dll", wglSwapBuffers, 1) { return Swap(Arg(c, 0)); }

// Extension entry points exist only for what GL_EXTENSIONS advertises.
HLE("opengl32.dll", wglGetProcAddress, 1) {
  static const char* const kExt[] = {
      "glActiveTextureARB", "glClientActiveTextureARB", "glMultiTexCoord2fARB",
      "glMultiTexCoord2fvARB", "glLockArraysEXT", "glUnlockArraysEXT",
      "glCompressedTexImage2DARB", "wglSwapIntervalEXT", "wglGetSwapIntervalEXT",
      "wglGetExtensionsStringEXT"};
  std::string name = GStr(Arg(c, 0));
  for (const char* e : kExt)
    if (name == e) return ThunkFor("opengl32.dll", name);
  RECOMP_TRACE("  wglGetProcAddress(%s): not provided\n", name.c_str());
  return 0;
}

HLE("opengl32.dll", wglGetExtensionsStringEXT, 0) {
  static uint32_t s = 0;
  if (!s) {
    const std::string e = "WGL_EXT_swap_control WGL_EXT_extensions_string";
    s = HeapAllocProcess(uint32_t(e.size() + 1), false);
    PutStr(s, e);
  }
  return s;
}

HLE("opengl32.dll", wglSwapIntervalEXT, 1) {
  GL_OR_RETURN(0);
  g->SetVsync(Arg(c, 0) != 0);
  return 1;
}

// ---- GL 1.1 -------------------------------------------------------------------

// Guest copies of the GL strings (the game keeps the pointers).
uint32_t GuestString(gl::Enum name, const char* s) {
  static std::map<gl::Enum, uint32_t> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  uint32_t va = HeapAllocProcess(uint32_t(std::strlen(s) + 1), false);
  PutStr(va, s);
  return cache[name] = va;
}

HLE("opengl32.dll", glGetString, 1) {
  GL_OR_RETURN(0);
  const char* s = g->GetString(Arg(c, 0));
  return s ? GuestString(Arg(c, 0), s) : 0;
}
HLE("opengl32.dll", glGetError, 0) {
  GL_OR_RETURN(0);
  return g->GetError();
}
HLE("opengl32.dll", glGetIntegerv, 2) {
  GL_OR_RETURN(0);
  int32_t v[16] = {};
  if (g->GetIntegerv(Arg(c, 0), v))
    for (int i = 0; i < (Arg(c, 0) == gl::VIEWPORT ? 4 : 1); i++)
      Wr32(Arg(c, 1) + 4 * uint32_t(i), uint32_t(v[i]));
  return 0;
}
HLE("opengl32.dll", glGetFloatv, 2) {
  GL_OR_RETURN(0);
  float v[16] = {};
  gl::Enum p = Arg(c, 0);
  if (g->GetFloatv(p, v)) {
    int n = p == gl::MODELVIEW_MATRIX || p == gl::PROJECTION_MATRIX ? 16
            : p == gl::VIEWPORT ? 4 : 1;
    for (int i = 0; i < n; i++) Wr32(Arg(c, 1) + 4 * uint32_t(i), std::bit_cast<uint32_t>(v[i]));
  }
  return 0;
}
HLE("opengl32.dll", glIsEnabled, 1) {
  GL_OR_RETURN(0);
  return g->IsEnabled(Arg(c, 0)) ? 1 : 0;
}
HLE("opengl32.dll", glEnable, 1) { GL_OR_RETURN(0); g->SetEnabled(Arg(c, 0), true); return 0; }
HLE("opengl32.dll", glDisable, 1) { GL_OR_RETURN(0); g->SetEnabled(Arg(c, 0), false); return 0; }
HLE("opengl32.dll", glHint, 2) { return 0; }  // hints may be ignored
HLE("opengl32.dll", glFinish, 0) { return 0; }
HLE("opengl32.dll", glFlush, 0) { return 0; }
// There is one colour buffer pair; only GL_BACK/GL_FRONT are meaningful.
HLE("opengl32.dll", glDrawBuffer, 1) { return 0; }
HLE("opengl32.dll", glReadBuffer, 1) { return 0; }

HLE("opengl32.dll", glBlendFunc, 2) { GL_OR_RETURN(0); g->BlendFunc(Arg(c, 0), Arg(c, 1)); return 0; }
HLE("opengl32.dll", glAlphaFunc, 2) { GL_OR_RETURN(0); g->AlphaFunc(Arg(c, 0), F(c, 1)); return 0; }
HLE("opengl32.dll", glDepthFunc, 1) { GL_OR_RETURN(0); g->DepthFunc(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glDepthMask, 1) { GL_OR_RETURN(0); g->DepthMask(Arg(c, 0) & 0xFF); return 0; }
HLE("opengl32.dll", glDepthRange, 4) { GL_OR_RETURN(0); g->DepthRange(D(c, 0), D(c, 2)); return 0; }
HLE("opengl32.dll", glColorMask, 4) {
  GL_OR_RETURN(0);
  g->ColorMask(Arg(c, 0) & 0xFF, Arg(c, 1) & 0xFF, Arg(c, 2) & 0xFF, Arg(c, 3) & 0xFF);
  return 0;
}
HLE("opengl32.dll", glCullFace, 1) { GL_OR_RETURN(0); g->CullFace(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glFrontFace, 1) { GL_OR_RETURN(0); g->FrontFace(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glPolygonMode, 2) { GL_OR_RETURN(0); g->PolygonMode(Arg(c, 0), Arg(c, 1)); return 0; }
HLE("opengl32.dll", glPolygonOffset, 2) { GL_OR_RETURN(0); g->PolygonOffset(F(c, 0), F(c, 1)); return 0; }
HLE("opengl32.dll", glShadeModel, 1) { GL_OR_RETURN(0); g->ShadeModel(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glViewport, 4) {
  GL_OR_RETURN(0);
  g->Viewport(int32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), int32_t(Arg(c, 3)));
  return 0;
}
HLE("opengl32.dll", glScissor, 4) {
  GL_OR_RETURN(0);
  g->Scissor(int32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), int32_t(Arg(c, 3)));
  return 0;
}
HLE("opengl32.dll", glStencilFunc, 3) {
  GL_OR_RETURN(0);
  g->StencilFunc(Arg(c, 0), int32_t(Arg(c, 1)), Arg(c, 2));
  return 0;
}
HLE("opengl32.dll", glStencilOp, 3) { GL_OR_RETURN(0); g->StencilOp(Arg(c, 0), Arg(c, 1), Arg(c, 2)); return 0; }
HLE("opengl32.dll", glStencilMask, 1) { GL_OR_RETURN(0); g->StencilMask(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glFogf, 2) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 1)};
  g->Fog(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glFogi, 2) {
  GL_OR_RETURN(0);
  float v[4] = {float(int32_t(Arg(c, 1)))};
  g->Fog(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glFogfv, 2) {
  GL_OR_RETURN(0);
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = Fv(Arg(c, 1), Arg(c, 0) == gl::FOG_COLOR ? i : 0);
  g->Fog(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glClipPlane, 2) {
  GL_OR_RETURN(0);
  double e[4];
  std::memcpy(e, GPtr(Arg(c, 1)), sizeof e);
  g->ClipPlane(Arg(c, 0), e);
  return 0;
}
HLE("opengl32.dll", glClearColor, 4) {
  GL_OR_RETURN(0);
  g->ClearColor(F(c, 0), F(c, 1), F(c, 2), F(c, 3));
  return 0;
}
HLE("opengl32.dll", glClearDepth, 2) { GL_OR_RETURN(0); g->ClearDepth(D(c, 0)); return 0; }
HLE("opengl32.dll", glClearStencil, 1) { GL_OR_RETURN(0); g->ClearStencil(int32_t(Arg(c, 0))); return 0; }
HLE("opengl32.dll", glClear, 1) { GL_OR_RETURN(0); g->Clear(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glPixelStorei, 2) { GL_OR_RETURN(0); g->PixelStore(Arg(c, 0), int32_t(Arg(c, 1))); return 0; }

// lighting: vector parameters have 4 components for colours and
// positions, 3 for the spot direction, else 1
int LightComponents(gl::Enum pname) {
  switch (pname) {
    case gl::SPOT_DIRECTION: return 3;
    case gl::AMBIENT: case gl::DIFFUSE: case gl::SPECULAR: case gl::POSITION:
    case gl::EMISSION: case gl::AMBIENT_AND_DIFFUSE: case gl::LIGHT_MODEL_AMBIENT:
      return 4;
    default: return 1;
  }
}
void ReadFv(uint32_t va, gl::Enum pname, float v[4]) {
  int n = LightComponents(pname);
  for (int i = 0; i < 4; i++) v[i] = i < n ? Fv(va, i) : 0;
}

HLE("opengl32.dll", glLightf, 3) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 2)};
  g->Light(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glLightfv, 3) {
  GL_OR_RETURN(0);
  float v[4];
  ReadFv(Arg(c, 2), Arg(c, 1), v);
  g->Light(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glLightModelf, 2) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 1)};
  g->LightModel(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glLightModeli, 2) {
  GL_OR_RETURN(0);
  float v[4] = {float(int32_t(Arg(c, 1)))};
  g->LightModel(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glLightModelfv, 2) {
  GL_OR_RETURN(0);
  float v[4];
  ReadFv(Arg(c, 1), Arg(c, 0), v);
  g->LightModel(Arg(c, 0), v);
  return 0;
}
HLE("opengl32.dll", glMaterialf, 3) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 2)};
  g->Material(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glMaterialfv, 3) {
  GL_OR_RETURN(0);
  float v[4];
  ReadFv(Arg(c, 2), Arg(c, 1), v);
  g->Material(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glColorMaterial, 2) {
  GL_OR_RETURN(0);
  g->ColorMaterial(Arg(c, 0), Arg(c, 1));
  return 0;
}
HLE("opengl32.dll", glNormal3fv, 1) {
  GL_OR_RETURN(0);
  g->Normal(Fv(Arg(c, 0), 0), Fv(Arg(c, 0), 1), Fv(Arg(c, 0), 2));
  return 0;
}

// matrices
HLE("opengl32.dll", glMatrixMode, 1) { GL_OR_RETURN(0); g->MatrixMode(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glLoadIdentity, 0) { GL_OR_RETURN(0); g->LoadIdentity(); return 0; }
HLE("opengl32.dll", glLoadMatrixf, 1) {
  GL_OR_RETURN(0);
  float m[16];
  std::memcpy(m, GPtr(Arg(c, 0)), sizeof m);
  g->LoadMatrix(m);
  return 0;
}
HLE("opengl32.dll", glMultMatrixf, 1) {
  GL_OR_RETURN(0);
  float m[16];
  std::memcpy(m, GPtr(Arg(c, 0)), sizeof m);
  g->MultMatrix(m);
  return 0;
}
HLE("opengl32.dll", glPushMatrix, 0) { GL_OR_RETURN(0); g->PushMatrix(); return 0; }
HLE("opengl32.dll", glPopMatrix, 0) { GL_OR_RETURN(0); g->PopMatrix(); return 0; }
HLE("opengl32.dll", glOrtho, 12) {
  GL_OR_RETURN(0);
  g->Ortho(D(c, 0), D(c, 2), D(c, 4), D(c, 6), D(c, 8), D(c, 10));
  return 0;
}
HLE("opengl32.dll", glFrustum, 12) {
  GL_OR_RETURN(0);
  g->Frustum(D(c, 0), D(c, 2), D(c, 4), D(c, 6), D(c, 8), D(c, 10));
  return 0;
}
HLE("opengl32.dll", glTranslatef, 3) { GL_OR_RETURN(0); g->Translate(F(c, 0), F(c, 1), F(c, 2)); return 0; }
HLE("opengl32.dll", glRotatef, 4) { GL_OR_RETURN(0); g->Rotate(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); return 0; }
HLE("opengl32.dll", glScalef, 3) { GL_OR_RETURN(0); g->Scale(F(c, 0), F(c, 1), F(c, 2)); return 0; }

// textures
HLE("opengl32.dll", glGenTextures, 2) {
  GL_OR_RETURN(0);
  int n = int32_t(Arg(c, 0));
  std::vector<uint32_t> names(size_t(std::max(n, 0)));
  g->GenTextures(n, names.data());
  for (int i = 0; i < n; i++) Wr32(Arg(c, 1) + 4 * uint32_t(i), names[size_t(i)]);
  return 0;
}
HLE("opengl32.dll", glDeleteTextures, 2) {
  GL_OR_RETURN(0);
  int n = int32_t(Arg(c, 0));
  std::vector<uint32_t> names(size_t(std::max(n, 0)));
  for (int i = 0; i < n; i++) names[size_t(i)] = Rd32(Arg(c, 1) + 4 * uint32_t(i));
  g->DeleteTextures(n, names.data());
  return 0;
}
HLE("opengl32.dll", glBindTexture, 2) { GL_OR_RETURN(0); g->BindTexture(Arg(c, 0), Arg(c, 1)); return 0; }
HLE("opengl32.dll", glTexParameterf, 3) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 2)};
  g->TexParameter(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glTexParameteri, 3) {
  GL_OR_RETURN(0);
  float v[4] = {float(int32_t(Arg(c, 2)))};
  g->TexParameter(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glTexParameterfv, 3) {
  GL_OR_RETURN(0);
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = Fv(Arg(c, 2), Arg(c, 1) == gl::TEXTURE_BORDER_COLOR ? i : 0);
  g->TexParameter(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glTexImage2D, 9) {
  GL_OR_RETURN(0);
  g->TexImage2D(Arg(c, 0), int32_t(Arg(c, 1)), Arg(c, 2), int32_t(Arg(c, 3)),
                int32_t(Arg(c, 4)), int32_t(Arg(c, 5)), Arg(c, 6), Arg(c, 7), P(c, 8));
  return 0;
}
HLE("opengl32.dll", glTexSubImage2D, 9) {
  GL_OR_RETURN(0);
  g->TexSubImage2D(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)), int32_t(Arg(c, 3)),
                   int32_t(Arg(c, 4)), int32_t(Arg(c, 5)), Arg(c, 6), Arg(c, 7), P(c, 8));
  return 0;
}
HLE("opengl32.dll", glCompressedTexImage2DARB, 8) {
  GL_OR_RETURN(0);
  g->CompressedTexImage2D(Arg(c, 0), int32_t(Arg(c, 1)), Arg(c, 2), int32_t(Arg(c, 3)),
                          int32_t(Arg(c, 4)), int32_t(Arg(c, 5)), int32_t(Arg(c, 6)),
                          P(c, 7));
  return 0;
}
HLE("opengl32.dll", glTexEnvf, 3) {
  GL_OR_RETURN(0);
  float v[4] = {F(c, 2)};
  g->TexEnv(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glTexEnvi, 3) {
  GL_OR_RETURN(0);
  float v[4] = {float(int32_t(Arg(c, 2)))};
  g->TexEnv(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glTexEnvfv, 3) {
  GL_OR_RETURN(0);
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = Fv(Arg(c, 2), Arg(c, 1) == gl::TEXTURE_ENV_COLOR ? i : 0);
  g->TexEnv(Arg(c, 0), Arg(c, 1), v);
  return 0;
}
HLE("opengl32.dll", glActiveTextureARB, 1) { GL_OR_RETURN(0); g->ActiveTexture(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glClientActiveTextureARB, 1) {
  GL_OR_RETURN(0);
  g->ClientActiveTexture(Arg(c, 0));
  return 0;
}

// arrays
HLE("opengl32.dll", glVertexPointer, 4) {
  GL_OR_RETURN(0);
  g->VertexPointer(int32_t(Arg(c, 0)), Arg(c, 1), int32_t(Arg(c, 2)), P(c, 3));
  return 0;
}
HLE("opengl32.dll", glColorPointer, 4) {
  GL_OR_RETURN(0);
  g->ColorPointer(int32_t(Arg(c, 0)), Arg(c, 1), int32_t(Arg(c, 2)), P(c, 3));
  return 0;
}
HLE("opengl32.dll", glTexCoordPointer, 4) {
  GL_OR_RETURN(0);
  g->TexCoordPointer(int32_t(Arg(c, 0)), Arg(c, 1), int32_t(Arg(c, 2)), P(c, 3));
  return 0;
}
HLE("opengl32.dll", glNormalPointer, 3) {
  GL_OR_RETURN(0);
  g->NormalPointer(Arg(c, 0), int32_t(Arg(c, 1)), P(c, 2));
  return 0;
}
HLE("opengl32.dll", glEnableClientState, 1) { GL_OR_RETURN(0); g->SetClientState(Arg(c, 0), true); return 0; }
HLE("opengl32.dll", glDisableClientState, 1) { GL_OR_RETURN(0); g->SetClientState(Arg(c, 0), false); return 0; }
HLE("opengl32.dll", glDrawArrays, 3) {
  GL_OR_RETURN(0);
  g->DrawArrays(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)));
  return 0;
}
HLE("opengl32.dll", glDrawElements, 4) {
  GL_OR_RETURN(0);
  g->DrawElements(Arg(c, 0), int32_t(Arg(c, 1)), Arg(c, 2), P(c, 3));
  return 0;
}
HLE("opengl32.dll", glArrayElement, 1) { GL_OR_RETURN(0); g->ArrayElement(int32_t(Arg(c, 0))); return 0; }
// Compiled vertex arrays are a hint that the arrays stay unchanged.
HLE("opengl32.dll", glLockArraysEXT, 2) { return 0; }
HLE("opengl32.dll", glUnlockArraysEXT, 0) { return 0; }

// immediate mode
HLE("opengl32.dll", glBegin, 1) { GL_OR_RETURN(0); g->Begin(Arg(c, 0)); return 0; }
HLE("opengl32.dll", glEnd, 0) { GL_OR_RETURN(0); g->End(); return 0; }
HLE("opengl32.dll", glVertex2f, 2) { GL_OR_RETURN(0); g->Vertex(F(c, 0), F(c, 1), 0, 1); return 0; }
HLE("opengl32.dll", glVertex2i, 2) {
  GL_OR_RETURN(0);
  g->Vertex(float(int32_t(Arg(c, 0))), float(int32_t(Arg(c, 1))), 0, 1);
  return 0;
}
HLE("opengl32.dll", glVertex3f, 3) { GL_OR_RETURN(0); g->Vertex(F(c, 0), F(c, 1), F(c, 2), 1); return 0; }
HLE("opengl32.dll", glVertex3fv, 1) {
  GL_OR_RETURN(0);
  uint32_t v = Arg(c, 0);
  g->Vertex(Fv(v, 0), Fv(v, 1), Fv(v, 2), 1);
  return 0;
}
HLE("opengl32.dll", glColor3f, 3) { GL_OR_RETURN(0); g->Color(F(c, 0), F(c, 1), F(c, 2), 1); return 0; }
HLE("opengl32.dll", glColor4f, 4) { GL_OR_RETURN(0); g->Color(F(c, 0), F(c, 1), F(c, 2), F(c, 3)); return 0; }
HLE("opengl32.dll", glColor3fv, 1) {
  GL_OR_RETURN(0);
  uint32_t v = Arg(c, 0);
  g->Color(Fv(v, 0), Fv(v, 1), Fv(v, 2), 1);
  return 0;
}
HLE("opengl32.dll", glColor4fv, 1) {
  GL_OR_RETURN(0);
  uint32_t v = Arg(c, 0);
  g->Color(Fv(v, 0), Fv(v, 1), Fv(v, 2), Fv(v, 3));
  return 0;
}
HLE("opengl32.dll", glColor4ub, 4) {
  GL_OR_RETURN(0);
  g->Color((Arg(c, 0) & 0xFF) / 255.0f, (Arg(c, 1) & 0xFF) / 255.0f,
           (Arg(c, 2) & 0xFF) / 255.0f, (Arg(c, 3) & 0xFF) / 255.0f);
  return 0;
}
HLE("opengl32.dll", glColor4ubv, 1) {
  GL_OR_RETURN(0);
  const uint8_t* p = GPtr(Arg(c, 0));
  g->Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
  return 0;
}
HLE("opengl32.dll", glColor3ubv, 1) {
  GL_OR_RETURN(0);
  const uint8_t* p = GPtr(Arg(c, 0));
  g->Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1);
  return 0;
}
HLE("opengl32.dll", glTexCoord2f, 2) { GL_OR_RETURN(0); g->TexCoord(0, F(c, 0), F(c, 1)); return 0; }
HLE("opengl32.dll", glTexCoord2fv, 1) {
  GL_OR_RETURN(0);
  g->TexCoord(0, Fv(Arg(c, 0), 0), Fv(Arg(c, 0), 1));
  return 0;
}
HLE("opengl32.dll", glMultiTexCoord2fARB, 3) {
  GL_OR_RETURN(0);
  g->TexCoord(int(Arg(c, 0) - gl::TEXTURE0), F(c, 1), F(c, 2));
  return 0;
}
HLE("opengl32.dll", glMultiTexCoord2fvARB, 2) {
  GL_OR_RETURN(0);
  g->TexCoord(int(Arg(c, 0) - gl::TEXTURE0), Fv(Arg(c, 1), 0), Fv(Arg(c, 1), 1));
  return 0;
}
HLE("opengl32.dll", glNormal3f, 3) { GL_OR_RETURN(0); g->Normal(F(c, 0), F(c, 1), F(c, 2)); return 0; }

HLE("opengl32.dll", glReadPixels, 7) {
  GL_OR_RETURN(0);
  g->ReadPixels(int32_t(Arg(c, 0)), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
                int32_t(Arg(c, 3)), Arg(c, 4), Arg(c, 5), GPtr(Arg(c, 6)));
  return 0;
}

}  // namespace
}  // namespace recomp::hle
