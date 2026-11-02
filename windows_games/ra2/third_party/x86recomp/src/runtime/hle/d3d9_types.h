#pragma once
// Direct3D 9 structures as the 32-bit guest lays them out. Pointers and
// handles are uint32_t guest values; field names follow the SDK. Every
// offset is checked against the real headers by d3d9_layout_check.inc
// (generated, see tools/layout/README).

#include <cstddef>
#include <cstdint>

#include "runtime/hle/ddraw_types.h"

namespace recomp::guest {

using UINT = uint32_t;
using INT = int32_t;
using BOOL = int32_t;
using D3DFORMAT = uint32_t;
using HWND = uint32_t;

struct D3DADAPTER_IDENTIFIER9 {
  char Driver[512];
  char Description[512];
  char DeviceName[32];
  uint32_t DriverVersionLowPart;
  uint32_t DriverVersionHighPart;
  DWORD VendorId, DeviceId, SubSysId, Revision;
  GUID DeviceIdentifier;
  DWORD WHQLLevel;
};

struct D3DDISPLAYMODE {
  UINT Width, Height, RefreshRate;
  D3DFORMAT Format;
};

struct D3DVSHADERCAPS2_0 {
  DWORD Caps;
  INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth;
};

struct D3DPSHADERCAPS2_0 {
  DWORD Caps;
  INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth,
      NumInstructionSlots;
};

struct D3DCAPS9 {
  DWORD DeviceType;
  UINT AdapterOrdinal;
  DWORD Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps,
      PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps,
      AlphaCmpCaps, ShadeCaps, TextureCaps, TextureFilterCaps,
      CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps,
      VolumeTextureAddressCaps, LineCaps, MaxTextureWidth, MaxTextureHeight,
      MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio, MaxAnisotropy;
  float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight,
      GuardBandBottom, ExtentsAdjust;
  DWORD StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages,
      MaxSimultaneousTextures, VertexProcessingCaps, MaxActiveLights,
      MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
  float MaxPointSize;
  DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride,
      VertexShaderVersion, MaxVertexShaderConst, PixelShaderVersion;
  float PixelShader1xMaxValue;
  DWORD DevCaps2;
  float MaxNpatchTessellationLevel;
  DWORD Reserved5;
  UINT MasterAdapterOrdinal, AdapterOrdinalInGroup, NumberOfAdaptersInGroup;
  DWORD DeclTypes, NumSimultaneousRTs, StretchRectFilterCaps;
  D3DVSHADERCAPS2_0 VS20Caps;
  D3DPSHADERCAPS2_0 PS20Caps;
  DWORD VertexTextureFilterCaps, MaxVShaderInstructionsExecuted,
      MaxPShaderInstructionsExecuted, MaxVertexShader30InstructionSlots,
      MaxPixelShader30InstructionSlots;
};

struct D3DPRESENT_PARAMETERS {
  UINT BackBufferWidth, BackBufferHeight;
  D3DFORMAT BackBufferFormat;
  UINT BackBufferCount;
  DWORD MultiSampleType, MultiSampleQuality, SwapEffect;
  HWND hDeviceWindow;
  BOOL Windowed, EnableAutoDepthStencil;
  D3DFORMAT AutoDepthStencilFormat;
  DWORD Flags;
  UINT FullScreen_RefreshRateInHz, PresentationInterval;
};

struct D3DDEVICE_CREATION_PARAMETERS {
  UINT AdapterOrdinal;
  DWORD DeviceType;
  HWND hFocusWindow;
  DWORD BehaviorFlags;
};

struct D3DSURFACE_DESC {
  D3DFORMAT Format;
  DWORD Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
  UINT Width, Height;
};

struct D3DLOCKED_RECT {
  INT Pitch;
  GPTR pBits;
};

struct D3DVIEWPORT9 {
  DWORD X, Y, Width, Height;
  float MinZ, MaxZ;
};

struct D3DVERTEXELEMENT9 {
  WORD Stream, Offset;
  uint8_t Type, Method, Usage, UsageIndex;
};

struct D3DVERTEXBUFFER_DESC {
  D3DFORMAT Format;
  DWORD Type, Usage, Pool;
  UINT Size;
  DWORD FVF;
};

struct D3DINDEXBUFFER_DESC {
  D3DFORMAT Format;
  DWORD Type, Usage, Pool;
  UINT Size;
};

struct D3DRASTER_STATUS {
  BOOL InVBlank;
  UINT ScanLine;
};

#include "runtime/hle/d3d9_layout_check.inc"

}  // namespace recomp::guest
