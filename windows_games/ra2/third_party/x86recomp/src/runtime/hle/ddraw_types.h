#pragma once
// DirectDraw 7 / Direct3D 7 structures as the 32-bit guest lays them out.
// Pointers are uint32_t guest addresses; field names follow the SDK (the
// first member of each anonymous union). Every offset is checked against
// the real headers by ddraw_layout_check.inc (generated, see
// tools/layout/README).

#include <cstddef>
#include <cstdint>

namespace recomp::guest {

using DWORD = uint32_t;
using WORD = uint16_t;
using LONG = int32_t;
using D3DVALUE = float;
using GPTR = uint32_t;  // guest pointer

struct GUID {
  uint32_t Data1;
  uint16_t Data2, Data3;
  uint8_t Data4[8];
};

struct DDCOLORKEY {
  DWORD dwColorSpaceLowValue, dwColorSpaceHighValue;
};

struct DDSCAPS {
  DWORD dwCaps;
};

struct DDSCAPS2 {
  DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4;
};

struct DDPIXELFORMAT {
  DWORD dwSize, dwFlags, dwFourCC;
  DWORD dwRGBBitCount;      // also dwZBufferBitDepth, dwAlphaBitDepth ...
  DWORD dwRBitMask;         // also dwStencilBitDepth ...
  DWORD dwGBitMask;         // also dwZBitMask ...
  DWORD dwBBitMask;         // also dwStencilBitMask ...
  DWORD dwRGBAlphaBitMask;  // also dwRGBZBitMask ...
};

struct DDSURFACEDESC2 {
  DWORD dwSize, dwFlags, dwHeight, dwWidth;
  LONG lPitch;              // also dwLinearSize
  DWORD dwBackBufferCount;  // also dwDepth
  DWORD dwMipMapCount;      // also dwRefreshRate, dwSrcVBHandle
  DWORD dwAlphaBitDepth, dwReserved;
  GPTR lpSurface;
  DDCOLORKEY ddckCKDestOverlay, ddckCKDestBlt, ddckCKSrcOverlay, ddckCKSrcBlt;
  DDPIXELFORMAT ddpfPixelFormat;  // also dwFVF
  DDSCAPS2 ddsCaps;
  DWORD dwTextureStage;
};

// The DirectDraw 1-3 surface description: DDSURFACEDESC2 up to and with
// the pixel format, then a DDSCAPS (one DWORD) and no texture stage.
struct DDSURFACEDESC {
  DWORD dwSize, dwFlags, dwHeight, dwWidth;
  LONG lPitch;
  DWORD dwBackBufferCount;
  DWORD dwMipMapCount;      // also dwZBufferBitDepth, dwRefreshRate
  DWORD dwAlphaBitDepth, dwReserved;
  GPTR lpSurface;
  DDCOLORKEY ddckCKDestOverlay, ddckCKDestBlt, ddckCKSrcOverlay, ddckCKSrcBlt;
  DDPIXELFORMAT ddpfPixelFormat;
  DDSCAPS ddsCaps;
};
static_assert(sizeof(DDSURFACEDESC) == 108, "DDSURFACEDESC size");
static_assert(offsetof(DDSURFACEDESC, ddpfPixelFormat) == 72, "DDSURFACEDESC.ddpfPixelFormat");
static_assert(offsetof(DDSURFACEDESC2, ddpfPixelFormat) == 72, "DDSURFACEDESC2.ddpfPixelFormat");

struct DDCAPS_DX7 {
  DWORD dwSize, dwCaps, dwCaps2, dwCKeyCaps, dwFXCaps, dwFXAlphaCaps,
      dwPalCaps, dwSVCaps, dwAlphaBltConstBitDepths, dwAlphaBltPixelBitDepths,
      dwAlphaBltSurfaceBitDepths, dwAlphaOverlayConstBitDepths,
      dwAlphaOverlayPixelBitDepths, dwAlphaOverlaySurfaceBitDepths,
      dwZBufferBitDepths, dwVidMemTotal, dwVidMemFree, dwMaxVisibleOverlays,
      dwCurrVisibleOverlays, dwNumFourCCCodes, dwAlignBoundarySrc,
      dwAlignSizeSrc, dwAlignBoundaryDest, dwAlignSizeDest,
      dwAlignStrideAlign;
  DWORD dwRops[8];
  DDSCAPS ddsOldCaps;
  DWORD dwMinOverlayStretch, dwMaxOverlayStretch, dwMinLiveVideoStretch,
      dwMaxLiveVideoStretch, dwMinHwCodecStretch, dwMaxHwCodecStretch,
      dwReserved1, dwReserved2, dwReserved3, dwSVBCaps, dwSVBCKeyCaps,
      dwSVBFXCaps;
  DWORD dwSVBRops[8];
  DWORD dwVSBCaps, dwVSBCKeyCaps, dwVSBFXCaps;
  DWORD dwVSBRops[8];
  DWORD dwSSBCaps, dwSSBCKeyCaps, dwSSBFXCaps;
  DWORD dwSSBRops[8];
  DWORD dwMaxVideoPorts, dwCurrVideoPorts, dwSVBCaps2, dwNLVBCaps,
      dwNLVBCaps2, dwNLVBCKeyCaps, dwNLVBFXCaps;
  DWORD dwNLVBRops[8];
  DDSCAPS2 ddsCaps;
};

struct DDDEVICEIDENTIFIER2 {
  char szDriver[512];
  char szDescription[512];
  uint32_t liDriverVersionLow, liDriverVersionHigh;  // LARGE_INTEGER
  DWORD dwVendorId, dwDeviceId, dwSubSysId, dwRevision;
  GUID guidDeviceIdentifier;
  DWORD dwWHQLLevel;
  DWORD pad_;  // LARGE_INTEGER makes the SDK struct 8-aligned (1072 bytes)
};

struct DDBLTFX {
  DWORD dwSize, dwDDFX, dwROP, dwDDROP, dwRotationAngle, dwZBufferOpCode,
      dwZBufferLow, dwZBufferHigh, dwZBufferBaseDest, dwZDestConstBitDepth,
      dwZDestConst, dwZSrcConstBitDepth, dwZSrcConst,
      dwAlphaEdgeBlendBitDepth, dwAlphaEdgeBlend, dwReserved,
      dwAlphaDestConstBitDepth, dwAlphaDestConst, dwAlphaSrcConstBitDepth,
      dwAlphaSrcConst;
  DWORD dwFillColor;  // also dwFillDepth, dwFillPixel, lpDDSPattern
  DDCOLORKEY ddckDestColorkey, ddckSrcColorkey;
};

struct D3DPRIMCAPS {
  DWORD dwSize, dwMiscCaps, dwRasterCaps, dwZCmpCaps, dwSrcBlendCaps,
      dwDestBlendCaps, dwAlphaCmpCaps, dwShadeCaps, dwTextureCaps,
      dwTextureFilterCaps, dwTextureBlendCaps, dwTextureAddressCaps,
      dwStippleWidth, dwStippleHeight;
};

struct D3DDEVICEDESC7 {
  DWORD dwDevCaps;
  D3DPRIMCAPS dpcLineCaps, dpcTriCaps;
  DWORD dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth;
  DWORD dwMinTextureWidth, dwMinTextureHeight;
  DWORD dwMaxTextureWidth, dwMaxTextureHeight;
  DWORD dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
  D3DVALUE dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight,
      dvGuardBandBottom, dvExtentsAdjust;
  DWORD dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
  WORD wMaxTextureBlendStages, wMaxSimultaneousTextures;
  DWORD dwMaxActiveLights;
  D3DVALUE dvMaxVertexW;
  GUID deviceGUID;
  WORD wMaxUserClipPlanes, wMaxVertexBlendMatrices;
  DWORD dwVertexProcessingCaps;
  DWORD dwReserved1, dwReserved2, dwReserved3, dwReserved4;
};

struct D3DVIEWPORT7 {
  DWORD dwX, dwY, dwWidth, dwHeight;
  D3DVALUE dvMinZ, dvMaxZ;
};

struct D3DCOLORVALUE {
  D3DVALUE r, g, b, a;
};

struct D3DVECTOR {
  D3DVALUE x, y, z;
};

struct D3DMATERIAL7 {
  D3DCOLORVALUE diffuse, ambient, specular, emissive;
  D3DVALUE power;
};

struct D3DLIGHT7 {
  DWORD dltType;
  D3DCOLORVALUE dcvDiffuse, dcvSpecular, dcvAmbient;
  D3DVECTOR dvPosition, dvDirection;
  D3DVALUE dvRange, dvFalloff, dvAttenuation0, dvAttenuation1,
      dvAttenuation2, dvTheta, dvPhi;
};

struct D3DMATRIX {
  D3DVALUE m[4][4];
};

struct D3DVERTEXBUFFERDESC {
  DWORD dwSize, dwCaps, dwFVF, dwNumVertices;
};

struct D3DRECT {
  LONG x1, y1, x2, y2;
};

struct RECT {
  LONG left, top, right, bottom;
};

struct D3DCLIPSTATUS {
  DWORD dwFlags, dwStatus;
  D3DVALUE minx, maxx, miny, maxy, minz, maxz;
};

#include "runtime/hle/ddraw_layout_check.inc"

}  // namespace recomp::guest
