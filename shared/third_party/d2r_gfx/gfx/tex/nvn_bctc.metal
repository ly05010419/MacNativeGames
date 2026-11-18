// TOTK, GPUs without BC (A15/A16): BC2/BC3/BC7 -> ASTC 4x4 and BC4/BC5 (unsigned) -> EAC R11/RG11, one thread per 4x4
// block, run on the deswizzled blocks of an upload (nvn_mtl.m, up_flush). Decompressed instead, these textures took
// 4x (RGBA8) and 2x (R8/RG8) the memory of the transcoded ones. The block is decoded to texels and encoded again, so
// the result is lossy (probe-gpu-bctc measures it against bcdec through Metal's own decoders).
// This file is the source; nvn_bctc_msl.h is generated from it by gen_bctc_msl.py.
//
// ASTC modes written (one partition, 4x4 weight grid; layout as in nvn_bc1astc.c):
//   opaque blocks          CEM 8  (RGB direct), 8-bit endpoints, 3-bit weights            block mode 0x053
//   constant alpha         CEM 12 (RGBA direct), 8-bit endpoints, 2-bit weights           block mode 0x042
//   varying alpha          CEM 12, dual plane (alpha on plane 1), 2-bit weights,          block mode 0x442
//                          endpoints in the 48-level range (trit + 4 bits, ISE packed)
// EAC R11: 8 bytes big-endian: base, multiplier << 4 | table, 16 x 3-bit indices (texel x * 4 + y, first at the top).
#include <metal_stdlib>
using namespace metal;

struct TP { uint src_off, src_pitch, w, h, fmt, dst_off; };   // fmt: NVN format; | 0x100 writes the decoded texels (probe)

