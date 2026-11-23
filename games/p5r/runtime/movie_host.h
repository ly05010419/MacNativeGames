/* Host video decoder behind the SDK movie library's movie::Decoder (runtime/movie_host.c). */
#ifndef TOTK_MOVIE_HOST_H
#define TOTK_MOVIE_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The guest functions the host decoder calls (nn::os::SignalEvent, movie::Buffer and movie::MediaData methods) and the
   original movie::Decoder code it hands other decoders to. runtime_movie_override fills them from the loaded modules;
   the probe (scripts/movie_probe.c) supplies stand-ins with the same behaviour. */
typedef struct {
    void (*signal_event)(void *event);
    void (*buf_set_range)(void *buf, int offset, int size);
    void (*buf_set_data)(void *buf, void *data, size_t capacity);
    void *(*buf_base)(const void *buf);
    size_t (*buf_capacity)(const void *buf);
    bool (*md_find_int32)(const void *md, const char *key, int *value);
    bool (*md_find_string)(const void *md, const char *key, const char **value);
    void (*md_set_int32)(void *md, const char *key, int value);
    void (*md_set_rect)(void *md, const char *key, int left, int top, int right, int bottom);
    void (*md_set_string)(void *md, const char *key, const char *value);
    /* Odyssey H.264 (optional: NULL when the library lacks them): codec-specific data csd-0 / csd-1 from the format */
    bool (*md_find_buffer)(const void *md, const char *key, void **buf);
    void *(*buf_data)(const void *buf);
    size_t (*buf_size)(const void *buf);
    void *vptr;                           /* vtable for movie::Decoder + 0x10: what the constructor stores at +0 */
    void *orig[20];                       /* original code per entry (MOVIE_* below), for decoders left to the library */
} MovieGuest;

enum { MOVIE_CTOR, MOVIE_D2, MOVIE_D0, MOVIE_CONFIGURE, MOVIE_REGISTER_EVENTS, MOVIE_GET_OUTPUT_FORMAT, MOVIE_START,
       MOVIE_STOP, MOVIE_RESET, MOVIE_FLUSH, MOVIE_ACQUIRE_INPUT, MOVIE_GET_INPUT, MOVIE_SEND_INPUT, MOVIE_ACQUIRE_OUTPUT,
       MOVIE_GET_OUTPUT, MOVIE_GET_OUTPUT_SIZE, MOVIE_RELEASE_OUTPUT, MOVIE_GET_LAST_ERROR, MOVIE_GET_OUTPUT_MD,
       MOVIE_GET_OUTPUT_BUFFER_FORMAT, MOVIE_ENTRIES };

void movie_host_set_guest(const MovieGuest *api);    /* probe only: use these stand-ins instead of the modules */
void *movie_host_entry(int which);                   /* the host implementation of entry `which` (MOVIE_*) */
void *runtime_movie_override(const char *name);
void movie_host_tick(void);                          /* every present: hand over decoded frames that are due */
void movie_host_set_clock(uint64_t (*clock)(void));  /* probe only: a fake clock (ns) for the pacing */
#endif
