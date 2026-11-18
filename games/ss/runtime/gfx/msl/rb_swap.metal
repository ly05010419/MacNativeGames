// The RGB565 family's red / blue exchange on the GPU, for copies between guest memory and a host texture (nvn_mtl.m rb_swap_gpu)
#include <metal_stdlib>
using namespace metal;
struct P { device const uchar *s; device uchar *d; uint w, h, row, img, drow, dimg, shift; };
kernel void rb_swap(constant P &p [[buffer(0)]], uint3 g [[thread_position_in_grid]]) {
  if (g.x >= p.w || g.y >= p.h) return;
  ushort v = ((device const ushort *)(p.s + g.z * p.img + g.y * p.row))[g.x], m = ushort(31u | 31u << p.shift);
  ((device ushort *)(p.d + g.z * p.dimg + g.y * p.drow))[g.x] = ushort((v & ~m) | (v & 31u) << p.shift | (v >> p.shift & 31u)); }