// BC7 partition table (from bcdec) and the ASTC trit-block encoding (inverse of the decode in the ASTC specification),
// generated once; probe-gpu-bctc checks both through the reference decoders.
constant uchar bc7_part[128][16] = {   // [subsets - 2][partition] flattened; texel row * 4 + col; 0x80 = fix-up index
  {128,0,1,1,0,0,1,1,0,0,1,1,0,0,1,129},
  {128,0,0,1,0,0,0,1,0,0,0,1,0,0,0,129},
  {128,1,1,1,0,1,1,1,0,1,1,1,0,1,1,129},
  {128,0,0,1,0,0,1,1,0,0,1,1,0,1,1,129},
  {128,0,0,0,0,0,0,1,0,0,0,1,0,0,1,129},
  {128,0,1,1,0,1,1,1,0,1,1,1,1,1,1,129},
  {128,0,0,1,0,0,1,1,0,1,1,1,1,1,1,129},
  {128,0,0,0,0,0,0,1,0,0,1,1,0,1,1,129},
  {128,0,0,0,0,0,0,0,0,0,0,1,0,0,1,129},
  {128,0,1,1,0,1,1,1,1,1,1,1,1,1,1,129},
  {128,0,0,0,0,0,0,1,0,1,1,1,1,1,1,129},
  {128,0,0,0,0,0,0,0,0,0,0,1,0,1,1,129},
  {128,0,0,1,0,1,1,1,1,1,1,1,1,1,1,129},
  {128,0,0,0,0,0,0,0,1,1,1,1,1,1,1,129},
  {128,0,0,0,1,1,1,1,1,1,1,1,1,1,1,129},
  {128,0,0,0,0,0,0,0,0,0,0,0,1,1,1,129},
  {128,0,0,0,1,0,0,0,1,1,1,0,1,1,1,129},
  {128,1,129,1,0,0,0,1,0,0,0,0,0,0,0,0},
  {128,0,0,0,0,0,0,0,129,0,0,0,1,1,1,0},
  {128,1,129,1,0,0,1,1,0,0,0,1,0,0,0,0},
  {128,0,129,1,0,0,0,1,0,0,0,0,0,0,0,0},
  {128,0,0,0,1,0,0,0,129,1,0,0,1,1,1,0},
  {128,0,0,0,0,0,0,0,129,0,0,0,1,1,0,0},
  {128,1,1,1,0,0,1,1,0,0,1,1,0,0,0,129},
  {128,0,129,1,0,0,0,1,0,0,0,1,0,0,0,0},
  {128,0,0,0,1,0,0,0,129,0,0,0,1,1,0,0},
  {128,1,129,0,0,1,1,0,0,1,1,0,0,1,1,0},
  {128,0,129,1,0,1,1,0,0,1,1,0,1,1,0,0},
  {128,0,0,1,0,1,1,1,129,1,1,0,1,0,0,0},
  {128,0,0,0,1,1,1,1,129,1,1,1,0,0,0,0},
  {128,1,129,1,0,0,0,1,1,0,0,0,1,1,1,0},
  {128,0,129,1,1,0,0,1,1,0,0,1,1,1,0,0},
  {128,1,0,1,0,1,0,1,0,1,0,1,0,1,0,129},
  {128,0,0,0,1,1,1,1,0,0,0,0,1,1,1,129},
  {128,1,0,1,1,0,129,0,0,1,0,1,1,0,1,0},
  {128,0,1,1,0,0,1,1,129,1,0,0,1,1,0,0},
  {128,0,129,1,1,1,0,0,0,0,1,1,1,1,0,0},
  {128,1,0,1,0,1,0,1,129,0,1,0,1,0,1,0},
  {128,1,1,0,1,0,0,1,0,1,1,0,1,0,0,129},
  {128,1,0,1,1,0,1,0,1,0,1,0,0,1,0,129},
  {128,1,129,1,0,0,1,1,1,1,0,0,1,1,1,0},
  {128,0,0,1,0,0,1,1,129,1,0,0,1,0,0,0},
  {128,0,129,1,0,0,1,0,0,1,0,0,1,1,0,0},
  {128,0,129,1,1,0,1,1,1,1,0,1,1,1,0,0},
  {128,1,129,0,1,0,0,1,1,0,0,1,0,1,1,0},
  {128,0,1,1,1,1,0,0,1,1,0,0,0,0,1,129},
  {128,1,1,0,0,1,1,0,1,0,0,1,1,0,0,129},
  {128,0,0,0,0,1,129,0,0,1,1,0,0,0,0,0},
  {128,1,0,0,1,1,129,0,0,1,0,0,0,0,0,0},
  {128,0,129,0,0,1,1,1,0,0,1,0,0,0,0,0},
  {128,0,0,0,0,0,129,0,0,1,1,1,0,0,1,0},
  {128,0,0,0,0,1,0,0,129,1,1,0,0,1,0,0},
  {128,1,1,0,1,1,0,0,1,0,0,1,0,0,1,129},
  {128,0,1,1,0,1,1,0,1,1,0,0,1,0,0,129},
  {128,1,129,0,0,0,1,1,1,0,0,1,1,1,0,0},
  {128,0,129,1,1,0,0,1,1,1,0,0,0,1,1,0},
  {128,1,1,0,1,1,0,0,1,1,0,0,1,0,0,129},
  {128,1,1,0,0,0,1,1,0,0,1,1,1,0,0,129},
  {128,1,1,1,1,1,1,0,1,0,0,0,0,0,0,129},
  {128,0,0,1,1,0,0,0,1,1,1,0,0,1,1,129},
  {128,0,0,0,1,1,1,1,0,0,1,1,0,0,1,129},
  {128,0,129,1,0,0,1,1,1,1,1,1,0,0,0,0},
  {128,0,129,0,0,0,1,0,1,1,1,0,1,1,1,0},
  {128,1,0,0,0,1,0,0,0,1,1,1,0,1,1,129},
  {128,0,1,129,0,0,1,1,0,2,2,1,2,2,2,130},
  {128,0,0,129,0,0,1,1,130,2,1,1,2,2,2,1},
  {128,0,0,0,2,0,0,1,130,2,1,1,2,2,1,129},
  {128,2,2,130,0,0,2,2,0,0,1,1,0,1,1,129},
  {128,0,0,0,0,0,0,0,129,1,2,2,1,1,2,130},
  {128,0,1,129,0,0,1,1,0,0,2,2,0,0,2,130},
  {128,0,2,130,0,0,2,2,1,1,1,1,1,1,1,129},
  {128,0,1,1,0,0,1,1,130,2,1,1,2,2,1,129},
  {128,0,0,0,0,0,0,0,129,1,1,1,2,2,2,130},
  {128,0,0,0,1,1,1,1,129,1,1,1,2,2,2,130},
  {128,0,0,0,1,1,129,1,2,2,2,2,2,2,2,130},
  {128,0,1,2,0,0,129,2,0,0,1,2,0,0,1,130},
  {128,1,1,2,0,1,129,2,0,1,1,2,0,1,1,130},
  {128,1,2,2,0,129,2,2,0,1,2,2,0,1,2,130},
  {128,0,1,129,0,1,1,2,1,1,2,2,1,2,2,130},
  {128,0,1,129,2,0,0,1,130,2,0,0,2,2,2,0},
  {128,0,0,129,0,0,1,1,0,1,1,2,1,1,2,130},
  {128,1,1,129,0,0,1,1,130,0,0,1,2,2,0,0},
  {128,0,0,0,1,1,2,2,129,1,2,2,1,1,2,130},
  {128,0,2,130,0,0,2,2,0,0,2,2,1,1,1,129},
  {128,1,1,129,0,1,1,1,0,2,2,2,0,2,2,130},
  {128,0,0,129,0,0,0,1,130,2,2,1,2,2,2,1},
  {128,0,0,0,0,0,129,1,0,1,2,2,0,1,2,130},
  {128,0,0,0,1,1,0,0,130,2,129,0,2,2,1,0},
  {128,1,2,130,0,129,2,2,0,0,1,1,0,0,0,0},
  {128,0,1,2,0,0,1,2,129,1,2,2,2,2,2,130},
  {128,1,1,0,1,2,130,1,129,2,2,1,0,1,1,0},
  {128,0,0,0,0,1,129,0,1,2,130,1,1,2,2,1},
  {128,0,2,2,1,1,0,2,129,1,0,2,0,0,2,130},
  {128,1,1,0,0,129,1,0,2,0,0,2,2,2,2,130},
  {128,0,1,1,0,1,2,2,0,1,130,2,0,0,1,129},
  {128,0,0,0,2,0,0,0,130,2,1,1,2,2,2,129},
  {128,0,0,0,0,0,0,2,129,1,2,2,1,2,2,130},
  {128,2,2,130,0,0,2,2,0,0,1,2,0,0,1,129},
  {128,0,1,129,0,0,1,2,0,0,2,2,0,2,2,130},
  {128,1,2,0,0,129,2,0,0,1,130,0,0,1,2,0},
  {128,0,0,0,1,1,129,1,2,2,130,2,0,0,0,0},
  {128,1,2,0,1,2,0,1,130,0,129,2,0,1,2,0},
  {128,1,2,0,2,0,1,2,129,130,0,1,0,1,2,0},
  {128,0,1,1,2,2,0,0,1,1,130,2,0,0,1,129},
  {128,0,1,1,1,1,130,2,2,2,0,0,0,0,1,129},
  {128,1,0,129,0,1,0,1,2,2,2,2,2,2,2,130},
  {128,0,0,0,0,0,0,0,130,1,2,1,2,1,2,129},
  {128,0,2,2,1,129,2,2,0,0,2,2,1,1,2,130},
  {128,0,2,130,0,0,1,1,0,0,2,2,0,0,1,129},
  {128,2,2,0,1,2,130,1,0,2,2,0,1,2,2,129},
  {128,1,0,1,2,2,130,2,2,2,2,2,0,1,0,129},
  {128,0,0,0,2,1,2,1,130,1,2,1,2,1,2,129},
  {128,1,0,129,0,1,0,1,0,1,0,1,2,2,2,130},
  {128,2,2,130,0,1,1,1,0,2,2,2,0,1,1,129},
  {128,0,0,2,1,129,1,2,0,0,0,2,1,1,1,130},
  {128,0,0,0,2,129,1,2,2,1,1,2,2,1,1,130},
  {128,2,2,2,0,129,1,1,0,1,1,1,0,2,2,130},
  {128,0,0,2,1,1,1,2,129,1,1,2,0,0,0,130},
  {128,1,1,0,0,129,1,0,0,1,1,0,2,2,2,130},
  {128,0,0,0,0,0,0,0,2,1,129,2,2,1,1,130},
  {128,1,1,0,0,129,1,0,2,2,2,2,2,2,2,130},
  {128,0,2,2,0,0,1,1,0,0,129,1,0,0,2,130},
  {128,0,2,2,1,1,2,2,129,1,2,2,0,0,2,130},
  {128,0,0,0,0,0,0,0,0,0,0,0,2,129,1,130},
  {128,0,0,130,0,0,0,1,0,0,0,2,0,0,0,129},
  {128,2,2,2,1,2,2,2,0,2,2,2,129,2,2,130},
  {128,1,0,129,2,2,2,2,2,2,2,2,2,2,2,130},
  {128,1,1,129,2,0,1,1,130,2,0,1,2,2,2,0}
};
constant uchar trit_enc[243] = {   // ASTC trit-block encoding: index t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4 -> 8 packed bits
  0,1,2,4,5,6,8,9,10,16,17,18,20,21,22,24,25,26,3,7,11,19,23,27,12,13,14,32,33,34,36,37,38,40,41,42,48,49,50,52,53,54,56,57,58,35,39,43,51,55,59,44,45,46,64,65,66,68,69,70,72,73,74,80,81,82,84,85,86,88,89,90,67,71,75,83,87,91,76,77,78,128,129,130,132,133,134,136,137,138,144,145,146,148,149,150,152,153,154,131,135,139,147,151,155,140,141,142,160,161,162,164,165,166,168,169,170,176,177,178,180,181,182,184,185,186,163,167,171,179,183,187,172,173,174,192,193,194,196,197,198,200,201,202,208,209,210,212,213,214,216,217,218,195,199,203,211,215,219,204,205,206,96,97,98,100,101,102,104,105,106,112,113,114,116,117,118,120,121,122,99,103,107,115,119,123,108,109,110,224,225,226,228,229,230,232,233,234,240,241,242,244,245,246,248,249,250,227,231,235,243,247,251,236,237,238,28,29,30,60,61,62,92,93,94,156,157,158,188,189,190,220,221,222,31,63,95,159,191,223,124,125,126
};

