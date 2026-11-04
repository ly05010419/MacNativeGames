/*
 * scenecap .nvs container: writer and reader. See nvs_format.h for the layout and the format contract.
 *
 * Dependencies: C11 only. The built-in zstd encoder emits valid RFC 8878 frames made of raw and RLE blocks, so the
 * module needs no compression library; real compression is a hook (nvs_writer_set_compressor) and the matching
 * decoder is a hook (nvs_read_set_decompressor).
 */
#include "nvs_format.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NVS_MANIFEST_LIMIT (64ull * 1024ull * 1024ull)   /* 64 MiB of JSON is far beyond any real scene */
#define NVS_RAW_LIMIT (16ull * 1024ull * 1024ull * 1024ull) /* 16 GiB per blob */
#define NVS_JSON_MAX_DEPTH 256u

/* --------------------------------------------------------------------------------------------------------------- */
/* small helpers                                                                                                   */
/* --------------------------------------------------------------------------------------------------------------- */

static void set_error(nvs_error *err, int code, const char *fmt, ...)
{
    va_list ap;
    if (!err) return;
    err->code = code;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
}

static void clear_error(nvs_error *err)
{
    if (!err) return;
    err->code = NVS_OK;
    err->message[0] = '\0';
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xffffffffu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* SHA-256 digests and message words are big-endian; the container is little-endian. */
static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* Chunk ids are restricted so that they can be embedded in JSON and in the chunk table without escaping. */
static int id_valid(const char *id)
{
    size_t i;
    if (!id || !id[0]) return 0;
    for (i = 0; id[i]; i++) {
        unsigned char c = (unsigned char)id[i];
        if (i >= NVS_CHUNK_ID_MAX - 1) return 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (c == '.' || c == '_' || c == ':' || c == '/' || c == '-') continue;
        return 0;
    }
    return 1;
}

const char *nvs_kind_name(nvs_kind kind)
{
    switch (kind) {
    case NVS_KIND_JSON: return "json";
    case NVS_KIND_MSL: return "msl";
    case NVS_KIND_REFLECTION: return "reflection";
    case NVS_KIND_TEXTURE: return "texture";
    case NVS_KIND_BUFFER: return "buffer";
    case NVS_KIND_BINARY: return "binary";
    default: return "";
    }
}

int nvs_kind_from_name(const char *name, nvs_kind *out)
{
    if (!name || !out) return 0;
    if (!strcmp(name, "json")) { *out = NVS_KIND_JSON; return 1; }
    if (!strcmp(name, "msl")) { *out = NVS_KIND_MSL; return 1; }
    if (!strcmp(name, "reflection")) { *out = NVS_KIND_REFLECTION; return 1; }
    if (!strcmp(name, "texture")) { *out = NVS_KIND_TEXTURE; return 1; }
    if (!strcmp(name, "buffer")) { *out = NVS_KIND_BUFFER; return 1; }
    if (!strcmp(name, "binary")) { *out = NVS_KIND_BINARY; return 1; }
    return 0;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4)                                                                                            */
/* --------------------------------------------------------------------------------------------------------------- */

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(nvs_sha256 *ctx, const uint8_t block[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) w[i] = get_u32_be(block + 4 * i);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t s0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + s1 + ch + sha256_k[i] + w[i];
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void nvs_sha256_init(nvs_sha256 *ctx)
{
    static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(ctx->state, init, sizeof init);
    ctx->bits = 0;
    ctx->buffered = 0;
}

void nvs_sha256_update(nvs_sha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bits += (uint64_t)len * 8u;
    if (ctx->buffered) {
        size_t take = 64 - ctx->buffered;
        if (take > len) take = len;
        memcpy(ctx->buffer + ctx->buffered, p, take);
        ctx->buffered += take;
        p += take;
        len -= take;
        if (ctx->buffered == 64) {
            sha256_block(ctx, ctx->buffer);
            ctx->buffered = 0;
        }
    }
    while (len >= 64) {
        sha256_block(ctx, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->buffer, p, len);
        ctx->buffered = len;
    }
}

void nvs_sha256_final(nvs_sha256 *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->bits;
    uint8_t pad[72];
    size_t padlen = (ctx->buffered < 56) ? (56 - ctx->buffered) : (120 - ctx->buffered);
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) pad[padlen + (size_t)i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xffu);
    nvs_sha256_update(ctx, pad, padlen + 8);
    for (i = 0; i < 8; i++) {
        out[4 * i + 0] = (uint8_t)(ctx->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)ctx->state[i];
    }
}

void nvs_sha256_digest(const void *data, size_t len, uint8_t out[32])
{
    nvs_sha256 ctx;
    nvs_sha256_init(&ctx);
    nvs_sha256_update(&ctx, data, len);
    nvs_sha256_final(&ctx, out);
}

void nvs_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0fu];
    }
    out[2 * len] = '\0';
}

/* --------------------------------------------------------------------------------------------------------------- */
/* built-in zstd frames (raw and RLE blocks only)                                                                  */
/* --------------------------------------------------------------------------------------------------------------- */

#define NVS_ZSTD_BLOCK 65536u

size_t nvs_zstd_bound(size_t size)
{
    size_t blocks;
    if (size > SIZE_MAX - (NVS_ZSTD_BLOCK - 1u)) return 0;
    blocks = (size + (NVS_ZSTD_BLOCK - 1u)) / NVS_ZSTD_BLOCK;
    size_t overhead;
    if (blocks == 0) blocks = 1;
    if (blocks > (SIZE_MAX - 16u) / 3u) return 0;
    overhead = blocks * 3u + 16u;
    if (size > SIZE_MAX - overhead) return 0;
    return size + overhead;
}

size_t nvs_zstd_compress(void *user, const void *src, size_t src_len, void *dst, size_t dst_cap)
{
    static const uint8_t magic[4] = {0x28, 0xb5, 0x2f, 0xfd};
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t need = nvs_zstd_bound(src_len);
    size_t o = 0, pos = 0;
    (void)user;
    if (!out || !need || dst_cap < need) return 0;
    memcpy(out, magic, 4);
    o = 4;
    if (src_len < 256) {
        out[o++] = 0x20; /* single segment, FCS 1 byte, no checksum, no dictionary */
        out[o++] = (uint8_t)src_len;
    } else if (src_len < 65536u + 256u) {
        uint32_t v = (uint32_t)(src_len - 256u);
        out[o++] = 0x60; /* single segment, FCS 2 bytes: content size - 256 */
        out[o++] = (uint8_t)(v & 0xffu);
        out[o++] = (uint8_t)(v >> 8);
    } else {
        out[o++] = 0xA0; /* single segment, FCS 4 bytes */
        if (src_len > 0xffffffffu) return 0;
        put_u32(out + o, (uint32_t)src_len);
        o += 4;
    }
    if (src_len == 0) {
        out[o++] = 1; /* last block, raw, size 0 */
        out[o++] = 0;
        out[o++] = 0;
        return o;
    }
    while (pos < src_len) {
        size_t n = src_len - pos;
        int rle = 1;
        size_t i;
        uint32_t last, header;
        if (n > NVS_ZSTD_BLOCK) n = NVS_ZSTD_BLOCK;
        for (i = 1; i < n; i++) {
            if (in[pos + i] != in[pos]) { rle = 0; break; }
        }
        last = (pos + n >= src_len) ? 1u : 0u;
        header = last | ((rle ? 1u : 0u) << 1) | ((uint32_t)n << 3);
        out[o++] = (uint8_t)(header & 0xffu);
        out[o++] = (uint8_t)((header >> 8) & 0xffu);
        out[o++] = (uint8_t)((header >> 16) & 0xffu);
        if (rle) {
            out[o++] = in[pos];
        } else {
            memcpy(out + o, in + pos, n);
            o += n;
        }
        pos += n;
    }
    return o;
}

