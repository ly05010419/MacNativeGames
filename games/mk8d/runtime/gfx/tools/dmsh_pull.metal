// Vertex pulling for the <key>_bp variant of a vertex shader (dmsh_pull.py): the attribute words Maxwell's vertex fetch gives the shader, read from a per-draw layout block.
// The host fills dm_vlayout for each draw (nvn_mtl.m): per stream the base address (kept 4-byte aligned: a base's low bits go into its attributes' offsets), stride (0: every vertex
// reads element 0) and divisor (0: per vertex; else the element is base instance + (instance - base instance) / divisor, Metal's own rule for a step rate with a base instance;
// the base instance is the draw's [[base_instance]], so indirect draws, whose arguments the CPU never reads, are right too), and per NVN attribute the raw NVN attribute state word
// (format 8 bits, offset 19, stream 5; format 0 or one this table does not know reads zeros, as the host's zero buffer).
// The result is four 32-bit register words: float formats as float bits (unorm and snorm through the unpack builtins, which round as Metal's vertex fetch does; a plain division
// under fast math is a unit in the last place off for some values), integer formats zero- or sign-extended, the int-to-float formats converted, missing components 0, 0, 0 and
// 1 (1.0 for the float formats). The input then takes them with as_type, whatever type the translator gave it. Bit-identical to Metal's fetch for every format it has (t_pull.m).
// Table byte per NVN vertex format: (components - 1) | size code << 2 (0: 8, 1: 16, 2: 32 bits) | kind << 4; kinds 1 unorm, 2 snorm, 3 uint, 4 sint, 5 float, 6 / 7 unsigned /
// signed integer read as float, 8 unorm 10:10:10:2, 9 snorm 10:10:10:2, 10 R11G11B10F, 11 BGRA8 unorm. The codes are those nvn_mtl.m vfmt knows (t_pull.m checks every one)
struct dm_vstream { device const uchar *base; uint stride; uint divisor; };
struct dm_vlayout { dm_vstream s[16]; uint attr[16]; };
constant uchar dm_ftab[256] = {
      0,  16,  32,  48,  64,  84,  20,  36,  52,  68,  88,  56,  72,  17,  33,  49,
     65,  85,  21,  37,  53,  69,  89,  57,  73,  18,  34,  50,  66,  86,  22,  38,
     54,  70,  90,  58,  74,  19,  35,  51,  67,  87,  23,  39,  55,  71,  91,  59,
     75,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 139,   0, 170,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,  96, 112, 100, 116, 104, 120,  97, 113, 101, 117, 105, 121,  98, 114, 102,
    118, 106, 122,  99, 115, 103, 119, 107, 123, 155,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 179,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};