constant int eac_mod[16][8] = {
  {-3,-6,-9,-15,2,5,8,14}, {-3,-7,-10,-13,2,6,9,12}, {-2,-5,-8,-13,1,4,7,12}, {-2,-4,-6,-13,1,3,5,12},
  {-3,-6,-8,-12,2,5,7,11}, {-3,-7,-9,-11,2,6,8,10}, {-4,-7,-8,-11,3,6,7,10}, {-3,-5,-8,-11,2,4,7,10},
  {-2,-6,-8,-10,1,5,7,9},  {-2,-5,-8,-10,1,4,7,9},  {-2,-4,-8,-10,1,3,7,9},  {-2,-5,-7,-10,1,4,6,9},
  {-3,-4,-7,-10,2,3,6,9},  {-1,-2,-3,-10,0,1,2,9},  {-4,-6,-8,-9,3,5,7,8},   {-3,-5,-7,-9,2,4,6,8}};
constant int w2u[4] = {0, 21, 43, 64};
constant int w3u[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constant int w4u[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static ulong ld64(device const uchar *b) { ulong v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | ulong(b[i]); return v; }

// ---------------------------------------------------------------- decoders (texel k = y * 4 + x)
static int x5(int v) { return (v << 3) | (v >> 2); }
static int x6(int v) { return (v << 2) | (v >> 4); }
static void dec_color4(device const uchar *b, thread int4 *px) {   // BC2/BC3 colour block: always the four-colour palette
  int c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  int3 p0 = int3(x5(c0 >> 11), x6((c0 >> 5) & 63), x5(c0 & 31)), p1 = int3(x5(c1 >> 11), x6((c1 >> 5) & 63), x5(c1 & 31));
  int3 pal[4] = {p0, p1, (2 * p0 + p1 + 1) / 3, (p0 + 2 * p1 + 1) / 3};
  uint ix = uint(b[4]) | (uint(b[5]) << 8) | (uint(b[6]) << 16) | (uint(b[7]) << 24);
  for (int k = 0; k < 16; k++) { int3 c = pal[(ix >> (2 * k)) & 3]; px[k].xyz = c; }
}
// BC4 palette scaled by 2047 / 255 (fractions kept), for the EAC encoder; also the BC3 alpha (0..255 via >> 3 scaling)
static void dec_bc4(device const uchar *b, thread float *v, float scale) {
  int a0 = b[0], a1 = b[1]; float pal[8]; pal[0] = a0; pal[1] = a1;
  if (a0 > a1) for (int k = 2; k < 8; k++) pal[k] = (float((8 - k) * a0 + (k - 1) * a1)) / 7.0f;
  else { for (int k = 2; k < 6; k++) pal[k] = (float((6 - k) * a0 + (k - 1) * a1)) / 5.0f; pal[6] = 0; pal[7] = 255; }
  ulong ix = 0; for (int i = 7; i >= 2; i--) ix = (ix << 8) | ulong(b[i]);
  for (int k = 0; k < 16; k++) v[k] = pal[(ix >> (3 * k)) & 7] * scale;
}
struct BS { ulong lo, hi; };
static int rd(thread BS &s, int n) {
  ulong m = (1ul << n) - 1; int r = int(s.lo & m);
  s.lo = (s.lo >> n) | ((s.hi & m) << (64 - n)); s.hi >>= n; return r;
}
static int interp(int a, int b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }
static int wt(int bits, int i) { return bits == 2 ? w2u[i] : bits == 3 ? w3u[i] : w4u[i]; }
static void dec_bc7(device const uchar *blk, thread int4 *px) {   // port of bcdec_bc7
  BS s; s.lo = ld64(blk); s.hi = ld64(blk + 8);
  int mode = 0; while (mode < 8 && rd(s, 1) == 0) mode++;
  if (mode >= 8) { for (int k = 0; k < 16; k++) px[k] = int4(0); return; }
  const int cbits[8] = {4, 6, 5, 7, 5, 7, 7, 5}, abits[8] = {0, 0, 0, 0, 6, 8, 7, 5};
  const int pbm = 0xCB;   // modes with p-bits
  int np = 1, part = 0, rot = 0, isb = 0;
  if (mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 7) { np = (mode == 0 || mode == 2) ? 3 : 2; part = rd(s, mode == 0 ? 4 : 6); }
  int ne = np * 2;
  if (mode == 4 || mode == 5) { rot = rd(s, 2); if (mode == 4) isb = rd(s, 1); }
  int ep[6][4];
  for (int j = 0; j < 6; j++) for (int c = 0; c < 4; c++) ep[j][c] = 0;
  for (int c = 0; c < 3; c++) for (int j = 0; j < ne; j++) ep[j][c] = rd(s, cbits[mode]);
  if (abits[mode]) for (int j = 0; j < ne; j++) ep[j][3] = rd(s, abits[mode]);
  if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
    for (int j = 0; j < ne; j++) for (int c = 0; c < 4; c++) ep[j][c] <<= 1;
    if (mode == 1) { int i0 = rd(s, 1), j0 = rd(s, 1); for (int c = 0; c < 3; c++) { ep[0][c] |= i0; ep[1][c] |= i0; ep[2][c] |= j0; ep[3][c] |= j0; } }
    else if ((pbm >> mode) & 1) for (int j = 0; j < ne; j++) { int p = rd(s, 1); for (int c = 0; c < 4; c++) ep[j][c] |= p; }
  }
  for (int j = 0; j < ne; j++) {
    int n = cbits[mode] + ((pbm >> mode) & 1);
    for (int c = 0; c < 3; c++) { int e = ep[j][c] << (8 - n); ep[j][c] = (e | (e >> n)) & 255; }
    n = abits[mode] + ((pbm >> mode) & 1);
    if (n > 0) { int e = ep[j][3] << (8 - n); ep[j][3] = (e | (e >> n)) & 255; }
  }
  if (!abits[mode]) for (int j = 0; j < ne; j++) ep[j][3] = 255;
  int ib = (mode == 0 || mode == 1) ? 3 : (mode == 6 ? 4 : 2), ib2 = mode == 4 ? 3 : (mode == 5 ? 2 : 0);
  int idx[16], sub[16];
  for (int k = 0; k < 16; k++) {
    int ps = np == 1 ? (k ? 0 : 128) : int(bc7_part[(np - 2) * 64 + part][k]);
    sub[k] = ps & 3; idx[k] = rd(s, ib - ((ps & 0x80) ? 1 : 0));
  }
  for (int k = 0; k < 16; k++) {
    int e = sub[k] * 2, r, g, b, a;
    if (!ib2) {
      r = interp(ep[e][0], ep[e + 1][0], wt(ib, idx[k])); g = interp(ep[e][1], ep[e + 1][1], wt(ib, idx[k]));
      b = interp(ep[e][2], ep[e + 1][2], wt(ib, idx[k])); a = interp(ep[e][3], ep[e + 1][3], wt(ib, idx[k]));
    } else {
      int i2 = rd(s, k ? ib2 : ib2 - 1);
      if (!isb) {
        r = interp(ep[e][0], ep[e + 1][0], wt(ib, idx[k])); g = interp(ep[e][1], ep[e + 1][1], wt(ib, idx[k]));
        b = interp(ep[e][2], ep[e + 1][2], wt(ib, idx[k])); a = interp(ep[e][3], ep[e + 1][3], wt(ib2, i2));
      } else {
        r = interp(ep[e][0], ep[e + 1][0], wt(ib2, i2)); g = interp(ep[e][1], ep[e + 1][1], wt(ib2, i2));
        b = interp(ep[e][2], ep[e + 1][2], wt(ib2, i2)); a = interp(ep[e][3], ep[e + 1][3], wt(ib, idx[k]));
      }
    }
    if (rot == 1) { int t = a; a = r; r = t; } else if (rot == 2) { int t = a; a = g; g = t; } else if (rot == 3) { int t = a; a = b; b = t; }
    px[k] = int4(r, g, b, a);
  }
}

// ---------------------------------------------------------------- ASTC encoder
static void put(thread uchar *o, uint pos, uint n, uint v) { for (uint i = 0; i < n; i++) if ((v >> i) & 1) o[(pos + i) >> 3] |= uchar(1u << ((pos + i) & 7)); }
static void putw(thread uchar *o, uint pos, uint n, uint v) { for (uint i = 0; i < n; i++) if ((v >> i) & 1) { uint bit = 127 - (pos + i); o[bit >> 3] |= uchar(1u << (bit & 7)); } }
static int unq48(int v) {   // colour endpoint unquantisation, range 48 (trit + 4 bits dcba): B = dcb000dcb, C = 22
  int t = v >> 4, m = v & 15, a = (m & 1) ? 0x1FF : 0, bb = (m >> 1) & 1, cc = (m >> 2) & 1, dd = (m >> 3) & 1;
  int B = (dd << 8) | (cc << 7) | (bb << 6) | (dd << 2) | (cc << 1) | bb;
  int T = t * 22 + B; T ^= a; return (a & 0x80) | (T >> 2);
}
static int q48(int x) { int best = 0, be = 1 << 20; for (int v = 0; v < 48; v++) { int d = unq48(v) - x; d *= d; if (d < be) { be = d; best = v; } } return best; }
// principal axis of the block's colours (or of all four channels, c4) by power iteration; returns the mean
static float4 fit_axis(thread const float4 *p, int c4, thread float4 &axis) {
  float4 mean = 0; for (int k = 0; k < 16; k++) mean += p[k]; mean /= 16.0f;
  if (!c4) mean.w = 0;
  float m[4][4]; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i][j] = 0;
  for (int k = 0; k < 16; k++) { float4 d = p[k] - mean; if (!c4) d.w = 0; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i][j] += d[i] * d[j]; }
  float4 v = float4(1, 1, 1, c4 ? 1 : 0); float mx = 0;
  for (int i = 0; i < 4; i++) if (m[i][i] > mx) { mx = m[i][i]; v = float4(0); v[i] = 1; }
  for (int it = 0; it < 8; it++) { float4 n = 0; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) n[i] += m[i][j] * v[j]; float l = length(n); if (l < 1e-6f) break; v = n / l; }
  axis = v; return mean;
}
// endpoints along the axis, then one least-squares refit for the chosen weights; nch channels starting at ch0
static void fit_line(thread const float4 *p, int ch0, int nch, int wbits, thread float4 &e0, thread float4 &e1) {
  float4 axis; float4 sel = 0; for (int c = ch0; c < ch0 + nch; c++) sel[c] = 1;
  float4 q[16]; for (int k = 0; k < 16; k++) q[k] = p[k] * sel;
  float4 mean = fit_axis(q, nch == 4 || ch0 == 3, axis); axis *= sel; mean *= sel;
  float lo = 1e9f, hi = -1e9f; for (int k = 0; k < 16; k++) { float t = dot(q[k] - mean, axis); lo = min(lo, t); hi = max(hi, t); }
  e0 = mean + axis * lo; e1 = mean + axis * hi;
  int n = 1 << wbits;
  for (int iter = 0; iter < 2; iter++) {   // assign weights on the line, refit the endpoints
    float4 d = e1 - e0; float dd = dot(d, d);
    float a00 = 0, a01 = 0, a11 = 0; float4 b0 = 0, b1 = 0;
    for (int k = 0; k < 16; k++) {
      float t = dd > 1e-6f ? clamp(dot(q[k] - e0, d) / dd, 0.0f, 1.0f) : 0; float w = round(t * (n - 1)) / (n - 1);
      a00 += (1 - w) * (1 - w); a01 += (1 - w) * w; a11 += w * w; b0 += (1 - w) * q[k]; b1 += w * q[k];
    }
    float det = a00 * a11 - a01 * a01;
    if (fabs(det) < 1e-6f) break;
    e0 = (b0 * a11 - b1 * a01) / det; e1 = (b1 * a00 - b0 * a01) / det;
  }
  e0 = clamp(e0, 0.0f, 255.0f) * sel; e1 = clamp(e1, 0.0f, 255.0f) * sel;
}
// best weight per texel for the (unquantised) integer endpoints over channels ch0..ch0+nch-1
static void pick_weights(thread const int4 *px, int ch0, int nch, int wbits, int4 E0, int4 E1, thread int *w, int stride, int off) {
  int n = 1 << wbits;
  for (int k = 0; k < 16; k++) {
    int best = 0, be = 1 << 30;
    for (int i = 0; i < n; i++) {
      int u = wt(wbits, i), e = 0;
      for (int c = ch0; c < ch0 + nch; c++) { int d = interp(E0[c], E1[c], u) - px[k][c]; e += d * d; }
      if (e < be) { be = e; best = i; }
    }
    w[k * stride + off] = best;
  }
}
static void astc_encode(thread const int4 *px, thread uchar *o) {
  for (int i = 0; i < 16; i++) o[i] = 0;
  int amin = 255, amax = 0; for (int k = 0; k < 16; k++) { amin = min(amin, px[k].w); amax = max(amax, px[k].w); }
  float4 p[16]; for (int k = 0; k < 16; k++) p[k] = float4(px[k]);
  int w[32];
  if (amin == amax) {   // single plane
    int opaque = amin == 255, wbits = opaque ? 3 : 2, cem = opaque ? 8 : 12;
    float4 f0, f1; fit_line(p, 0, 3, wbits, f0, f1);
    int4 E0 = int4(round(f0)), E1 = int4(round(f1)); E0.w = E1.w = amin;
    if (E1.x + E1.y + E1.z < E0.x + E0.y + E0.z) { int4 t = E0; E0 = E1; E1 = t; }   // no blue contraction
    pick_weights(px, 0, 3, wbits, E0, E1, w, 1, 0);
    put(o, 0, 11, opaque ? 0x053u : 0x042u); put(o, 13, 4, uint(cem));
    uint pos = 17; int nc = opaque ? 3 : 4;
    for (int c = 0; c < nc; c++) { put(o, pos, 8, uint(E0[c])); put(o, pos + 8, 8, uint(E1[c])); pos += 16; }
    for (int k = 0; k < 16; k++) putw(o, uint(k * wbits), uint(wbits), uint(w[k]));
    return;
  }
  // dual plane: RGB on plane 0, alpha on plane 1; endpoints in the 48-level range
  float4 f0, f1, g0, g1; fit_line(p, 0, 3, 2, f0, f1); fit_line(p, 3, 1, 2, g0, g1);   // colour line, alpha line (least squares)
  if (g0.w > g1.w) { float4 t = g0; g0 = g1; g1 = t; }
  int v[8] = {q48(int(round(f0.x))), q48(int(round(f1.x))), q48(int(round(f0.y))), q48(int(round(f1.y))),
              q48(int(round(f0.z))), q48(int(round(f1.z))), q48(int(round(g0.w))), q48(int(round(g1.w)))};
  int4 E0 = int4(unq48(v[0]), unq48(v[2]), unq48(v[4]), unq48(v[6])), E1 = int4(unq48(v[1]), unq48(v[3]), unq48(v[5]), unq48(v[7]));
  if (E1.x + E1.y + E1.z < E0.x + E0.y + E0.z) {   // decoder would swap (and blue-contract): swap all four channels ourselves
    for (int c = 0; c < 4; c++) { int t = v[2 * c]; v[2 * c] = v[2 * c + 1]; v[2 * c + 1] = t; }
    int4 t = E0; E0 = E1; E1 = t;
  }
  pick_weights(px, 0, 3, 2, E0, E1, w, 2, 0);
  pick_weights(px, 3, 1, 2, E0, E1, w, 2, 1);
  put(o, 0, 11, 0x442u); put(o, 13, 4, 12u);
  // ISE: values 0-4 then 5-7 (trits padded with 0); per block m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7]
  uint pos = 17;
  for (int blk = 0; blk < 2; blk++) {
    int b0 = blk * 5, cnt = blk ? 3 : 5, t[5] = {0, 0, 0, 0, 0}, m[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < cnt; i++) { t[i] = v[b0 + i] >> 4; m[i] = v[b0 + i] & 15; }
    uint T = trit_enc[t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4]];
    const int tb[5] = {2, 2, 1, 2, 1}; int ts = 0;
    for (int i = 0; i < cnt; i++) { put(o, pos, 4, uint(m[i])); pos += 4; put(o, pos, uint(tb[i]), (T >> ts) & ((1u << tb[i]) - 1)); pos += uint(tb[i]); ts += tb[i]; }
  }
  for (int i = 0; i < 32; i++) putw(o, uint(i * 2), 2u, uint(w[i]));
  put(o, 128 - 64 - 2, 2, 3u);   // colour component selector: alpha on plane 1
}