size_t nvs_zstd_decompress_ex(const void *src, size_t src_len, void *dst, size_t dst_cap, nvs_error *err)
{
    const uint8_t *in = (const uint8_t *)src;
    uint8_t *out = (uint8_t *)dst;
    size_t pos = 0, o = 0;
    uint8_t fhd;
    uint32_t fcs_flag, single, reserved, checksum, dict;
    uint64_t content_size = 0;
    int has_size = 0, last = 0;

    clear_error(err);
    if (!in || !out) {
        set_error(err, NVS_ERR_ARG, "zstd: null buffer");
        return 0;
    }
    if (src_len < 5 || in[0] != 0x28 || in[1] != 0xb5 || in[2] != 0x2f || in[3] != 0xfd) {
        set_error(err, NVS_ERR_FORMAT, "zstd: missing frame magic");
        return 0;
    }
    pos = 4;
    fhd = in[pos++];
    fcs_flag = (uint32_t)fhd >> 6;
    single = ((uint32_t)fhd >> 5) & 1u;
    reserved = ((uint32_t)fhd >> 3) & 1u;
    checksum = ((uint32_t)fhd >> 2) & 1u;
    dict = (uint32_t)fhd & 3u;
    if (reserved) {
        set_error(err, NVS_ERR_FORMAT, "zstd: reserved frame-header bit set");
        return 0;
    }
    if (checksum) {
        set_error(err, NVS_ERR_UNSUPPORTED, "zstd: frame checksum needs an external decoder");
        return 0;
    }
    if (dict) {
        set_error(err, NVS_ERR_UNSUPPORTED, "zstd: dictionary frames are not supported");
        return 0;
    }
    if (!single) {
        if (pos >= src_len) {
            set_error(err, NVS_ERR_FORMAT, "zstd: truncated window descriptor");
            return 0;
        }
        pos++;
    }
    if (fcs_flag == 0) {
        if (single) {
            if (pos >= src_len) {
                set_error(err, NVS_ERR_FORMAT, "zstd: truncated content size");
                return 0;
            }
            content_size = in[pos++];
            has_size = 1;
        }
    } else if (fcs_flag == 1) {
        if (pos + 2 > src_len) {
            set_error(err, NVS_ERR_FORMAT, "zstd: truncated content size");
            return 0;
        }
        content_size = (uint64_t)get_u16(in + pos) + 256u;
        pos += 2;
        has_size = 1;
    } else if (fcs_flag == 2) {
        if (pos + 4 > src_len) {
            set_error(err, NVS_ERR_FORMAT, "zstd: truncated content size");
            return 0;
        }
        content_size = get_u32(in + pos);
        pos += 4;
        has_size = 1;
    } else {
        if (pos + 8 > src_len) {
            set_error(err, NVS_ERR_FORMAT, "zstd: truncated content size");
            return 0;
        }
        content_size = get_u64(in + pos);
        pos += 8;
        has_size = 1;
    }
    if (has_size && content_size > dst_cap) {
        set_error(err, NVS_ERR_TOO_LARGE, "zstd: frame holds %llu bytes, buffer is %zu",
                  (unsigned long long)content_size, dst_cap);
        return 0;
    }
    while (!last) {
        uint32_t header;
        size_t block_size, type;
        if (pos + 3 > src_len) {
            set_error(err, NVS_ERR_FORMAT, "zstd: truncated block header");
            return 0;
        }
        header = (uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) | ((uint32_t)in[pos + 2] << 16);
        pos += 3;
        last = (int)(header & 1u);
        type = (header >> 1) & 3u;
        block_size = (size_t)(header >> 3);
        if (type == 3) {
            set_error(err, NVS_ERR_FORMAT, "zstd: reserved block type");
            return 0;
        }
        if (type == 2) {
            set_error(err, NVS_ERR_UNSUPPORTED, "zstd: compressed block needs an external decoder");
            return 0;
        }
        if (type == 0) {
            if (block_size > src_len - pos || block_size > dst_cap - o) {
                set_error(err, NVS_ERR_FORMAT, "zstd: raw block out of bounds");
                return 0;
            }
            memcpy(out + o, in + pos, block_size);
            pos += block_size;
            o += block_size;
        } else {
            if (pos >= src_len || block_size > dst_cap - o) {
                set_error(err, NVS_ERR_FORMAT, "zstd: RLE block out of bounds");
                return 0;
            }
            memset(out + o, in[pos], block_size);
            pos++;
            o += block_size;
        }
    }
    if (has_size && content_size != (uint64_t)o) {
        set_error(err, NVS_ERR_FORMAT, "zstd: decoded %zu bytes, frame declares %llu", o,
                  (unsigned long long)content_size);
        return 0;
    }
    return o;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* JSON object scanner: the writer embeds the caller manifest verbatim, so it insists on exactly one object      */
/* --------------------------------------------------------------------------------------------------------------- */

static int json_is_single_object(const char *json, size_t len, nvs_error *err)
{
    size_t i = 0, depth = 0, end = 0;
    int in_string = 0, escaped = 0, seen = 0;
    if (!json) {
        set_error(err, NVS_ERR_ARG, "manifest json is null");
        return 0;
    }
    while (i < len) {
        unsigned char c = (unsigned char)json[i];
        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_string = 0;
            } else if (c < 0x20) {
                set_error(err, NVS_ERR_ARG, "manifest json has a raw control character at byte %zu", i);
                return 0;
            }
            i++;
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{' || c == '[') {
            depth++;
            if (!seen && c != '{') {
                set_error(err, NVS_ERR_ARG, "manifest json must start with an object");
                return 0;
            }
            if (depth > NVS_JSON_MAX_DEPTH) {
                set_error(err, NVS_ERR_ARG, "manifest json nests deeper than %u", NVS_JSON_MAX_DEPTH);
                return 0;
            }
            seen = 1;
        } else if (c == '}' || c == ']') {
            if (!depth) {
                set_error(err, NVS_ERR_ARG, "manifest json closes more than it opens at byte %zu", i);
                return 0;
            }
            depth--;
            if (!depth) end = i;
        } else if (c > 0x20) {
            if (!seen) {
                set_error(err, NVS_ERR_ARG, "manifest json must start with an object");
                return 0;
            }
            if (end) {
                set_error(err, NVS_ERR_ARG, "manifest json has trailing data after the object");
                return 0;
            }
        }
        i++;
    }
    if (in_string || escaped) {
        set_error(err, NVS_ERR_ARG, "manifest json has an unterminated string");
        return 0;
    }
    if (!seen || depth != 0) {
        set_error(err, NVS_ERR_ARG, "manifest json is not one complete object");
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* growable byte buffer                                                                                            */
/* --------------------------------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} nvs_buf;

static int buf_reserve(nvs_buf *buf, size_t extra)
{
    size_t need;
    if (extra > SIZE_MAX - buf->len) return 0;
    need = buf->len + extra;
    if (need <= buf->cap) return 1;
    if (!buf->data) {
        buf->cap = need < 256 ? 256 : need;
        buf->data = (uint8_t *)malloc(buf->cap);
        return buf->data != NULL;
    }
    {
        size_t cap = buf->cap;
        uint8_t *grown;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) return 0;
            cap *= 2;
        }
        grown = (uint8_t *)realloc(buf->data, cap);
        if (!grown) return 0;
        buf->data = grown;
        buf->cap = cap;
    }
    return 1;
}

static int buf_append(nvs_buf *buf, const void *bytes, size_t len)
{
    if (!len) return 1;
    if (!buf_reserve(buf, len)) return 0;
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    return 1;
}

static int buf_append_str(nvs_buf *buf, const char *text)
{
    return buf_append(buf, text, strlen(text));
}

static int buf_appendf(nvs_buf *buf, const char *fmt, ...)
{
    va_list ap;
    int need;
    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return 0;
    if (!buf_reserve(buf, (size_t)need + 1)) return 0;
    va_start(ap, fmt);
    vsnprintf((char *)buf->data + buf->len, (size_t)need + 1, fmt, ap);
    va_end(ap);
    buf->len += (size_t)need;
    return 1;
}

/* JSON-escape a caller string (generator label). */
static int buf_append_json_string(nvs_buf *buf, const char *text)
{
    size_t i;
    if (!buf_append_str(buf, "\"")) return 0;
    for (i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            if (!buf_append(buf, pair, 2)) return 0;
        } else if (c < 0x20) {
            if (!buf_appendf(buf, "\\u%04x", c)) return 0;
        } else {
            if (!buf_append(buf, &c, 1)) return 0;
        }
    }
    return buf_append_str(buf, "\"");
}

/* --------------------------------------------------------------------------------------------------------------- */
/* writer                                                                                                          */
/* --------------------------------------------------------------------------------------------------------------- */

typedef struct {
    char id[NVS_CHUNK_ID_MAX];
    nvs_kind kind;
    uint32_t flags;
    uint8_t *stored;
    uint64_t stored_size;
    uint64_t raw_size;
    uint8_t sha256[32];
    uint64_t offset;
} nvs_entry;

struct nvs_writer {
    char *path;
    char generator[128];
    nvs_compress_fn compress;
    void *compress_user;
    nvs_entry *entries;
    size_t count, capacity;
    uint8_t *manifest_stored;
    uint64_t manifest_stored_size;
    uint8_t *manifest_raw;
    size_t manifest_raw_len;
    uint64_t manifest_raw_size;
    uint8_t manifest_sha256[32];
    int manifest_zstd;
    int has_manifest;
};

static void writer_free(nvs_writer *writer)
{
    size_t i;
    if (!writer) return;
    for (i = 0; i < writer->count; i++) free(writer->entries[i].stored);
    free(writer->entries);
    free(writer->manifest_stored);
    free(writer->manifest_raw);
    free(writer->path);
    free(writer);
}

nvs_writer *nvs_writer_create(const char *path, const nvs_write_options *options, nvs_error *err)
{
    nvs_writer *writer;
    clear_error(err);
    if (!path || !path[0]) {
        set_error(err, NVS_ERR_ARG, "package path is empty");
        return NULL;
    }
    writer = (nvs_writer *)calloc(1, sizeof *writer);
    if (!writer) {
        set_error(err, NVS_ERR_NOMEM, "nvs_writer_create: out of memory");
        return NULL;
    }
    writer->path = strdup(path);
    if (!writer->path) {
        set_error(err, NVS_ERR_NOMEM, "nvs_writer_create: out of memory");
        writer_free(writer);
        return NULL;
    }
    snprintf(writer->generator, sizeof writer->generator, "%s", "scenecap-format/1.0");
    if (options && options->generator && options->generator[0]) {
        snprintf(writer->generator, sizeof writer->generator, "%s", options->generator);
    }
    writer->compress = nvs_zstd_compress;
    writer->compress_user = NULL;
    if (options && options->compress) {
        writer->compress = options->compress;
        writer->compress_user = options->compress_user;
    }
    return writer;
}

void nvs_writer_set_compressor(nvs_writer *writer, nvs_compress_fn compress, void *user)
{
    if (!writer) return;
    writer->compress = compress ? compress : nvs_zstd_compress;
    writer->compress_user = compress ? user : NULL;
}

static int find_entry(const nvs_writer *writer, const char *id)
{
    size_t i;
    for (i = 0; i < writer->count; i++) {
        if (!strcmp(writer->entries[i].id, id)) return (int)i;
    }
    return -1;
}

/* Compress `raw` into an owned buffer, storing raw bytes when the frame would only grow the package. */
static int store_block(nvs_writer *writer, const void *raw, size_t raw_len, int allow_zstd, uint8_t **out,
                       uint64_t *stored_size, int *used_zstd, nvs_error *err)
{
    uint8_t *buffer;
    size_t bound;
    *out = NULL;
    *stored_size = 0;
    *used_zstd = 0;
    if (raw_len > NVS_RAW_LIMIT) {
        set_error(err, NVS_ERR_TOO_LARGE, "block is %zu bytes, the limit is %llu", raw_len,
                  (unsigned long long)NVS_RAW_LIMIT);
        return 0;
    }
    if (allow_zstd) {
        bound = nvs_zstd_bound(raw_len);
        if (!bound) {
            set_error(err, NVS_ERR_TOO_LARGE, "block is too large to encode");
            return 0;
        }
        buffer = (uint8_t *)malloc(bound);
        if (!buffer) {
            set_error(err, NVS_ERR_NOMEM, "cannot allocate %zu bytes for compression", bound);
            return 0;
        }
        {
            size_t produced = writer->compress(writer->compress_user, raw, raw_len, buffer, bound);
            if (!produced) {
                free(buffer);
                set_error(err, NVS_ERR_UNSUPPORTED, "the installed zstd compressor produced no output");
                return 0;
            }
            if (produced < raw_len) {
                *out = buffer;
                *stored_size = (uint64_t)produced;
                *used_zstd = 1;
                return 1;
            }
        }
        free(buffer);
    }
    buffer = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!buffer) {
        set_error(err, NVS_ERR_NOMEM, "cannot allocate %zu bytes", raw_len);
        return 0;
    }
    if (raw_len) memcpy(buffer, raw, raw_len);
    *out = buffer;
    *stored_size = (uint64_t)raw_len;
    *used_zstd = 0;
    return 1;
}

int nvs_writer_add_blob(nvs_writer *writer, const nvs_blob_input *blob, nvs_error *err)
{
    uint8_t *stored = NULL;
    uint64_t stored_size = 0;
    int used_zstd = 0;
    nvs_entry *entry;
    clear_error(err);
    if (!writer || !blob) {
        set_error(err, NVS_ERR_ARG, "nvs_writer_add_blob: null writer or blob");
        return NVS_ERR_ARG;
    }
    if (!id_valid(blob->id)) {
        set_error(err, NVS_ERR_ARG, "invalid blob id \"%s\" (1..63 bytes of [A-Za-z0-9._:/-])",
                  blob->id ? blob->id : "");
        return NVS_ERR_ARG;
    }
    if (!nvs_kind_name(blob->kind)[0]) {
        set_error(err, NVS_ERR_ARG, "blob \"%s\": invalid kind %d", blob->id, (int)blob->kind);
        return NVS_ERR_ARG;
    }
    if (!blob->data && blob->size) {
        set_error(err, NVS_ERR_ARG, "blob \"%s\": null data with %zu bytes", blob->id, blob->size);
        return NVS_ERR_ARG;
    }
    if (find_entry(writer, blob->id) >= 0) {
        set_error(err, NVS_ERR_ARG, "blob \"%s\" was added twice", blob->id);
        return NVS_ERR_ARG;
    }
    if (!store_block(writer, blob->data, blob->size, !(blob->flags & NVS_BLOB_STORE_RAW), &stored, &stored_size,
                     &used_zstd, err)) {
        return err ? err->code : NVS_ERR_NOMEM;
    }
    if (writer->count == writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity * 2 : 32;
        nvs_entry *grown = (nvs_entry *)realloc(writer->entries, capacity * sizeof *grown);
        if (!grown) {
            free(stored);
            set_error(err, NVS_ERR_NOMEM, "cannot grow the chunk directory");
            return NVS_ERR_NOMEM;
        }
        writer->entries = grown;
        writer->capacity = capacity;
    }
    entry = &writer->entries[writer->count];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->id, sizeof entry->id, "%s", blob->id);
    entry->kind = blob->kind;
    entry->flags = used_zstd ? NVS_CHUNK_FLAG_ZSTD : 0u;
    entry->stored = stored;
    entry->stored_size = stored_size;
    entry->raw_size = (uint64_t)blob->size;
    nvs_sha256_digest(stored, (size_t)stored_size, entry->sha256);
    writer->count++;
    return NVS_OK;
}

