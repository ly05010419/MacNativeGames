// Format/FVF/primitive helpers shared by all backends.

#include "runtime/gfx/device.h"

namespace recomp::gfx {

uint32_t BitsPerPixel(Format f) {
  switch (f) {
    case Format::X8R8G8B8: case Format::A8R8G8B8: case Format::D24X8:
    case Format::D24S8: case Format::D32:
      return 32;
    case Format::R8G8B8:
      return 24;
    case Format::R5G6B5: case Format::X1R5G5B5: case Format::A1R5G5B5:
    case Format::A4R4G4B4: case Format::X4R4G4B4: case Format::A8L8:
    case Format::D16: case Format::D15S1:
      return 16;
    case Format::R3G3B2: case Format::A8: case Format::L8: case Format::P8:
      return 8;
    case Format::DXT1:
      return 4;
    case Format::DXT2: case Format::DXT3: case Format::DXT4: case Format::DXT5:
      return 8;
    case Format::R16F:
      return 16;
    case Format::G16R16F: case Format::R32F:
      return 32;
    case Format::A16B16G16R16F: case Format::G32R32F:
      return 64;
    case Format::A32B32G32R32F:
      return 128;
    default:
      return 0;
  }
}

bool IsCompressed(Format f) {
  return f == Format::DXT1 || f == Format::DXT2 || f == Format::DXT3 ||
         f == Format::DXT4 || f == Format::DXT5;
}

bool IsFloat(Format f) {
  return f == Format::R16F || f == Format::G16R16F || f == Format::A16B16G16R16F ||
         f == Format::R32F || f == Format::G32R32F || f == Format::A32B32G32R32F;
}

uint32_t RowPitch(Format f, uint32_t width) {
  if (IsCompressed(f)) {
    uint32_t blocks = (width + 3) / 4;
    return blocks * (f == Format::DXT1 ? 8 : 16);
  }
  return width * BitsPerPixel(f) / 8;
}

uint32_t RowCount(Format f, uint32_t height) {
  return IsCompressed(f) ? (height + 3) / 4 : height;
}

namespace {
uint32_t Expand(uint32_t v, int bits) {
  // Replicate high bits so 0x1F -> 0xFF exactly.
  v <<= (8 - bits);
  return v | (v >> bits);
}
}  // namespace

uint32_t RawPixel(const uint8_t* p, Format f) {
  switch (BitsPerPixel(f)) {
    case 32: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    case 24: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    case 16: return uint32_t(p[0]) | uint32_t(p[1]) << 8;
    case 8: return p[0];
    default: return 0;
  }
}

void PutRawPixel(uint8_t* p, Format f, uint32_t v) {
  switch (BitsPerPixel(f)) {
    case 32: p[3] = uint8_t(v >> 24); [[fallthrough]];
    case 24: p[2] = uint8_t(v >> 16); [[fallthrough]];
    case 16: p[1] = uint8_t(v >> 8); [[fallthrough]];
    case 8: p[0] = uint8_t(v); break;
    default: break;
  }
}

uint32_t DecodePixel(const uint8_t* p, Format f) {
  uint32_t v = RawPixel(p, f);
  switch (f) {
    case Format::A8R8G8B8: return v;
    case Format::X8R8G8B8: case Format::R8G8B8: return v | 0xFF000000u;
    case Format::R5G6B5:
      return 0xFF000000u | Expand((v >> 11) & 0x1F, 5) << 16 |
             Expand((v >> 5) & 0x3F, 6) << 8 | Expand(v & 0x1F, 5);
    case Format::X1R5G5B5: case Format::A1R5G5B5: {
      uint32_t a = (f == Format::X1R5G5B5 || (v & 0x8000)) ? 0xFF : 0;
      return a << 24 | Expand((v >> 10) & 0x1F, 5) << 16 |
             Expand((v >> 5) & 0x1F, 5) << 8 | Expand(v & 0x1F, 5);
    }
    case Format::A4R4G4B4: case Format::X4R4G4B4: {
      uint32_t a = f == Format::X4R4G4B4 ? 0xFF : ((v >> 12) & 0xF) * 0x11;
      return a << 24 | ((v >> 8) & 0xF) * 0x11 << 16 |
             ((v >> 4) & 0xF) * 0x11 << 8 | (v & 0xF) * 0x11;
    }
    case Format::R3G3B2:
      return 0xFF000000u | ((v >> 5) & 7) * 0x24 << 16 |
             ((v >> 2) & 7) * 0x24 << 8 | (v & 3) * 0x55;
    case Format::A8: return v << 24;
    case Format::L8: return 0xFF000000u | v << 16 | v << 8 | v;
    case Format::A8L8: {
      uint32_t l = v & 0xFF;
      return (v >> 8) << 24 | l << 16 | l << 8 | l;
    }
    default: return 0xFFFF00FFu;  // magenta: unsupported
  }
}

void EncodePixel(uint8_t* p, Format f, uint32_t c) {
  uint32_t a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  uint32_t v = 0;
  switch (f) {
    case Format::A8R8G8B8: case Format::X8R8G8B8: case Format::R8G8B8: v = c; break;
    case Format::R5G6B5: v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); break;
    case Format::X1R5G5B5: case Format::A1R5G5B5:
      v = (a >= 0x80 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
      break;
    case Format::A4R4G4B4: case Format::X4R4G4B4:
      v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
      break;
    case Format::R3G3B2: v = (r >> 5) << 5 | (g >> 5) << 2 | (b >> 6); break;
    case Format::A8: v = a; break;
    case Format::L8: v = (r * 77 + g * 150 + b * 29) >> 8; break;
    case Format::A8L8: v = a << 8 | ((r * 77 + g * 150 + b * 29) >> 8); break;
    default: break;
  }
  PutRawPixel(p, f, v);
}

uint32_t FvfStride(uint32_t fvf) {
  uint32_t s = 0;
  switch (fvf & 0x00E) {
    case 0x002: s = 12; break;  // XYZ
    case 0x004: s = 16; break;  // XYZRHW
    case 0x006: s = 16; break;  // XYZB1
    case 0x008: s = 20; break;
    case 0x00A: s = 24; break;
    case 0x00C: s = 28; break;
    case 0x00E: s = 32; break;
  }
  if ((fvf & 0x400E) == 0x4002) s = 16;  // XYZW (D3D9)
  if (fvf & 0x010) s += 12;  // NORMAL
  if (fvf & 0x020) s += 4;   // RESERVED1 (D3D7) / PSIZE (D3D9)
  if (fvf & 0x040) s += 4;   // DIFFUSE
  if (fvf & 0x080) s += 4;   // SPECULAR
  uint32_t ntex = (fvf >> 8) & 0xF;
  for (uint32_t i = 0; i < ntex; i++) {
    switch ((fvf >> (16 + 2 * i)) & 3) {
      case 0: s += 8; break;
      case 1: s += 12; break;
      case 2: s += 16; break;
      case 3: s += 4; break;
    }
  }
  return s;
}

uint32_t PrimitiveCountFromVertices(uint32_t prim, uint32_t n) {
  switch (prim) {
    case 1: return n;                     // POINTLIST
    case 2: return n / 2;                 // LINELIST
    case 3: return n > 0 ? n - 1 : 0;     // LINESTRIP
    case 4: return n / 3;                 // TRIANGLELIST
    case 5: case 6: return n > 2 ? n - 2 : 0;  // STRIP, FAN
    default: return 0;
  }
}

}  // namespace recomp::gfx
