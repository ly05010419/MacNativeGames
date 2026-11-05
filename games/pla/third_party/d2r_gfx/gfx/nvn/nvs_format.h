/*
 * scenecap .nvs — portable, versioned, self-describing render-scene package.
 *
 * A package is a single file:
 *
 *   +----------------------------+ 0
 *   | header (128 bytes)         |   magic "SCENECAP", version, directory geometry, manifest digest
 *   +----------------------------+ 128
 *   | chunk table (128 B / chunk)|   id, kind, flags, payload offset, stored/raw sizes, SHA-256
 *   +----------------------------+
 *   | manifest payload           |   zstd frame (or verbatim) holding the JSON envelope described below
 *   +----------------------------+   every blob and the manifest start on an 8-byte boundary
 *   | blob payloads              |   one per chunk, in table order, each zstd frame or verbatim
 *   +----------------------------+ file_size
 *
 * All integers are little-endian. The header covers bytes 0..127:
 *
 *   0   char[8]  magic "SCENECAP"
 *   8   u16      major (1)
 *   10  u16      minor (0)
 *   12  u32      header_size (128; readers accept larger values from compatible minors)
 *   16  u32      flags (bit0: manifest is a zstd frame, bit1: at least one chunk is a zstd frame)
 *   20  u32      chunk_count
 *   24  u32      chunk_entry_size (128)
 *   28  u64      chunk_table_offset
 *   36  u64      manifest_offset
 *   44  u64      manifest_stored_size   bytes on disk
 *   52  u64      manifest_raw_size      bytes after decompression (the JSON envelope length)
 *   60  u64      file_size
 *   68  u8[32]   SHA-256 of the manifest payload as stored
 *   100 u8[28]   reserved, must be zero
 *
 * Each chunk table entry covers 128 bytes:
 *
 *   0   char[64] id, NUL-terminated, 1..63 bytes of [A-Za-z0-9._:/-]
 *   64  u32      kind (nvs_kind)
 *   68  u32      flags (bit0: payload is a zstd frame)
 *   72  u64      payload offset
 *   80  u64      stored_size (bytes on disk)
 *   88  u64      raw_size (bytes after decompression; equals stored_size when uncompressed)
 *   96  u8[32]   SHA-256 of the payload as stored
 *
 * The manifest payload is one JSON object (the "envelope"):
 *
 *   {"nvs": {"format": "scenecap", "version": "1.0", "generator": "...",
 *            "chunk_count": N, "manifest_bytes": M},
 *    "chunks": [{"id": "...", "kind": "texture", "flags": 0, "compression": "zstd",
 *                "stored_bytes": 123, "raw_bytes": 456, "sha256": "<hex>"}, ...],
 *    "manifest": { ... the scene manifest written by the capture layer ... }}
 *
 * The envelope is the single source of truth for the chunk directory: readers cross-check it against the table
 * (same ids, kinds, sizes and digests, in the same order) and then resolve manifest references against it.
 *
 * Version policy: a reader accepts any package whose major equals its own and whose minor is <= its own. A new
 * minor may only add optional fields, kinds and manifest keys; a reader that meets an unknown minor refuses the
 * file so that no half-understood scene is ever replayed. Major bumps may change the layout or field meanings.
 */
#ifndef SCENECAP_NVS_FORMAT_H
#define SCENECAP_NVS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_MAGIC "SCENECAP"
#define NVS_FORMAT_MAJOR 1u
#define NVS_FORMAT_MINOR 0u
#define NVS_FORMAT_VERSION "1.0"

#define NVS_HEADER_SIZE 128u
#define NVS_CHUNK_ENTRY_SIZE 128u
#define NVS_CHUNK_ID_MAX 64u

#define NVS_HEADER_FLAG_MANIFEST_ZSTD 0x1u
#define NVS_HEADER_FLAG_CHUNKS_ZSTD 0x2u
#define NVS_CHUNK_FLAG_ZSTD 0x1u

/* Pass NVS_BLOB_STORE_RAW to keep a blob uncompressed even when a compressor is installed. */
#define NVS_BLOB_STORE_RAW 0x1u

typedef enum {
    NVS_OK = 0,
    NVS_ERR_ARG = 1,        /* caller misuse */
    NVS_ERR_IO = 2,         /* open/read/write/rename failure */
    NVS_ERR_FORMAT = 3,     /* corrupt or inconsistent package, or a missing referenced chunk */
    NVS_ERR_UNSUPPORTED = 4,/* newer major/minor, unknown compression, or a feature this build lacks */
    NVS_ERR_NOMEM = 5,
    NVS_ERR_TOO_LARGE = 6
} nvs_status;

typedef struct {
    int code;              /* nvs_status */
    char message[512];     /* human-readable, always NUL-terminated when code != 0 */
} nvs_error;

typedef enum {
    NVS_KIND_NONE = 0,
    NVS_KIND_JSON = 1,       /* auxiliary JSON (reflection, argument-table dumps, ...) */
    NVS_KIND_MSL = 2,        /* Metal Shading Language source */
    NVS_KIND_REFLECTION = 3, /* pipeline reflection JSON */
    NVS_KIND_TEXTURE = 4,    /* texture bytes, as stored by the capture (row order documented in the manifest) */
    NVS_KIND_BUFFER = 5,     /* buffer bytes */
    NVS_KIND_BINARY = 6      /* anything else */
} nvs_kind;

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buffer[64];
    size_t buffered;
} nvs_sha256;

