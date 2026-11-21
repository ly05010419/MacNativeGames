// Indirect draws Metal cannot take as they are: records converted to indexed indirect draws (nvn_mtl.m ixc_run)
#include <metal_stdlib>
using namespace metal;
struct ajob { device const uint *rec; device uint *args; uint prim, cap; };
kernel void ixc_arrays(device const ajob *j [[buffer(0)]], uint i [[threadgroup_position_in_grid]]) {
  device const uint *s = j[i].rec; uint n = s[0], c = j[i].prim == 7 ? n / 4 * 6 : n >= 3 ? (n - 2) * 3 : 0;
  device uint *d = j[i].args; d[0] = min(c, j[i].cap); d[1] = s[1]; d[2] = 0; d[3] = s[2]; d[4] = s[3]; }
struct ejob { device const uint *rec; device const uchar *ib; device uint *out; device uint *args; uint prim, isz, ilen, cap; };
kernel void ixc_expand(constant ejob &j [[buffer(0)]], uint i [[thread_position_in_grid]]) {
  uint n = j.rec[0], m = min(j.prim == 7 ? n / 4 * 6 : j.prim == 6 ? (n >= 3 ? (n - 2) * 3 : 0) : n, j.cap);
  if (i == 0) { j.args[0] = m; j.args[1] = j.rec[1]; j.args[2] = 0; j.args[3] = j.rec[3]; j.args[4] = j.rec[4]; }
  if (i >= m) return;
  uint t = i / 3, c = i % 3, p = j.prim == 7 ? t / 2 * 4 + (c ? c + (t & 1) : 0) : j.prim == 6 ? (c ? t + c : 0) : i, x = j.rec[2] + p;
  j.out[i] = (ulong)x * j.isz + j.isz > j.ilen ? 0u : j.isz == 1 ? uint(j.ib[x]) : j.isz == 2 ? uint(((device const ushort *)j.ib)[x]) : ((device const uint *)j.ib)[x]; }