// ---------------------------------------------------------------- EAC R11 encoder (targets in 0..2047)
static int eac_dec(int base, int mul, int tab, int i) {
  int mod = eac_mod[tab][i]; int v = base * 8 + 4 + (mul ? mod * mul * 8 : mod); return clamp(v, 0, 2047);
}
static void eac_encode(thread const float *t, thread uchar *o) {
  float mn = 2047, mx = 0; for (int k = 0; k < 16; k++) { mn = min(mn, t[k]); mx = max(mx, t[k]); }
  float best = 1e30f; int bb = 0, bm = 1, bt = 0;
  for (int tab = 0; tab < 16; tab++) {
    int lo = eac_mod[tab][3], hi = eac_mod[tab][7];
    float me = (mx - mn) / (8.0f * float(hi - lo));
    int m0 = clamp(int(floor(me)), 1, 15);
    for (int mul = max(m0 - 1, 1); mul <= min(m0 + 2, 15); mul++) {
      float be = ((mx + mn) * 0.5f - 4.0f - float(mul * 8) * float(lo + hi) * 0.5f) / 8.0f;
      int b0 = clamp(int(round(be)), 0, 255);
      for (int base = max(b0 - 1, 0); base <= min(b0 + 1, 255); base++) {
        float err = 0;
        for (int k = 0; k < 16 && err < best; k++) { float e = 1e30f; for (int i = 0; i < 8; i++) { float d = float(eac_dec(base, mul, tab, i)) - t[k]; e = min(e, d * d); } err += e; }
        if (err < best) { best = err; bb = base; bm = mul; bt = tab; }
      }
    }
  }
  ulong bits = (ulong(bb) << 56) | (ulong(bm) << 52) | (ulong(bt) << 48);
  for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) {
    float tk = t[y * 4 + x]; int bi = 0; float e = 1e30f;
    for (int i = 0; i < 8; i++) { float d = float(eac_dec(bb, bm, bt, i)) - tk; if (d * d < e) { e = d * d; bi = i; } }
    bits |= ulong(bi) << (45 - 3 * (x * 4 + y));
  }
  for (int i = 0; i < 8; i++) o[i] = uchar(bits >> (56 - 8 * i));   // big-endian
}