// R11G11B10F component: 5-bit exponent, mb-bit mantissa, no sign; a NaN comes out as the canonical one, as from Metal's fetch
static inline uint dm_uf(uint m, uint mb) {
    uint e = m >> mb, mm = m & ((1u << mb) - 1u);
    return e == 31u ? (mm ? 0x7fc00000u : 0x7f800000u) : e == 0u ? as_type<uint>(float(mm) * exp2(-14.0f - float(mb))) : (((e + 112u) << 23) | (mm << (23u - mb)));
}
// p: the element; al: its address is a multiple of 4 (a whole-word read then stays inside the vertex's stride)
static inline uint4 dm_decode(device const uchar *p, uint fmt, bool al) {
    uint t = dm_ftab[fmt], kind = t >> 4, n = (t & 3u) + 1u, sz = (t >> 2) & 3u;
    if (kind == 0u) return uint4(0u);
    uint4 w = uint4(0u);   // 32-bit components, or the packed bytes / halves: w.x = components 0-3 (8-bit) or 0-1 (16-bit), w.y = 2-3 (16-bit)
    if (kind >= 8u && kind <= 10u) w.x = *(device const uint *)p;
    else if (sz == 2u) {
        if (n == 4u) w = uint4(*(device const packed_uint4 *)p);
        else if (n == 3u) w.xyz = uint3(*(device const packed_uint3 *)p);
        else if (n == 2u) w.xy = uint2(*(device const packed_uint2 *)p);
        else w.x = *(device const uint *)p;
    } else if (al) {
        if (sz == 1u && n > 2u) w.xy = uint2(*(device const packed_uint2 *)p); else w.x = *(device const uint *)p;
    } else if (sz == 1u) {
        device const ushort *q = (device const ushort *)p;
        w.x = uint(q[0]) | (n > 1u ? uint(q[1]) << 16 : 0u); if (n > 2u) w.y = uint(q[2]) | (n > 3u ? uint(q[3]) << 16 : 0u);
    } else {
        w.x = uint(p[0]) | (n > 1u ? uint(p[1]) << 8 : 0u) | (n > 2u ? uint(p[2]) << 16 : 0u) | (n > 3u ? uint(p[3]) << 24 : 0u);
    }
    if (kind >= 8u && kind <= 10u) {
        uint v = w.x;
        if (kind == 8u) return as_type<uint4>(unpack_unorm10a2_to_float(v));
        if (kind == 9u) {
            int4 s = int4(int(v << 22) >> 22, int(v << 12) >> 22, int(v << 2) >> 22, int(v) >> 30);
            return as_type<uint4>(max(float4(s) / float4(511.0f, 511.0f, 511.0f, 1.0f), -1.0f));
        }
        return uint4(dm_uf(v & 2047u, 6u), dm_uf((v >> 11) & 2047u, 6u), dm_uf(v >> 22, 5u), 0x3f800000u);
    }
    uint4 c = sz == 2u ? w : sz == 1u ? uint4(w.x & 0xffffu, w.x >> 16, w.y & 0xffffu, w.y >> 16) : uint4(w.x & 0xffu, (w.x >> 8) & 0xffu, (w.x >> 16) & 0xffu, w.x >> 24);
    uint sh = 32u - (8u << sz);
    uint4 r;
    switch (kind) {
        case 1u: r = as_type<uint4>(sz == 0u ? unpack_unorm4x8_to_float(w.x) : float4(unpack_unorm2x16_to_float(w.x), unpack_unorm2x16_to_float(w.y))); break;
        case 2u: r = as_type<uint4>(sz == 0u ? unpack_snorm4x8_to_float(w.x) : float4(unpack_snorm2x16_to_float(w.x), unpack_snorm2x16_to_float(w.y))); break;
        case 4u: r = as_type<uint4>(as_type<int4>(c << sh) >> sh); break;
        case 5u: r = sz == 1u ? as_type<uint4>(float4(as_type<half4>(ushort4(c)))) : c; break;
        case 6u: r = as_type<uint4>(float4(c)); break;
        case 7u: r = as_type<uint4>(float4(as_type<int4>(c << sh) >> sh)); break;
        case 11u: r = as_type<uint4>(unpack_unorm4x8_to_float(w.x).zyxw); break;
        default: r = c; break;   // 3: uint
    }
    return select(uint4(0u, 0u, 0u, kind == 3u || kind == 4u ? 1u : 0x3f800000u), r, uint4(0u, 1u, 2u, 3u) < uint4(n));
}
// NVN attribute a of this draw, for vertex vid of instance iid ([[vertex_id]] and [[instance_id]], which include the base vertex and base instance; bi: [[base_instance]])
static inline uint4 dm_fetch(constant dm_vlayout &L, uint a, uint vid, uint iid, uint bi) {
    uint w = L.attr[a], off = (w >> 8) & 0x7ffffu, st = w >> 27, stride = L.s[st].stride, div = L.s[st].divisor;
    uint idx = stride == 0u ? 0u : div != 0u ? (iid - bi) / div + bi : vid;
    uint rel = idx * stride + off;
    return dm_decode(L.s[st].base + rel, w & 0xffu, (rel & 3u) == 0u);
}