int nvs_writer_add_text(nvs_writer *writer, const char *id, nvs_kind kind, const char *text, nvs_error *err)
{
    nvs_blob_input blob;
    if (!text) {
        set_error(err, NVS_ERR_ARG, "nvs_writer_add_text: null text");
        return NVS_ERR_ARG;
    }
    blob.id = id;
    blob.kind = kind;
    blob.data = text;
    blob.size = strlen(text);
    blob.flags = 0;
    return nvs_writer_add_blob(writer, &blob, err);
}

int nvs_writer_set_manifest(nvs_writer *writer, const char *json, size_t len, nvs_error *err)
{
    uint8_t *stored = NULL;
    uint64_t stored_size = 0;
    int used_zstd = 0;
    uint8_t *raw;
    clear_error(err);
    if (!writer) {
        set_error(err, NVS_ERR_ARG, "nvs_writer_set_manifest: null writer");
        return NVS_ERR_ARG;
    }
    if (writer->has_manifest) {
        set_error(err, NVS_ERR_ARG, "manifest was already set");
        return NVS_ERR_ARG;
    }
    if (len > NVS_MANIFEST_LIMIT) {
        set_error(err, NVS_ERR_TOO_LARGE, "manifest is %zu bytes, the limit is %llu", len,
                  (unsigned long long)NVS_MANIFEST_LIMIT);
        return NVS_ERR_TOO_LARGE;
    }
    if (!json_is_single_object(json, len, err)) return err ? err->code : NVS_ERR_ARG;
    raw = (uint8_t *)malloc(len ? len : 1);
    if (!raw) {
        set_error(err, NVS_ERR_NOMEM, "cannot copy the manifest");
        return NVS_ERR_NOMEM;
    }
    if (len) memcpy(raw, json, len);
    if (!store_block(writer, json, len, 1, &stored, &stored_size, &used_zstd, err)) {
        free(raw);
        return err ? err->code : NVS_ERR_NOMEM;
    }
    writer->manifest_stored = stored;
    writer->manifest_stored_size = stored_size;
    writer->manifest_raw = raw;
    writer->manifest_raw_len = len;
    writer->manifest_raw_size = (uint64_t)len;
    writer->manifest_zstd = used_zstd;
    nvs_sha256_digest(stored, (size_t)stored_size, writer->manifest_sha256);
    writer->has_manifest = 1;
    return NVS_OK;
}