typedef struct {
    const char *id;      /* required, see the charset above */
    nvs_kind kind;       /* must not be NVS_KIND_NONE */
    const void *data;
    size_t size;
    uint32_t flags;      /* NVS_BLOB_STORE_RAW to force verbatim storage */
} nvs_blob_input;

typedef struct {
    char id[NVS_CHUNK_ID_MAX];
    nvs_kind kind;
    uint32_t flags;
    uint64_t offset;
    uint64_t stored_size;
    uint64_t raw_size;
    uint8_t sha256[32];
} nvs_chunk_info;

typedef struct {
    uint16_t major, minor;
    uint32_t flags;
    uint32_t chunk_count;
    uint64_t chunk_table_offset;
    uint64_t manifest_offset;
    uint64_t manifest_stored_size;
    uint64_t manifest_raw_size;
    uint64_t file_size;
    uint8_t manifest_sha256[32];
} nvs_package_header;

/* zstd_compress: return the number of bytes written to dst, or 0 on failure. dst_cap >= nvs_zstd_bound(src_len). */
typedef size_t (*nvs_compress_fn)(void *user, const void *src, size_t src_len, void *dst, size_t dst_cap);
/* zstd_decompress: return the number of bytes written to dst, or 0 on failure (must equal dst_cap on success). */
typedef size_t (*nvs_decompress_fn)(void *user, const void *src, size_t src_len, void *dst, size_t dst_cap);

typedef struct {
    const char *generator;  /* "nier-native/0.1" and friends; defaults to "scenecap-format/1.0" */
    nvs_compress_fn compress;
    void *compress_user;
} nvs_write_options;

typedef struct nvs_writer nvs_writer;
typedef struct nvs_reader nvs_reader;

/* ---- SHA-256 ------------------------------------------------------------------------------------------------ */
void nvs_sha256_init(nvs_sha256 *ctx);
void nvs_sha256_update(nvs_sha256 *ctx, const void *data, size_t len);
void nvs_sha256_final(nvs_sha256 *ctx, uint8_t out[32]);
void nvs_sha256_digest(const void *data, size_t len, uint8_t out[32]);
/* out must hold 2*len + 1 bytes. */
void nvs_hex(const uint8_t *bytes, size_t len, char *out);

/* ---- built-in zstd frames (raw/RLE blocks) ------------------------------------------------------------------ */
size_t nvs_zstd_bound(size_t size);
size_t nvs_zstd_compress(void *user, const void *src, size_t src_len, void *dst, size_t dst_cap);
/* Decodes frames produced by nvs_zstd_compress. Compressed blocks need nvs_read_set_decompressor(). */
size_t nvs_zstd_decompress_ex(const void *src, size_t src_len, void *dst, size_t dst_cap, nvs_error *err);

/* ---- writer ------------------------------------------------------------------------------------------------- */
nvs_writer *nvs_writer_create(const char *path, const nvs_write_options *options, nvs_error *err);
void nvs_writer_set_compressor(nvs_writer *writer, nvs_compress_fn compress, void *user);
int nvs_writer_add_blob(nvs_writer *writer, const nvs_blob_input *blob, nvs_error *err);
int nvs_writer_add_text(nvs_writer *writer, const char *id, nvs_kind kind, const char *text, nvs_error *err);
/* The manifest is copied; len must be the exact JSON length (no trailing NUL). */
int nvs_writer_set_manifest(nvs_writer *writer, const char *json, size_t len, nvs_error *err);
/* Writes the file atomically (temp file + rename) and frees the writer, including on failure. */
int nvs_writer_finish(nvs_writer *writer, nvs_error *err);

/* ---- reader ------------------------------------------------------------------------------------------------- */
/* Reads and structurally validates the package. Hash verification is nvs_read_verify_hashes(). */
int nvs_read_open(const char *path, nvs_reader **out, nvs_error *err);
void nvs_read_close(nvs_reader *reader);
void nvs_read_set_decompressor(nvs_reader *reader, nvs_decompress_fn decompress, void *user);
const nvs_package_header *nvs_read_header(const nvs_reader *reader);
size_t nvs_read_chunk_count(const nvs_reader *reader);
int nvs_read_chunk_at(const nvs_reader *reader, size_t index, nvs_chunk_info *out, nvs_error *err);
int nvs_read_find_chunk(const nvs_reader *reader, const char *id, nvs_chunk_info *out, nvs_error *err);
/* The returned buffer is malloc'ed (and NUL-terminated) and belongs to the caller. */
int nvs_read_manifest(nvs_reader *reader, char **json, size_t *len, nvs_error *err);
int nvs_read_blob(nvs_reader *reader, const char *id, void **data, size_t *len, nvs_error *err);
int nvs_read_verify_hashes(nvs_reader *reader, nvs_error *err);

/* ---- introspection ------------------------------------------------------------------------------------------- */
const char *nvs_kind_name(nvs_kind kind);
int nvs_kind_from_name(const char *name, nvs_kind *out);

#ifdef __cplusplus
}
#endif

#endif /* SCENECAP_NVS_FORMAT_H */
