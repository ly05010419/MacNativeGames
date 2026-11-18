/* Completed buffer storage, bounded independently of the live working set.
 * Fence-based table reuse and rotating readback sizes follow nxnative 6121fd75
 * (MIT, LICENSE.nxnative). The caller supplies the last GPU fence and retires
 * evictions through its normal resource-lifetime path. */
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    void *object; // CF-owned; no autoreleased observer may become an owner
    size_t bytes, allocated;
    uint64_t fence, age;
} nvn_buffer_spare;
typedef struct {
    pthread_mutex_t mutex;
    nvn_buffer_spare entries[64];
    unsigned count, limit;
    size_t bytes, byte_limit;
    uint64_t clock, made, reused, evicted, rejected;
} nvn_buffer_cache;
typedef void (*nvn_buffer_retire)(id<MTLBuffer>, uint64_t);

static id<MTLBuffer> nvn_buffer_cache_take(nvn_buffer_cache *cache, size_t requested,
                                        size_t alignment, uint64_t done, int enabled) {
    if (!requested || !alignment || (alignment & (alignment - 1)) || requested > SIZE_MAX - (alignment - 1)) return nil;
    size_t bytes = enabled ? (requested + alignment - 1) & ~(alignment - 1) : requested;
    void *object = NULL;
    pthread_mutex_lock(&cache->mutex);
    unsigned best = cache->count;
    if (enabled) for (unsigned i = 0; i < cache->count; i++) {
        const nvn_buffer_spare *entry = &cache->entries[i];
        if (entry->fence > done || entry->bytes != bytes) continue;
        if (best == cache->count || entry->bytes < cache->entries[best].bytes) best = i;
    }
    if (best < cache->count) {
        object = cache->entries[best].object;
        cache->bytes -= cache->entries[best].allocated;
        cache->entries[best] = cache->entries[--cache->count];
        cache->reused++;
    }
    pthread_mutex_unlock(&cache->mutex);
    if (object) return CFBridgingRelease(object);
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer) { pthread_mutex_lock(&cache->mutex); cache->made++; pthread_mutex_unlock(&cache->mutex); }
    return buffer;
}

static void nvn_buffer_cache_put(nvn_buffer_cache *cache, id<MTLBuffer> buffer,
                                uint64_t fence, nvn_buffer_retire retire, int enabled) {
    if (!buffer) return;
    size_t bytes = buffer.length;
    size_t allocated = buffer.allocatedSize;
    if (allocated < bytes) allocated = bytes;
    nvn_buffer_spare victims[64]; unsigned count = 0;
    pthread_mutex_lock(&cache->mutex);
    if (!enabled || !cache->limit || allocated > cache->byte_limit) {
        cache->rejected++;
        pthread_mutex_unlock(&cache->mutex);
        if (retire) retire(buffer, fence);
        return;
    }
    while (cache->count && (cache->count >= cache->limit || cache->count >= 64 || allocated > cache->byte_limit - cache->bytes)) {
        unsigned oldest = 0;
        for (unsigned i = 1; i < cache->count; i++) if (cache->entries[i].age < cache->entries[oldest].age) oldest = i;
        victims[count++] = cache->entries[oldest];
        cache->bytes -= cache->entries[oldest].allocated;
        cache->entries[oldest] = cache->entries[--cache->count];
        cache->evicted++;
    }
    cache->entries[cache->count++] = (nvn_buffer_spare){(void *)CFBridgingRetain(buffer), bytes, allocated, fence, ++cache->clock};
    cache->bytes += allocated;
    pthread_mutex_unlock(&cache->mutex);
    // Evicting a pending table is retirement, never permission to reuse it.
    for (unsigned i = 0; i < count; i++) {
        if (retire) retire((__bridge id<MTLBuffer>)victims[i].object, victims[i].fence);
        CFRelease(victims[i].object);
    }
}

static void nvn_buffer_cache_clear(nvn_buffer_cache *cache, nvn_buffer_retire retire) {
    nvn_buffer_spare victims[64]; unsigned count;
    pthread_mutex_lock(&cache->mutex);
    count = cache->count;
    for (unsigned i = 0; i < count; i++) victims[i] = cache->entries[i];
    cache->count = 0; cache->bytes = 0;
    pthread_mutex_unlock(&cache->mutex);
    for (unsigned i = 0; i < count; i++) {
        if (retire) retire((__bridge id<MTLBuffer>)victims[i].object, victims[i].fence);
        CFRelease(victims[i].object);
    }
}