static int build_envelope(nvs_writer *writer, nvs_buf *env, uint64_t manifest_bytes, nvs_error *err)
{
    size_t i;
    if (!buf_append_str(env, "{\"nvs\":{\"format\":\"scenecap\",\"version\":\"")) goto oom;
    if (!buf_appendf(env, "%u.%u", NVS_FORMAT_MAJOR, NVS_FORMAT_MINOR)) goto oom;
    if (!buf_append_str(env, "\",\"generator\":")) goto oom;
    if (!buf_append_json_string(env, writer->generator)) goto oom;
    if (!buf_appendf(env, ",\"chunk_count\":%zu,\"manifest_bytes\":%llu},\"chunks\":[", writer->count,
                     (unsigned long long)manifest_bytes))
        goto oom;
    for (i = 0; i < writer->count; i++) {
        const nvs_entry *entry = &writer->entries[i];
        char hex[65];
        nvs_hex(entry->sha256, 32, hex);
        if (i && !buf_append_str(env, ",")) goto oom;
        if (!buf_appendf(env,
                         "{\"id\":\"%s\",\"kind\":\"%s\",\"flags\":%u,\"compression\":\"%s\","
                         "\"stored_bytes\":%llu,\"raw_bytes\":%llu,\"sha256\":\"%s\"}",
                         entry->id, nvs_kind_name(entry->kind), entry->flags,
                         (entry->flags & NVS_CHUNK_FLAG_ZSTD) ? "zstd" : "none",
                         (unsigned long long)entry->stored_size, (unsigned long long)entry->raw_size, hex))
            goto oom;
    }
    if (!buf_append_str(env, "],\"manifest\":")) goto oom;
    if (!buf_append(env, writer->manifest_raw, writer->manifest_raw_len)) goto oom;
    if (!buf_append_str(env, "}")) goto oom;
    return 1;
oom:
    set_error(err, NVS_ERR_NOMEM, "cannot build the package envelope");
    return 0;
}

