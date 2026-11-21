// The first 64 bytes are shared with offline main0_tf (dmsh_build.py).
// Control buffers are opaque to the game: four byte offsets followed by Metal
// array-draw arguments. All updates and saved/restored counts follow GPU order.
typedef struct {
    uint32_t offset[4], args[4]; uint64_t order;
    uint32_t first, count, base_instance, pad; uint64_t reserved;
    uint64_t state, source_count; uint32_t stride[4], capacity[4];
    uint32_t requested, primitive; uint64_t reserved2;
} nvn_tf_job;
_Static_assert(offsetof(nvn_tf_job, state) == 64 && sizeof(nvn_tf_job) == 128, "feedback capture ABI");
// The kernel tf_prepare: dm/gfx/msl/tf.metal (host_msl_lib "tf")