kernel void bctc(device const uchar *src [[buffer(0)]], device const TP &p [[buffer(1)]], device uchar *dst [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {
  uint bw = (p.w + 3) / 4, bh = (p.h + 3) / 4; if (g.x >= bw || g.y >= bh) return;
  uint f = p.fmt & 0xff, raw = p.fmt & 0x100;
  int sbytes = (f == 0x49) ? 8 : 16;
  device const uchar *b = src + p.src_off + g.y * p.src_pitch + g.x * uint(sbytes);
  uchar o[16]; for (int i = 0; i < 16; i++) o[i] = 0;
  int obytes = 16;
  if (f == 0x49 || f == 0x4b) {   // BC4 / BC5 unsigned -> EAC R11 / RG11
    float t[16];
    dec_bc4(b, t, 2047.0f / 255.0f); eac_encode(t, o);
    if (f == 0x49) obytes = 8;
    else { uchar o2[16]; dec_bc4(b + 8, t, 2047.0f / 255.0f); eac_encode(t, o2); for (int i = 0; i < 8; i++) o[8 + i] = o2[i]; }
  } else {
    int4 px[16];
    if (f == 0x4d || f == 0x4e) dec_bc7(b, px);
    else {
      dec_color4(b + 8, px);
      if (f == 0x43 || f == 0x47) { for (int k = 0; k < 16; k++) { int a = (b[k >> 1] >> ((k & 1) * 4)) & 15; px[k].w = a * 17; } }   // BC2: explicit 4-bit alpha
      else { float a[16]; dec_bc4(b, a, 1.0f); for (int k = 0; k < 16; k++) px[k].w = int(a[k] + 0.5f); }
    }
    if (raw) {   // probe: decoded texels, 64 bytes per block
      device uchar *d = dst + p.dst_off + (g.y * bw + g.x) * 64;
      for (int k = 0; k < 16; k++) { d[4 * k] = uchar(px[k].x); d[4 * k + 1] = uchar(px[k].y); d[4 * k + 2] = uchar(px[k].z); d[4 * k + 3] = uchar(px[k].w); }
      return;
    }
    astc_encode(px, o);
  }
  device uchar *d = dst + p.dst_off + (g.y * bw + g.x) * uint(obytes);
  for (int i = 0; i < obytes; i++) d[i] = o[i];
}