static int write_all(FILE *file, const void *data, size_t len)
{
    return len == 0 || fwrite(data, 1, len, file) == len;
}

int nvs_writer_finish(nvs_writer *writer, nvs_error *err)
{
    nvs_buf env;
    uint8_t *envelope = NULL;
    uint64_t envelope_stored_size = 0;
    int envelope_zstd = 0;
    uint8_t envelope_sha256[32];
    uint8_t *envelope_compressed = NULL;
    uint8_t *header = NULL;
    uint8_t *table = NULL;
    uint64_t pos, table_offset, manifest_offset, file_size;
    FILE *file = NULL;
    char temp[4096];
    size_t i;
    int ok = 0;

    clear_error(err);
    memset(&env, 0, sizeof env);
    temp[0] = '\0';
    if (!writer) {
        set_error(err, NVS_ERR_ARG, "nvs_writer_finish: null writer");
        return NVS_ERR_ARG;
    }
    if (!writer->has_manifest) {
        set_error(err, NVS_ERR_ARG, "no manifest was set before finish");
        writer_free(writer);
        return NVS_ERR_ARG;
    }
    if (writer->count > 0xffffffffu) {
        set_error(err, NVS_ERR_TOO_LARGE, "the package holds more than 2^32 chunks");
        writer_free(writer);
        return NVS_ERR_TOO_LARGE;
    }
    /* manifest_bytes is the length of this envelope; its decimal width feeds back into the length, so settle it. */
    {
        uint64_t guess = 0;
        int attempt;
        for (attempt = 0; attempt < 4; attempt++) {
            env.len = 0;
            if (!build_envelope(writer, &env, guess, err)) {
                free(env.data);
                writer_free(writer);
                return err ? err->code : NVS_ERR_NOMEM;
            }
            if ((uint64_t)env.len == guess) break;
            guess = (uint64_t)env.len;
        }
        if ((uint64_t)env.len != guess) {
            set_error(err, NVS_ERR_FORMAT, "the envelope length did not settle");
            free(env.data);
            writer_free(writer);
            return NVS_ERR_FORMAT;
        }
    }
    /* The envelope becomes the manifest payload: compress it like any other block. */
    if (!store_block(writer, env.data, env.len, 1, &envelope_compressed, &envelope_stored_size, &envelope_zstd,
                     err)) {
        free(env.data);
        writer_free(writer);
        return err ? err->code : NVS_ERR_NOMEM;
    }
    envelope = envelope_compressed;
    nvs_sha256_digest(envelope, (size_t)envelope_stored_size, envelope_sha256);

    pos = NVS_HEADER_SIZE;
    table_offset = pos;
    if (writer->count > (UINT64_MAX - pos) / NVS_CHUNK_ENTRY_SIZE) {
        set_error(err, NVS_ERR_TOO_LARGE, "chunk directory overflows the file size");
        goto done;
    }
    pos += (uint64_t)writer->count * NVS_CHUNK_ENTRY_SIZE;
    manifest_offset = pos;
    if (envelope_stored_size > UINT64_MAX - pos) {
        set_error(err, NVS_ERR_TOO_LARGE, "manifest overflows the file size");
        goto done;
    }
    pos += envelope_stored_size;
    for (i = 0; i < writer->count; i++) {
        pos = (pos + 7u) & ~(uint64_t)7u;
        writer->entries[i].offset = pos;
        if (writer->entries[i].stored_size > UINT64_MAX - pos) {
            set_error(err, NVS_ERR_TOO_LARGE, "blob \"%s\" overflows the file size", writer->entries[i].id);
            goto done;
        }
        pos += writer->entries[i].stored_size;
    }
    file_size = pos;

    header = (uint8_t *)calloc(1, NVS_HEADER_SIZE);
    table = (uint8_t *)calloc(writer->count ? writer->count : 1, NVS_CHUNK_ENTRY_SIZE);
    if (!header || !table) {
        set_error(err, NVS_ERR_NOMEM, "cannot allocate the package directory");
        goto done;
    }
    memcpy(header, NVS_MAGIC, 8);
    put_u16(header + 8, NVS_FORMAT_MAJOR);
    put_u16(header + 10, NVS_FORMAT_MINOR);
    put_u32(header + 12, NVS_HEADER_SIZE);
    {
        uint32_t flags = 0;
        if (envelope_zstd) flags |= NVS_HEADER_FLAG_MANIFEST_ZSTD;
        for (i = 0; i < writer->count; i++) {
            if (writer->entries[i].flags & NVS_CHUNK_FLAG_ZSTD) flags |= NVS_HEADER_FLAG_CHUNKS_ZSTD;
        }
        put_u32(header + 16, flags);
    }
    put_u32(header + 20, (uint32_t)writer->count);
    put_u32(header + 24, NVS_CHUNK_ENTRY_SIZE);
    put_u64(header + 28, table_offset);
    put_u64(header + 36, manifest_offset);
    put_u64(header + 44, envelope_stored_size);
    put_u64(header + 52, (uint64_t)env.len);
    put_u64(header + 60, file_size);
    memcpy(header + 68, envelope_sha256, 32);
    for (i = 0; i < writer->count; i++) {
        const nvs_entry *entry = &writer->entries[i];
        uint8_t *slot = table + i * NVS_CHUNK_ENTRY_SIZE;
        size_t id_len = strlen(entry->id);
        memcpy(slot, entry->id, id_len);
        put_u32(slot + 64, (uint32_t)entry->kind);
        put_u32(slot + 68, entry->flags);
        put_u64(slot + 72, entry->offset);
        put_u64(slot + 80, entry->stored_size);
        put_u64(slot + 88, entry->raw_size);
        memcpy(slot + 96, entry->sha256, 32);
    }

    if (snprintf(temp, sizeof temp, "%s.tmp.%ld", writer->path, (long)getpid()) >= (int)sizeof temp) {
        temp[0] = '\0';
        set_error(err, NVS_ERR_IO, "package path is too long");
        goto done;
    }
    file = fopen(temp, "wb");
    if (!file) {
        set_error(err, NVS_ERR_IO, "cannot create %s: %s", temp, strerror(errno));
        goto done;
    }
    if (!write_all(file, header, NVS_HEADER_SIZE) || !write_all(file, table,
                                                               (size_t)writer->count * NVS_CHUNK_ENTRY_SIZE) ||
        !write_all(file, env.data, (size_t)envelope_stored_size)) {
        set_error(err, NVS_ERR_IO, "cannot write the package header or directory: %s", strerror(errno));
        goto done;
    }
    {
        uint64_t written = NVS_HEADER_SIZE + (uint64_t)writer->count * NVS_CHUNK_ENTRY_SIZE + envelope_stored_size;
        for (i = 0; i < writer->count; i++) {
            static const uint8_t zeros[8] = {0};
            uint64_t padding = writer->entries[i].offset - written;
            if (padding && !write_all(file, zeros, (size_t)padding)) {
                set_error(err, NVS_ERR_IO, "cannot write padding before \"%s\"", writer->entries[i].id);
                goto done;
            }
            if (!write_all(file, writer->entries[i].stored, (size_t)writer->entries[i].stored_size)) {
                set_error(err, NVS_ERR_IO, "cannot write blob \"%s\"", writer->entries[i].id);
                goto done;
            }
            written = writer->entries[i].offset + writer->entries[i].stored_size;
        }
        if (written != file_size) {
            set_error(err, NVS_ERR_IO, "wrote %llu bytes but planned %llu", (unsigned long long)written,
                      (unsigned long long)file_size);
            goto done;
        }
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        set_error(err, NVS_ERR_IO, "cannot flush %s: %s", temp, strerror(errno));
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        set_error(err, NVS_ERR_IO, "cannot close %s: %s", temp, strerror(errno));
        goto done;
    }
    file = NULL;
    if (rename(temp, writer->path) != 0) {
        set_error(err, NVS_ERR_IO, "cannot rename %s to %s: %s", temp, writer->path, strerror(errno));
        goto done;
    }
    ok = 1;

done:
    if (file) fclose(file);
    if (!ok && temp[0]) unlink(temp);
    free(header);
    free(table);
    free(envelope);
    free(env.data);
    writer_free(writer);
    return ok ? NVS_OK : (err ? err->code : NVS_ERR_IO);
}

/* --------------------------------------------------------------------------------------------------------------- */
/* reader                                                                                                          */
/* --------------------------------------------------------------------------------------------------------------- */

struct nvs_reader {
    char *path;
    uint8_t *data;
    size_t size;
    nvs_package_header header;
    nvs_chunk_info *chunks;
    size_t chunk_count;
    nvs_decompress_fn decompress;
    void *decompress_user;
};

static int read_package_header(nvs_reader *reader, nvs_error *err)
{
    const uint8_t *h = reader->data;
    nvs_package_header *header = &reader->header;
    uint64_t table_end;
    if (reader->size < NVS_HEADER_SIZE) {
        set_error(err, NVS_ERR_FORMAT, "file is %zu bytes, shorter than the %u-byte header", reader->size,
                  NVS_HEADER_SIZE);
        return 0;
    }
    if (memcmp(h, NVS_MAGIC, 8) != 0) {
        set_error(err, NVS_ERR_FORMAT, "bad magic (expected \"%s\")", NVS_MAGIC);
        return 0;
    }
    header->major = get_u16(h + 8);
    header->minor = get_u16(h + 10);
    header->flags = get_u32(h + 16);
    header->chunk_count = get_u32(h + 20);
    header->file_size = get_u64(h + 60);
    header->chunk_table_offset = get_u64(h + 28);
    header->manifest_offset = get_u64(h + 36);
    header->manifest_stored_size = get_u64(h + 44);
    header->manifest_raw_size = get_u64(h + 52);
    memcpy(header->manifest_sha256, h + 68, 32);
    if (header->major != NVS_FORMAT_MAJOR) {
        set_error(err, NVS_ERR_UNSUPPORTED,
                  "format major %u is not supported by this reader (it reads %u), file was probably written by a "
                  "newer scenecap",
                  (unsigned)header->major, (unsigned)NVS_FORMAT_MAJOR);
        return 0;
    }
    if (header->minor > NVS_FORMAT_MINOR) {
        set_error(err, NVS_ERR_UNSUPPORTED,
                  "format 1.%u is newer than this reader (1.%u); upgrade scenecap", (unsigned)header->minor,
                  (unsigned)NVS_FORMAT_MINOR);
        return 0;
    }
    if (get_u32(h + 12) != NVS_HEADER_SIZE) {
        set_error(err, NVS_ERR_UNSUPPORTED, "header size %u is not supported (expected %u)", (unsigned)get_u32(h + 12),
                  NVS_HEADER_SIZE);
        return 0;
    }
    if (header->flags & ~(uint32_t)(NVS_HEADER_FLAG_MANIFEST_ZSTD | NVS_HEADER_FLAG_CHUNKS_ZSTD)) {
        set_error(err, NVS_ERR_FORMAT, "unknown header flag bits 0x%08x", (unsigned)header->flags);
        return 0;
    }
    {
        size_t i;
        for (i = 100; i < NVS_HEADER_SIZE; i++) {
            if (h[i]) {
                set_error(err, NVS_ERR_FORMAT, "reserved header byte %zu is not zero", i);
                return 0;
            }
        }
    }
    {
        uint32_t entry_size = get_u32(h + 24);
        if (entry_size != NVS_CHUNK_ENTRY_SIZE) {
            set_error(err, NVS_ERR_UNSUPPORTED, "chunk entry size %u is not supported (expected %u)",
                      (unsigned)entry_size, NVS_CHUNK_ENTRY_SIZE);
            return 0;
        }
    }
    if (header->file_size != (uint64_t)reader->size) {
        set_error(err, NVS_ERR_FORMAT, "header records %llu bytes but the file is %zu bytes",
                  (unsigned long long)header->file_size, reader->size);
        return 0;
    }
    if (header->chunk_table_offset < NVS_HEADER_SIZE ||
        header->chunk_table_offset > (uint64_t)reader->size) {
        set_error(err, NVS_ERR_FORMAT, "chunk table offset %llu is outside the file",
                  (unsigned long long)header->chunk_table_offset);
        return 0;
    }
    if ((uint64_t)header->chunk_count * NVS_CHUNK_ENTRY_SIZE >
        (uint64_t)reader->size - header->chunk_table_offset) {
        set_error(err, NVS_ERR_FORMAT, "chunk table (%u entries) does not fit in the file",
                  (unsigned)header->chunk_count);
        return 0;
    }
    table_end = header->chunk_table_offset + (uint64_t)header->chunk_count * NVS_CHUNK_ENTRY_SIZE;
    if (header->manifest_offset < table_end || header->manifest_offset > (uint64_t)reader->size ||
        header->manifest_stored_size > (uint64_t)reader->size - header->manifest_offset) {
        set_error(err, NVS_ERR_FORMAT, "manifest region (%llu + %llu) is outside the file",
                  (unsigned long long)header->manifest_offset, (unsigned long long)header->manifest_stored_size);
        return 0;
    }
    if (header->manifest_raw_size > NVS_MANIFEST_LIMIT) {
        set_error(err, NVS_ERR_TOO_LARGE, "manifest declares %llu bytes, the limit is %llu",
                  (unsigned long long)header->manifest_raw_size, (unsigned long long)NVS_MANIFEST_LIMIT);
        return 0;
    }
    return 1;
}

static int read_package_chunks(nvs_reader *reader, nvs_error *err)
{
    const uint8_t *table = reader->data + reader->header.chunk_table_offset;
    size_t i, j;
    reader->chunk_count = reader->header.chunk_count;
    if (!reader->chunk_count) return 1;
    reader->chunks = (nvs_chunk_info *)calloc(reader->chunk_count, sizeof *reader->chunks);
    if (!reader->chunks) {
        set_error(err, NVS_ERR_NOMEM, "cannot allocate the chunk directory");
        return 0;
    }
    for (i = 0; i < reader->chunk_count; i++) {
        const uint8_t *slot = table + i * NVS_CHUNK_ENTRY_SIZE;
        nvs_chunk_info *chunk = &reader->chunks[i];
        size_t id_len = strnlen((const char *)slot, NVS_CHUNK_ID_MAX);
        if (id_len == NVS_CHUNK_ID_MAX || !id_valid((const char *)slot)) {
            set_error(err, NVS_ERR_FORMAT, "chunk %zu has an invalid id in the directory", i);
            return 0;
        }
        memcpy(chunk->id, slot, id_len);
        chunk->id[id_len] = '\0';
        chunk->kind = (nvs_kind)get_u32(slot + 64);
        chunk->flags = get_u32(slot + 68);
        chunk->offset = get_u64(slot + 72);
        chunk->stored_size = get_u64(slot + 80);
        chunk->raw_size = get_u64(slot + 88);
        memcpy(chunk->sha256, slot + 96, 32);
        if (!nvs_kind_name(chunk->kind)[0]) {
            set_error(err, NVS_ERR_FORMAT, "chunk \"%s\" has unknown kind %u", chunk->id, (unsigned)chunk->kind);
            return 0;
        }
        if (chunk->offset < NVS_HEADER_SIZE || chunk->offset > (uint64_t)reader->size ||
            chunk->stored_size > (uint64_t)reader->size - chunk->offset) {
            set_error(err, NVS_ERR_FORMAT, "chunk \"%s\" payload (%llu + %llu) is outside the file", chunk->id,
                      (unsigned long long)chunk->offset, (unsigned long long)chunk->stored_size);
            return 0;
        }
        if (chunk->raw_size > NVS_RAW_LIMIT) {
            set_error(err, NVS_ERR_TOO_LARGE, "chunk \"%s\" declares %llu raw bytes", chunk->id,
                      (unsigned long long)chunk->raw_size);
            return 0;
        }
        if (!(chunk->flags & NVS_CHUNK_FLAG_ZSTD) && chunk->stored_size != chunk->raw_size) {
            set_error(err, NVS_ERR_FORMAT, "chunk \"%s\" is stored verbatim but its sizes differ (%llu vs %llu)",
                      chunk->id, (unsigned long long)chunk->stored_size, (unsigned long long)chunk->raw_size);
            return 0;
        }
        for (j = 0; j < i; j++) {
            if (!strcmp(reader->chunks[j].id, chunk->id)) {
                set_error(err, NVS_ERR_FORMAT, "chunk id \"%s\" appears twice in the directory", chunk->id);
                return 0;
            }
        }
    }
    return 1;
}

int nvs_read_open(const char *path, nvs_reader **out, nvs_error *err)
{
    FILE *file;
    long length;
    nvs_reader *reader;
    clear_error(err);
    if (!path || !out) {
        set_error(err, NVS_ERR_ARG, "nvs_read_open: null path or output");
        return NVS_ERR_ARG;
    }
    *out = NULL;
    file = fopen(path, "rb");
    if (!file) {
        set_error(err, NVS_ERR_IO, "cannot open %s: %s", path, strerror(errno));
        return NVS_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        set_error(err, NVS_ERR_IO, "cannot size %s: %s", path, strerror(errno));
        fclose(file);
        return NVS_ERR_IO;
    }
    if ((unsigned long)length > (unsigned long)SIZE_MAX) {
        set_error(err, NVS_ERR_TOO_LARGE, "%s is too large for this reader", path);
        fclose(file);
        return NVS_ERR_TOO_LARGE;
    }
    reader = (nvs_reader *)calloc(1, sizeof *reader);
    if (!reader) {
        set_error(err, NVS_ERR_NOMEM, "nvs_read_open: out of memory");
        fclose(file);
        return NVS_ERR_NOMEM;
    }
    reader->path = strdup(path);
    reader->size = (size_t)length;
    reader->data = (uint8_t *)malloc(reader->size ? reader->size : 1);
    if (!reader->path || !reader->data) {
        set_error(err, NVS_ERR_NOMEM, "nvs_read_open: out of memory");
        fclose(file);
        nvs_read_close(reader);
        return NVS_ERR_NOMEM;
    }
    if (reader->size && fread(reader->data, 1, reader->size, file) != reader->size) {
        set_error(err, NVS_ERR_IO, "cannot read %s", path);
        fclose(file);
        nvs_read_close(reader);
        return NVS_ERR_IO;
    }
    fclose(file);
    if (!read_package_header(reader, err) || !read_package_chunks(reader, err)) {
        int code = err ? err->code : NVS_ERR_FORMAT;
        nvs_read_close(reader);
        return code;
    }
    *out = reader;
    return NVS_OK;
}

void nvs_read_close(nvs_reader *reader)
{
    if (!reader) return;
    free(reader->path);
    free(reader->data);
    free(reader->chunks);
    free(reader);
}

const nvs_package_header *nvs_read_header(const nvs_reader *reader)
{
    return reader ? &reader->header : NULL;
}

size_t nvs_read_chunk_count(const nvs_reader *reader)
{
    return reader ? reader->chunk_count : 0;
}

int nvs_read_chunk_at(const nvs_reader *reader, size_t index, nvs_chunk_info *out, nvs_error *err)
{
    clear_error(err);
    if (!reader || !out || index >= reader->chunk_count) {
        set_error(err, NVS_ERR_ARG, "chunk index %zu is out of range", index);
        return NVS_ERR_ARG;
    }
    *out = reader->chunks[index];
    return NVS_OK;
}

int nvs_read_find_chunk(const nvs_reader *reader, const char *id, nvs_chunk_info *out, nvs_error *err)
{
    size_t i;
    clear_error(err);
    if (!reader || !id) {
        set_error(err, NVS_ERR_ARG, "nvs_read_find_chunk: null reader or id");
        return NVS_ERR_ARG;
    }
    for (i = 0; i < reader->chunk_count; i++) {
        if (!strcmp(reader->chunks[i].id, id)) {
            if (out) *out = reader->chunks[i];
            return NVS_OK;
        }
    }
    set_error(err, NVS_ERR_FORMAT, "no chunk named \"%s\"", id);
    return NVS_ERR_FORMAT;
}

void nvs_read_set_decompressor(nvs_reader *reader, nvs_decompress_fn decompress, void *user)
{
    if (!reader) return;
    reader->decompress = decompress;
    reader->decompress_user = decompress ? user : NULL;
}

static int decode_block(nvs_reader *reader, const uint8_t *stored, uint64_t stored_size, uint64_t raw_size,
                        uint32_t flags, uint8_t **out, size_t *out_len, const char *what, nvs_error *err)
{
    uint8_t *raw;
    size_t produced;
    if (raw_size > SIZE_MAX) {
        set_error(err, NVS_ERR_TOO_LARGE, "%s: %llu bytes does not fit this platform", what,
                  (unsigned long long)raw_size);
        return NVS_ERR_TOO_LARGE;
    }
    if (raw_size == SIZE_MAX) {
        set_error(err, NVS_ERR_TOO_LARGE, "%s: %llu bytes does not fit this platform", what,
                  (unsigned long long)raw_size);
        return NVS_ERR_TOO_LARGE;
    }
    raw = (uint8_t *)malloc((size_t)raw_size + 1u);
    if (!raw) {
        set_error(err, NVS_ERR_NOMEM, "%s: cannot allocate %llu bytes", what, (unsigned long long)raw_size);
        return NVS_ERR_NOMEM;
    }
    if (flags & NVS_CHUNK_FLAG_ZSTD) {
        if (reader->decompress) {
            produced = reader->decompress(reader->decompress_user, stored, (size_t)stored_size, raw,
                                          (size_t)raw_size);
            if (produced != (size_t)raw_size) {
                free(raw);
                set_error(err, NVS_ERR_FORMAT, "%s: the installed zstd decoder returned %zu of %llu bytes", what,
                          produced, (unsigned long long)raw_size);
                return NVS_ERR_FORMAT;
            }
        } else {
            nvs_error zerr;
            produced = nvs_zstd_decompress_ex(stored, (size_t)stored_size, raw, (size_t)raw_size, &zerr);
            if (produced != (size_t)raw_size) {
                free(raw);
                if (zerr.code == NVS_ERR_UNSUPPORTED) {
                    set_error(err, NVS_ERR_UNSUPPORTED, "%s: %s; install a zstd decoder with "
                                                        "nvs_read_set_decompressor()",
                              what, zerr.message);
                    return NVS_ERR_UNSUPPORTED;
                }
                set_error(err, NVS_ERR_FORMAT, "%s: %s", what, zerr.message);
                return NVS_ERR_FORMAT;
            }
        }
    } else {
        if (stored_size != raw_size) {
            free(raw);
            set_error(err, NVS_ERR_FORMAT, "%s: stored verbatim with mismatched sizes", what);
            return NVS_ERR_FORMAT;
        }
        if (raw_size) memcpy(raw, stored, (size_t)raw_size);
    }
    raw[raw_size] = 0;
    *out = raw;
    *out_len = (size_t)raw_size;
    return NVS_OK;
}

int nvs_read_manifest(nvs_reader *reader, char **json, size_t *len, nvs_error *err)
{
    const uint8_t *stored;
    uint8_t digest[32];
    uint8_t *raw_ptr = NULL;
    char *text = NULL;
    size_t text_len = 0;
    int code;
    clear_error(err);
    if (!reader || !json || !len) {
        set_error(err, NVS_ERR_ARG, "nvs_read_manifest: null argument");
        return NVS_ERR_ARG;
    }
    *json = NULL;
    *len = 0;
    stored = reader->data + reader->header.manifest_offset;
    nvs_sha256_digest(stored, (size_t)reader->header.manifest_stored_size, digest);
    if (memcmp(digest, reader->header.manifest_sha256, 32) != 0) {
        char expected[65], actual[65];
        nvs_hex(reader->header.manifest_sha256, 32, expected);
        nvs_hex(digest, 32, actual);
        set_error(err, NVS_ERR_FORMAT, "manifest hash mismatch: header records %s, payload hashes to %s", expected,
                  actual);
        return NVS_ERR_FORMAT;
    }
    code = decode_block(reader, stored, reader->header.manifest_stored_size, reader->header.manifest_raw_size,
                        (reader->header.flags & NVS_HEADER_FLAG_MANIFEST_ZSTD) ? NVS_CHUNK_FLAG_ZSTD : 0u,
                        &raw_ptr, &text_len, "manifest", err);
    if (code != NVS_OK) return code;
    text = (char *)raw_ptr;
    *json = text;
    *len = text_len;
    return NVS_OK;
}

int nvs_read_blob(nvs_reader *reader, const char *id, void **data, size_t *len, nvs_error *err)
{
    nvs_chunk_info chunk;
    const uint8_t *stored;
    uint8_t digest[32];
    int code;
    clear_error(err);
    if (!reader || !id || !data || !len) {
        set_error(err, NVS_ERR_ARG, "nvs_read_blob: null argument");
        return NVS_ERR_ARG;
    }
    *data = NULL;
    *len = 0;
    code = nvs_read_find_chunk(reader, id, &chunk, err);
    if (code != NVS_OK) return code;
    stored = reader->data + chunk.offset;
    nvs_sha256_digest(stored, (size_t)chunk.stored_size, digest);
    if (memcmp(digest, chunk.sha256, 32) != 0) {
        char expected[65], actual[65];
        nvs_hex(chunk.sha256, 32, expected);
        nvs_hex(digest, 32, actual);
        set_error(err, NVS_ERR_FORMAT, "blob \"%s\" hash mismatch: directory records %s, payload hashes to %s", id,
                  expected, actual);
        return NVS_ERR_FORMAT;
    }
    {
        uint8_t *raw_ptr = NULL;
        code = decode_block(reader, stored, chunk.stored_size, chunk.raw_size, chunk.flags, &raw_ptr, len, id, err);
        if (code == NVS_OK) *data = raw_ptr;
        return code;
    }
}

int nvs_read_verify_hashes(nvs_reader *reader, nvs_error *err)
{
    size_t i;
    clear_error(err);
    if (!reader) {
        set_error(err, NVS_ERR_ARG, "nvs_read_verify_hashes: null reader");
        return NVS_ERR_ARG;
    }
    {
        uint8_t digest[32];
        nvs_sha256_digest(reader->data + reader->header.manifest_offset, (size_t)reader->header.manifest_stored_size,
                   digest);
        if (memcmp(digest, reader->header.manifest_sha256, 32) != 0) {
            set_error(err, NVS_ERR_FORMAT, "manifest hash mismatch");
            return NVS_ERR_FORMAT;
        }
    }
    for (i = 0; i < reader->chunk_count; i++) {
        const nvs_chunk_info *chunk = &reader->chunks[i];
        uint8_t digest[32];
        nvs_sha256_digest(reader->data + chunk->offset, (size_t)chunk->stored_size, digest);
        if (memcmp(digest, chunk->sha256, 32) != 0) {
            set_error(err, NVS_ERR_FORMAT, "blob \"%s\" at offset %llu fails its SHA-256 check", chunk->id,
                      (unsigned long long)chunk->offset);
            return NVS_ERR_FORMAT;
        }
    }
    return NVS_OK;
}
