#include "runtime.h"
#include "mach_vm_compat.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Optional MCD1 resource-copy probe.  The resource helper at main+0x3136090
 * calls the descriptor size helper at +0x18f0b80, which returns
 * descriptor[0x10] * descriptor->vtable[0x10]().  The probe replaces that BL
 * with a BRK and performs the same helper call from the signal handler.  This
 * records the producer of the copy length while preserving the guest's other
 * registers and resumes at the original post-call instruction.
 *
 * The optional final-copy probe targets the small-copy dispatcher's direct BL
 * at main+0x3136170, which is the path that reaches main+0x47ef420 for the
 * observed 0x3fe0 transfer.
 */
static uint8_t *main_base;
static uint8_t *sdk_base;
static uint32_t original_bl;
static uint32_t original_type5_bl;
static uint32_t original_type5_vcall;
static uint32_t original_type5_size_bl;
static uint32_t original_copy_bl;
static uint32_t original_copy_large_bl;
static uint32_t original_alloc_format_madd;
static uint32_t original_alloc_size_bl;
static uint32_t original_alloc_final_mov;
static uint32_t original_alloc_record_stp;
static int installed;
static int alloc_inputs_installed;
static int type5_helper_installed;
static int type5_vcall_installed;
static int copy_installed;
static int loop_installed;
static __thread struct {
    uint64_t wrapper, resource, index, target, out;
} loop_call;
static uint64_t last_type5_object;
static uint64_t last_type5_type;
static uint64_t last_type5_index;
static uint64_t last_type5_destination;
static uint64_t last_type5_source;
static uint64_t last_type5_entry;
static uint64_t last_type5_entry_source;
static uint64_t last_type5_descriptor;
static uint64_t last_type5_producer;
static _Atomic unsigned records;
static __thread uint32_t alloc_input_size;
static __thread uint32_t alloc_input_alignment;
static __thread uint32_t alloc_input_type;
static __thread uint32_t alloc_input_index;
static __thread uint64_t alloc_format_table;
static __thread uint64_t alloc_format_entry;

static uint64_t word(uint64_t address) {
    uint64_t value = 0;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), address, sizeof value,
                               (mach_vm_address_t)&value, &got) != KERN_SUCCESS || got != sizeof value)
        return 0;
    return value;
}

static uint32_t dword(uint64_t address) {
    return (uint32_t)word(address);
}

static uint64_t type5_descriptor(uint64_t object, uint64_t index) {
    uint64_t table = word(word(object + 0x310) + 0xb8);
    uint64_t descriptors = word(table + 0x10);
    return word(descriptors + index * 8) + 0x10;
}

static void trace_bytes(const char *label, uint64_t address) {
    if (!address) return;
    fprintf(stderr, "[resource-type5-bytes] %s=%#llx words=%#llx,%#llx,%#llx,%#llx\n",
            label, (unsigned long long)address,
            (unsigned long long)word(address + 0x00),
            (unsigned long long)word(address + 0x08),
            (unsigned long long)word(address + 0x10),
            (unsigned long long)word(address + 0x18));
}

static void trace_type5(uint64_t object, uint64_t destination, uint64_t bytes,
                        uint64_t type, uint64_t index, uint64_t entry,
                        uint64_t source) {
    if (!getenv("TOTK_RESOURCE_TRACE") || !getenv("TOTK_RESOURCE_TYPE5")) return;
    uint64_t descriptor = type5_descriptor(object, index);
    uint64_t vtable = word(descriptor);
    uint64_t count = word(descriptor + 0x10);
    uint64_t size_fn = word(vtable + 0x10);
    uint32_t element = size_fn ? ((uint32_t (*)(uint64_t))(uintptr_t)size_fn)(descriptor) : 0;
    uint64_t computed = count * (uint64_t)(int32_t)element;
    uint64_t remaining = runtime_address_region_remaining(destination);
    fprintf(stderr,
            "[resource-type5] object=%#llx type=%#llx index=%#llx "
            "entry=%#llx source=%#llx destination=%#llx bytes_arg=%#llx "
            "descriptor=%#llx count=%#llx element=%#x computed=%#llx "
            "remaining=%#llx object+0x310=%#llx\n",
            (unsigned long long)object, (unsigned long long)type,
            (unsigned long long)index, (unsigned long long)entry,
            (unsigned long long)source, (unsigned long long)destination,
            (unsigned long long)bytes,
            (unsigned long long)descriptor, (unsigned long long)count,
            element, (unsigned long long)computed, (unsigned long long)remaining,
            (unsigned long long)word(object + 0x310));
}

void runtime_resource_trace_install(Module *m) {
    const char *e = getenv("TOTK_RESOURCE_TRACE");
    if (!e || !*e || strcmp(e, "0") == 0) return;
    const uint64_t off = 0x31360d4;
    const uint64_t type5_off = 0x313a7b8;
    const uint64_t type5_vcall_off = 0x313a7a0;
    const uint64_t type5_size_off = 0x313aa10;
    const uint64_t copy_off = 0x3136170;
    const uint64_t copy_large_off = 0x313ab30;
    const uint64_t alloc_format_off = 0x35f04e8;
    const uint64_t alloc_size_off = 0x35f0528;
    const uint64_t alloc_final_off = 0x16edbdc;
    const uint64_t alloc_record_off = 0x35f0580;
    uint32_t *site = (uint32_t *)(m->base + off);
    uint32_t *type5_site = (uint32_t *)(m->base + type5_off);
    uint32_t *type5_vcall_site = (uint32_t *)(m->base + type5_vcall_off);
    uint32_t *type5_size_site = (uint32_t *)(m->base + type5_size_off);
    uint32_t *copy_site = (uint32_t *)(m->base + copy_off);
    uint32_t *copy_large_site = (uint32_t *)(m->base + copy_large_off);
    uint32_t *alloc_format_site = (uint32_t *)(m->base + alloc_format_off);
    uint32_t *alloc_size_site = (uint32_t *)(m->base + alloc_size_off);
    uint32_t *alloc_final_site = (uint32_t *)(m->base + alloc_final_off);
    uint32_t *alloc_record_site = (uint32_t *)(m->base + alloc_record_off);
    original_bl = *site;
    original_type5_bl = *type5_site;
    original_type5_vcall = *type5_vcall_site;
    original_type5_size_bl = *type5_size_site;
    original_copy_bl = *copy_site;
    original_copy_large_bl = *copy_large_site;
    original_alloc_format_madd = *alloc_format_site;
    original_alloc_size_bl = *alloc_size_site;
    original_alloc_final_mov = *alloc_final_site;
    original_alloc_record_stp = *alloc_record_site;
    type5_vcall_installed = getenv("TOTK_RESOURCE_VCALL") &&
                            strcmp(getenv("TOTK_RESOURCE_VCALL"), "0") != 0;
    type5_helper_installed = getenv("TOTK_RESOURCE_HELPER") &&
                             strcmp(getenv("TOTK_RESOURCE_HELPER"), "0") != 0;
    copy_installed = getenv("TOTK_RESOURCE_COPY") &&
                     strcmp(getenv("TOTK_RESOURCE_COPY"), "0") != 0;
    alloc_inputs_installed = getenv("TOTK_RESOURCE_ALLOC_INPUTS") &&
                             strcmp(getenv("TOTK_RESOURCE_ALLOC_INPUTS"), "0") != 0;
    if ((original_bl & 0xfc000000u) != 0x94000000u)
        runtime_fail("resource trace site is not a BL");
    if ((original_type5_bl & 0xfc000000u) != 0x94000000u)
        runtime_fail("type5 trace site is not a BL");
    if ((original_type5_vcall & 0xfffffc1fu) != 0xd63f0000u)
        runtime_fail("type5 trace site is not a BLR");
    if ((original_type5_size_bl & 0xfc000000u) != 0x94000000u)
        runtime_fail("type5 size trace site is not a BL");
    if ((original_copy_bl & 0xfc000000u) != 0x94000000u)
        runtime_fail("resource copy site is not a BL");
    if ((original_copy_large_bl & 0xfc000000u) != 0x94000000u)
        runtime_fail("resource large-copy site is not a BL");
    if (alloc_inputs_installed && (original_alloc_format_madd & 0xffc00000u) != 0x9b000000u)
        runtime_fail("resource allocation format site is not MADD");
    if (alloc_inputs_installed && (original_alloc_size_bl & 0xff000000u) != 0x2a000000u)
        runtime_fail("resource allocation size site is not MOV W0,W9");
    if (alloc_inputs_installed && (original_alloc_final_mov & 0xff000000u) != 0xaa000000u)
        runtime_fail("resource allocation final site is not MOV W1,W21");
    if (alloc_inputs_installed && (original_alloc_record_stp & 0xffc00000u) != 0x29000000u)
        runtime_fail("resource allocation record site is not STP W");
    const char *loop_env = getenv("TOTK_RESOURCE_LOOP");
    loop_installed = loop_env && *loop_env && strcmp(loop_env, "0") != 0;
    if (loop_installed) {
        uint32_t *call = (uint32_t *)(m->base + 0x3135ae8);
        uint32_t *ret = (uint32_t *)(m->base + 0x3135aec);
        if (*call != 0xd63f0100u || *ret != 0xb94013e3u)
            runtime_fail("resource loop probe instruction mismatch");
        *call = 0xd4200000u | (0x7aau << 5);
        *ret = 0xd4200000u | (0x7abu << 5);
        sys_icache_invalidate(call, 8);
    }
    *site = 0xd4200000u | (0x7a1u << 5);
    if (type5_helper_installed)
        *type5_site = 0xd4200000u | (0x7a2u << 5);
    if (type5_vcall_installed)
        *type5_vcall_site = 0xd4200000u | (0x7a3u << 5);
    *type5_size_site = 0xd4200000u | (0x7a4u << 5);
    if (copy_installed)
        *copy_site = 0xd4200000u | (0x7a5u << 5);
    if (copy_installed)
        *copy_large_site = 0xd4200000u | (0x7a6u << 5);
    if (alloc_inputs_installed) {
        *alloc_format_site = 0xd4200000u | (0x7a0u << 5);
        *alloc_size_site = 0xd4200000u | (0x7a7u << 5);
        *alloc_record_site = 0xd4200000u | (0x7a8u << 5);
        *alloc_final_site = 0xd4200000u | (0x7a9u << 5);
    }
    sys_icache_invalidate(site, sizeof *site);
    if (type5_helper_installed)
        sys_icache_invalidate(type5_site, sizeof *type5_site);
    sys_icache_invalidate(type5_size_site, sizeof *type5_size_site);
    if (copy_installed)
        sys_icache_invalidate(copy_site, sizeof *copy_site);
    if (copy_installed)
        sys_icache_invalidate(copy_large_site, sizeof *copy_large_site);
    if (alloc_inputs_installed) {
        sys_icache_invalidate(alloc_format_site, sizeof *alloc_format_site);
        sys_icache_invalidate(alloc_size_site, sizeof *alloc_size_site);
        sys_icache_invalidate(alloc_record_site, sizeof *alloc_record_site);
        sys_icache_invalidate(alloc_final_site, sizeof *alloc_final_site);
    }
    main_base = m->base;
    sdk_base = runtime_module_base("sdk");
    installed = 1;
    fprintf(stderr, "[resource-trace] installed at main+0x%llx, main+0x%llx, main+0x%llx, and main+0x%llx%s\n",
            (unsigned long long)off, (unsigned long long)type5_off,
            (unsigned long long)type5_vcall_off, (unsigned long long)type5_size_off,
            copy_installed ? ", plus main+0x3136170 and main+0x313ab30 copy probes" : "");
    if (alloc_inputs_installed)
        fprintf(stderr, "[resource-trace] allocation input probes enabled at main+0x35f04e8, main+0x35f0528, main+0x35f0580, and main+0x16edbdc\n");
}

int runtime_resource_trace_trap(void *context) {
    if (!installed || !main_base || !context) return 0;
    ucontext_t *u = context;
    uint64_t pc = u->uc_mcontext->__ss.__pc;
    if (loop_installed && pc == (uint64_t)(uintptr_t)(main_base + 0x3135ae8)) {
        loop_call.wrapper = u->uc_mcontext->__ss.__x[19];
        loop_call.resource = u->uc_mcontext->__ss.__x[2];
        loop_call.index = (uint32_t)u->uc_mcontext->__ss.__x[21];
        loop_call.target = u->uc_mcontext->__ss.__x[8];
        loop_call.out = u->uc_mcontext->__ss.__x[5];
        fprintf(stderr, "[resource-loop-enter] wrapper=%#llx resource=%#llx index=%#llx target=%#llx target_main=%#llx out=%#llx staging=%#llx\n",
                (unsigned long long)loop_call.wrapper, (unsigned long long)loop_call.resource,
                (unsigned long long)loop_call.index, (unsigned long long)loop_call.target,
                (unsigned long long)(loop_call.target - (uint64_t)(uintptr_t)main_base),
                (unsigned long long)loop_call.out,
                (unsigned long long)word(loop_call.wrapper + 0x80 + loop_call.index * 8));
        /* Emulate BLR, not a host call: preserve the guest stack, ABI and thread. */
        u->uc_mcontext->__ss.__lr = pc + 4;
        u->uc_mcontext->__ss.__pc = loop_call.target;
        return 1;
    }
    if (loop_installed && pc == (uint64_t)(uintptr_t)(main_base + 0x3135aec)) {
        uint64_t destination = u->uc_mcontext->__ss.__x[0];
        uint64_t staging = word(loop_call.wrapper + 0x80 + loop_call.index * 8);
        uint64_t meta = word(loop_call.wrapper + 0x60);
        uint64_t table = word(word(meta + 0xd8) + 0x10);
        uint64_t descriptor = word(table + loop_call.index * 8) + 0x10;
        uint32_t explicit_size = dword(u->uc_mcontext->__ss.__sp + 0x10);
        fprintf(stderr, "[resource-loop-return] wrapper=%#llx resource=%#llx index=%#llx destination=%#llx staging=%#llx explicit_size=%#x descriptor=%#llx count=%#llx remaining=%#llx\n",
                (unsigned long long)loop_call.wrapper, (unsigned long long)loop_call.resource,
                (unsigned long long)loop_call.index, (unsigned long long)destination,
                (unsigned long long)staging, explicit_size, (unsigned long long)descriptor,
                (unsigned long long)word(descriptor + 0x10),
                (unsigned long long)runtime_address_region_remaining(destination));
        if (word(descriptor + 0x10) == 0x40000) {
            trace_bytes("failing-staging", staging);
            trace_bytes("failing-staging+0x30000", staging + 0x30000);
            trace_bytes("failing-destination", destination);
            trace_bytes("failing-resource", loop_call.resource);
        }
        /* Emulate the original LDR W3,[SP,#0x10]. */
        u->uc_mcontext->__ss.__x[3] = explicit_size;
        u->uc_mcontext->__ss.__pc = pc + 4;
        return 1;
    }
    const uint64_t sdk_copy_pc = sdk_base
        ? (uint64_t)(uintptr_t)(sdk_base + 0x4d8ad8)
        : 0;
    if (copy_installed && sdk_copy_pc && pc == sdk_copy_pc) {
        uint64_t destination = u->uc_mcontext->__ss.__x[0];
        uint64_t source = u->uc_mcontext->__ss.__x[1];
        uint64_t bytes = u->uc_mcontext->__ss.__x[2];
        fprintf(stderr,
                "[resource-copy-fault] pc=%#llx destination=%#llx source=%#llx "
                "bytes=%#llx remaining=%#llx x19=%#llx x20=%#llx x21=%#llx "
                "lr=%#llx\n",
                (unsigned long long)pc,
                (unsigned long long)destination,
                (unsigned long long)source,
                (unsigned long long)bytes,
                (unsigned long long)runtime_address_region_remaining(destination),
                (unsigned long long)u->uc_mcontext->__ss.__x[19],
                (unsigned long long)u->uc_mcontext->__ss.__x[20],
                (unsigned long long)u->uc_mcontext->__ss.__x[21],
                (unsigned long long)u->uc_mcontext->__ss.__lr);
        return 0;
    }
    if (alloc_inputs_installed) {
        const uint64_t alloc_format_pc = (uint64_t)(uintptr_t)(main_base + 0x35f04e8);
        const uint64_t alloc_size_pc = (uint64_t)(uintptr_t)(main_base + 0x35f0528);
        const uint64_t alloc_record_pc = (uint64_t)(uintptr_t)(main_base + 0x35f0580);
        const uint64_t alloc_final_pc = (uint64_t)(uintptr_t)(main_base + 0x16edbdc);
        if (pc == alloc_format_pc) {
            alloc_format_table = u->uc_mcontext->__ss.__x[12];
            alloc_format_entry = u->uc_mcontext->__ss.__x[11] * u->uc_mcontext->__ss.__x[13] + alloc_format_table;
            fprintf(stderr,
                    "[resource-alloc-format] table=%#llx entry=%#llx index=%#x "
                    "entry+0x08=%#x entry+0x0c=%#x entry+0x14=%#x\n",
                    (unsigned long long)alloc_format_table,
                    (unsigned long long)alloc_format_entry,
                    (unsigned)u->uc_mcontext->__ss.__x[11],
                    dword(alloc_format_entry + 0x08),
                    dword(alloc_format_entry + 0x0c),
                    dword(alloc_format_entry + 0x14));
            u->uc_mcontext->__ss.__x[11] = alloc_format_entry;
            u->uc_mcontext->__ss.__pc = alloc_format_pc + 4;
            return 1;
        }
        if (pc == alloc_size_pc) {
            fprintf(stderr,
                    "[resource-alloc-size] caller=%#llx computed_size=%#llx "
                    "resource=%#llx type=%#llx index=%#llx layout=%#llx "
                    "w8=%#x w9=%#x w10=%#x w11=%#x w12=%#x w13=%#x w14=%#x "
                    "layout+0=%#x layout+0x08=%#x layout+0x10=%#x "
                    "layout+0x14=%#x layout+0x18=%#x layout+0x1c=%#x "
                    "layout+0x20=%#x layout+0x24=%#x layout+0x28=%#llx "
                    "layout+0x30=%#llx layout+0x38=%#llx format=%#llx "
                    "format+0x08=%#x format+0x0c=%#x format+0x14=%#x\n",
                    (unsigned long long)u->uc_mcontext->__ss.__lr,
                    (unsigned long long)u->uc_mcontext->__ss.__x[9],
                    (unsigned long long)u->uc_mcontext->__ss.__x[21],
                    (unsigned long long)(uint32_t)u->uc_mcontext->__ss.__x[20],
                    (unsigned long long)u->uc_mcontext->__ss.__x[22],
                    (unsigned long long)u->uc_mcontext->__ss.__x[0],
                    (unsigned)u->uc_mcontext->__ss.__x[8],
                    (unsigned)u->uc_mcontext->__ss.__x[9],
                    (unsigned)u->uc_mcontext->__ss.__x[10],
                    (unsigned)u->uc_mcontext->__ss.__x[11],
                    (unsigned)u->uc_mcontext->__ss.__x[12],
                    (unsigned)u->uc_mcontext->__ss.__x[13],
                    (unsigned)u->uc_mcontext->__ss.__x[14],
                    dword(u->uc_mcontext->__ss.__x[0] + 0x00),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x08),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x10),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x14),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x18),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x1c),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x20),
                    dword(u->uc_mcontext->__ss.__x[0] + 0x24),
                    (unsigned long long)word(u->uc_mcontext->__ss.__x[0] + 0x28),
                    (unsigned long long)word(u->uc_mcontext->__ss.__x[0] + 0x30),
                    (unsigned long long)word(u->uc_mcontext->__ss.__x[0] + 0x38),
                    (unsigned long long)u->uc_mcontext->__ss.__x[11],
                    dword(u->uc_mcontext->__ss.__x[11] + 0x08),
                    dword(u->uc_mcontext->__ss.__x[11] + 0x0c),
                    dword(u->uc_mcontext->__ss.__x[11] + 0x14));
            uint64_t effective_size = u->uc_mcontext->__ss.__x[9];
            const char *watch_resource_text = getenv("TOTK_RESOURCE_ALLOC_WATCH");
            const char *watch_size_text = getenv("TOTK_RESOURCE_ALLOC_WATCH_SIZE");
            if (watch_resource_text && watch_size_text &&
                ((strcmp(watch_resource_text, "all-30000") == 0 && effective_size == 0x30000) ||
                 u->uc_mcontext->__ss.__x[21] == strtoull(watch_resource_text, NULL, 0))) {
                uint64_t watch_size = strtoull(watch_size_text, NULL, 0);
                fprintf(stderr, "[resource-alloc-watch] resource=%#llx original=%#llx effective=%#llx\n",
                        (unsigned long long)u->uc_mcontext->__ss.__x[21],
                        (unsigned long long)effective_size,
                        (unsigned long long)watch_size);
                effective_size = watch_size;
                u->uc_mcontext->__ss.__x[9] = effective_size;
            }
            alloc_input_size = (uint32_t)effective_size;
            alloc_input_alignment = (uint32_t)u->uc_mcontext->__ss.__x[19];
            alloc_input_type = (uint32_t)u->uc_mcontext->__ss.__x[20];
            alloc_input_index = (uint32_t)u->uc_mcontext->__ss.__x[22];
            u->uc_mcontext->__ss.__x[0] = u->uc_mcontext->__ss.__x[9];
            u->uc_mcontext->__ss.__pc = alloc_size_pc + 4;
            return 1;
        }
        if (pc == alloc_record_pc) {
            uint64_t record = u->uc_mcontext->__ss.__x[8];
            fprintf(stderr,
                    "[resource-alloc-record] record=%#llx resource=%#llx type=%#llx "
                    "index=%#llx alloc_object=%#llx\n",
                    (unsigned long long)record,
                    (unsigned long long)u->uc_mcontext->__ss.__x[21],
                    (unsigned long long)(uint32_t)u->uc_mcontext->__ss.__x[20],
                    (unsigned long long)(uint32_t)u->uc_mcontext->__ss.__x[19],
                    (unsigned long long)u->uc_mcontext->__ss.__x[22]);
            uint64_t fields = (uint64_t)(uint32_t)u->uc_mcontext->__ss.__x[20] << 32;
            *(uint64_t *)(uintptr_t)(record + 0x10) = fields;
            u->uc_mcontext->__ss.__pc = alloc_record_pc + 4;
            return 1;
        }
        if (pc == alloc_final_pc) {
            fprintf(stderr,
                    "[resource-alloc-final] x23=%#llx x19=%#llx x21=%#llx x25=%#llx "
                    "x28=%#llx lr=%#llx\n",
                    (unsigned long long)u->uc_mcontext->__ss.__x[23],
                    (unsigned long long)u->uc_mcontext->__ss.__x[19],
                    (unsigned long long)u->uc_mcontext->__ss.__x[21],
                    (unsigned long long)u->uc_mcontext->__ss.__x[25],
                    (unsigned long long)u->uc_mcontext->__ss.__x[28],
                    (unsigned long long)u->uc_mcontext->__ss.__lr);
            u->uc_mcontext->__ss.__x[0] = u->uc_mcontext->__ss.__x[23];
            u->uc_mcontext->__ss.__pc = alloc_final_pc + 4;
            return 1;
        }
    }
    const uint64_t type5_pc = (uint64_t)(uintptr_t)(main_base + 0x313a7b8);
    const uint64_t type5_vcall_pc = (uint64_t)(uintptr_t)(main_base + 0x313a7a0);
    const uint64_t type5_size_pc = (uint64_t)(uintptr_t)(main_base + 0x313aa10);
    if (pc == type5_vcall_pc && !type5_vcall_installed) return 0;
    if (pc == type5_pc && !type5_helper_installed) return 0;
    const uint64_t copy_pc = (uint64_t)(uintptr_t)(main_base + 0x3136170);
    const uint64_t copy_large_pc = (uint64_t)(uintptr_t)(main_base + 0x313ab30);
    if (pc == copy_pc && !copy_installed) return 0;
    if (pc == copy_large_pc && !copy_installed) return 0;
    if (pc == copy_pc || pc == copy_large_pc) {
        uint64_t destination = u->uc_mcontext->__ss.__x[0];
        uint64_t source = u->uc_mcontext->__ss.__x[1];
        uint64_t bytes = u->uc_mcontext->__ss.__x[2];
        uint64_t remaining = runtime_address_region_remaining(destination);
        fprintf(stderr,
                "[resource-copy] site=%#llx object=%#llx type=%#llx index=%#llx "
                "descriptor=%#llx producer=%#llx entry=%#llx entry_source=%#llx "
                "destination=%#llx source=%#llx bytes=%#llx remaining=%#llx "
                "x19=%#llx x20=%#llx x21=%#llx\n",
                (unsigned long long)pc,
                (unsigned long long)last_type5_object,
                (unsigned long long)last_type5_type,
                (unsigned long long)last_type5_index,
                (unsigned long long)last_type5_descriptor,
                (unsigned long long)last_type5_producer,
                (unsigned long long)last_type5_entry,
                (unsigned long long)last_type5_entry_source,
                (unsigned long long)destination,
                (unsigned long long)source,
                (unsigned long long)bytes,
                (unsigned long long)remaining,
                (unsigned long long)u->uc_mcontext->__ss.__x[19],
                (unsigned long long)u->uc_mcontext->__ss.__x[20],
                (unsigned long long)u->uc_mcontext->__ss.__x[21]);
        uint64_t target = (uint64_t)(uintptr_t)(main_base + 0x47ef420);
        uint64_t result = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))(uintptr_t)target)(
            destination, source, bytes);
        u->uc_mcontext->__ss.__x[0] = result;
        u->uc_mcontext->__ss.__pc = pc == copy_pc
            ? (uint64_t)(uintptr_t)(main_base + 0x3136174)
            : (uint64_t)(uintptr_t)(main_base + 0x313ab34);
        return 1;
    }
    if (pc == type5_size_pc) {
        uint64_t descriptor = u->uc_mcontext->__ss.__x[0];
        uint64_t destination = u->uc_mcontext->__ss.__x[19];
        uint64_t source = u->uc_mcontext->__ss.__x[20];
        uint64_t result = ((uint64_t (*)(uint64_t))(uintptr_t)(main_base + 0x18f0b80))(descriptor);
        if (result >= 0x30000 || result == 0x3fe0) {
            uint64_t vtable = word(descriptor);
            uint64_t count = word(descriptor + 0x10);
            uint64_t size_fn = word(vtable + 0x10);
            uint32_t element = size_fn ? ((uint32_t (*)(uint64_t))(uintptr_t)size_fn)(descriptor) : 0;
            uint64_t remaining = runtime_address_region_remaining(destination);
            fprintf(stderr,
                    "[resource-type5-size] descriptor=%#llx count=%#llx element=%#x "
                    "source=%#llx destination=%#llx remaining=%#llx result=%#llx "
                    "x1=%#llx x2=%#llx x19=%#llx x20=%#llx x21=%#llx\n",
                    (unsigned long long)descriptor, (unsigned long long)count,
                    element, (unsigned long long)source,
                    (unsigned long long)destination, (unsigned long long)remaining,
                    (unsigned long long)result,
                    (unsigned long long)u->uc_mcontext->__ss.__x[1],
                    (unsigned long long)u->uc_mcontext->__ss.__x[2],
                    (unsigned long long)destination, (unsigned long long)source,
                    (unsigned long long)u->uc_mcontext->__ss.__x[21]);
        }
        u->uc_mcontext->__ss.__x[0] = result;
        u->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)(main_base + 0x313aa14);
        return 1;
    }
    if (pc == type5_vcall_pc) {
        uint64_t manager = u->uc_mcontext->__ss.__x[0];
        uint64_t resource = u->uc_mcontext->__ss.__x[1];
        uint64_t input = u->uc_mcontext->__ss.__x[2];
        uint64_t type = u->uc_mcontext->__ss.__x[3];
        uint64_t explicit_size = u->uc_mcontext->__ss.__x[4];
        uint64_t index = u->uc_mcontext->__ss.__x[5];
        uint64_t mode = u->uc_mcontext->__ss.__x[6];
        uint64_t out_ptr = u->uc_mcontext->__ss.__x[7];
        uint64_t vcall = u->uc_mcontext->__ss.__x[8];
        uint64_t result = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t, uint64_t))(uintptr_t)vcall)(
            manager, resource, input, type, explicit_size, index, mode, out_ptr);
        uint64_t out_value = word(out_ptr);
        if (getenv("TOTK_RESOURCE_TYPE5")) {
            fprintf(stderr,
                    "[resource-type5-vcall] manager=%#llx resource=%#llx input=%#llx "
                    "type=%#llx index=%#llx explicit_size=%#llx mode=%#llx "
                    "out_ptr=%#llx out=%#llx result=%#llx vcall=%#llx\n",
                    (unsigned long long)manager, (unsigned long long)resource,
                    (unsigned long long)input, (unsigned long long)type,
                    (unsigned long long)index, (unsigned long long)explicit_size,
                    (unsigned long long)mode, (unsigned long long)out_ptr,
                    (unsigned long long)out_value, (unsigned long long)result,
                    (unsigned long long)vcall);
            if (type == 5 && index == 0 && result >= 0x7000000000ULL && result < 0x9000000000ULL) {
                trace_bytes("source", result);
                trace_bytes("source+0x3ffc0", result + 0x3ffc0);
                trace_bytes("input", input);
                trace_bytes("input+0x40", input + 0x40);
            }
        }
        u->uc_mcontext->__ss.__x[0] = result;
        u->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)(main_base + 0x313a7a4);
        return 1;
    }
    if (pc == type5_pc) {
        uint64_t object = u->uc_mcontext->__ss.__x[0];
        uint64_t type = u->uc_mcontext->__ss.__x[1];
        uint64_t index = u->uc_mcontext->__ss.__x[2];
        uint64_t source = u->uc_mcontext->__ss.__x[3];
        uint64_t bytes = u->uc_mcontext->__ss.__x[4];
        uint64_t entry = object + type * 0x70 + index * 8;
        uint64_t entry_source = word(entry + 0x70);
        last_type5_object = object;
        last_type5_type = type;
        last_type5_index = index;
        last_type5_destination = source;
        last_type5_source = entry_source;
        last_type5_entry = entry;
        last_type5_entry_source = entry_source;
        last_type5_descriptor = type5_descriptor(object, index);
        last_type5_producer = source;
        trace_type5(object, source, bytes, type, index, entry, entry_source);
        uint64_t helper = (uint64_t)(uintptr_t)(main_base + 0x313a9a0);
        ((void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))(uintptr_t)helper)(
            object, type, index, source, bytes);
        u->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)(main_base + 0x313a7bc);
        return 1;
    }
    if (pc != (uint64_t)(uintptr_t)(main_base + 0x31360d4)) return 0;
    unsigned n = atomic_fetch_add(&records, 1);
    uint64_t x0 = u->uc_mcontext->__ss.__x[0];
    uint64_t x8 = u->uc_mcontext->__ss.__x[8];
    uint64_t x19 = u->uc_mcontext->__ss.__x[19];
    uint64_t x20 = u->uc_mcontext->__ss.__x[20];
    uint64_t x21 = u->uc_mcontext->__ss.__x[21];
    uint64_t descriptor = x8 + 0x10;
    uint64_t vtable = word(descriptor);
    uint64_t count = word(descriptor + 0x10);
    uint64_t size_fn = word(vtable + 0x10);
    uint32_t element = 0;
    if (size_fn) element = ((uint32_t (*)(uint64_t))(uintptr_t)size_fn)(descriptor);
    uint64_t bytes = count * (uint64_t)(int32_t)element;
    uint64_t helper = (uint64_t)(uintptr_t)(main_base + 0x18f0b80);
    uint64_t result = ((uint64_t (*)(uint64_t))(uintptr_t)helper)(descriptor);
    if (getenv("TOTK_RESOURCE_TRACE") && x19 == strtoull(getenv("TOTK_RESOURCE_WATCH_DST") ? getenv("TOTK_RESOURCE_WATCH_DST") : "0", NULL, 0)) {
        fprintf(stderr, "[resource-trace-watch] record=%u src=%#llx dst=%#llx descriptor=%#llx count=%#llx element=%#x computed=%#llx\\n",
                n, (unsigned long long)x20, (unsigned long long)x19,
                (unsigned long long)descriptor, (unsigned long long)count,
                element, (unsigned long long)bytes);
    }
    const char *bound = getenv("TOTK_RESOURCE_BOUND");
    if (bound && *bound && strcmp(bound, "0") != 0) {
        uint64_t available = runtime_address_region_remaining(x19);
        if (available && result > available) {
            fprintf(stderr, "[resource-trace] diagnostic bound %#llx -> %#llx dst=%#llx\\n",
                    (unsigned long long)result, (unsigned long long)available,
                    (unsigned long long)x19);
            result = available;
        }
    }
    const char *clamp = getenv("TOTK_RESOURCE_CLAMP");
    if (clamp && *clamp && strcmp(clamp, "0") != 0 && result == 0x40000) {
        const char *size_text = getenv("TOTK_RESOURCE_CLAMP_SIZE");
        uint64_t clamp_size = size_text && *size_text ? strtoull(size_text, NULL, 0) : 0x30000;
        fprintf(stderr, "[resource-trace] diagnostic clamp %#llx -> %#llx\n",
                (unsigned long long)result, (unsigned long long)clamp_size);
        result = clamp_size;
    }
    if (n < 64 || bytes == 0x40000 ||
        (x20 >= 0x7000000000ULL && x20 < 0x9000000000ULL &&
         x19 >= 0x7000000000ULL && x19 < 0x9000000000ULL)) {
        fprintf(stderr,
                "[resource-trace] entry=%#llx descriptor=%#llx vtable=%#llx count=%#llx size_fn=%#llx element=%#x computed=%#llx helper=%#llx result=%#llx src=%#llx dst=%#llx record=%u\n",
                (unsigned long long)x21, (unsigned long long)descriptor,
                (unsigned long long)vtable, (unsigned long long)count,
                (unsigned long long)size_fn, element,
                (unsigned long long)bytes, (unsigned long long)helper,
                (unsigned long long)result, (unsigned long long)x20,
                (unsigned long long)x19, n);
        if (bytes == 0x40000) {
            uint64_t object = word(x21 + 0x70);
            fprintf(stderr, "[resource-trace-fields] x0=%#llx x8=%#llx current_entry=%#llx original_entry=%#llx current+0=%#llx current+0x08=%#llx current+0x10=%#llx current+0x18=%#llx current+0x20=%#llx current+0x28=%#llx current+0x30=%#llx current+0x38=%#llx current+0x40=%#llx current+0x48=%#llx current+0x50=%#llx current+0x58=%#llx current+0x60=%#llx current+0x68=%#llx current+0x70=%#llx current+0x78=%#llx current+0x80=%#llx original+0=%#llx original+0x08=%#llx original+0x10=%#llx original+0x18=%#llx original+0x20=%#llx original+0x28=%#llx original+0x30=%#llx original+0x38=%#llx original+0x40=%#llx original+0x48=%#llx original+0x50=%#llx original+0x58=%#llx original+0x60=%#llx original+0x68=%#llx original+0x70=%#llx original+0x78=%#llx descriptor+0=%#llx descriptor+0x08=%#llx descriptor+0x10=%#llx descriptor+0x18=%#llx descriptor+0x20=%#llx object=%#llx object+0=%#llx object+0x08=%#llx object+0x10=%#llx object+0x18=%#llx object+0x20=%#llx object+0x28=%#llx object+0x30=%#llx object+0x38=%#llx object+0x40=%#llx object+0x48=%#llx object+0x50=%#llx object+0x58=%#llx object+0x60=%#llx object+0x68=%#llx object+0x70=%#llx object+0x78=%#llx\n",
                    (unsigned long long)x0, (unsigned long long)x8,
                    (unsigned long long)x21, (unsigned long long)(x21 - 0x80),
                    (unsigned long long)word(x21 + 0x00), (unsigned long long)word(x21 + 0x08),
                    (unsigned long long)word(x21 + 0x10), (unsigned long long)word(x21 + 0x18),
                    (unsigned long long)word(x21 + 0x20), (unsigned long long)word(x21 + 0x28),
                    (unsigned long long)word(x21 + 0x30), (unsigned long long)word(x21 + 0x38),
                    (unsigned long long)word(x21 + 0x40), (unsigned long long)word(x21 + 0x48),
                    (unsigned long long)word(x21 + 0x50), (unsigned long long)word(x21 + 0x58),
                    (unsigned long long)word(x21 + 0x60), (unsigned long long)word(x21 + 0x68),
                    (unsigned long long)word(x21 + 0x70), (unsigned long long)word(x21 + 0x78),
                    (unsigned long long)word(x21 + 0x80),
                    (unsigned long long)word(x21 - 0x80), (unsigned long long)word(x21 - 0x78),
                    (unsigned long long)word(x21 - 0x70), (unsigned long long)word(x21 - 0x68),
                    (unsigned long long)word(x21 - 0x60), (unsigned long long)word(x21 - 0x58),
                    (unsigned long long)word(x21 - 0x50), (unsigned long long)word(x21 - 0x48),
                    (unsigned long long)word(x21 - 0x40), (unsigned long long)word(x21 - 0x38),
                    (unsigned long long)word(x21 - 0x30), (unsigned long long)word(x21 - 0x28),
                    (unsigned long long)word(x21 - 0x20), (unsigned long long)word(x21 - 0x18),
                    (unsigned long long)word(x21 - 0x10), (unsigned long long)word(x21 - 0x08),
                    (unsigned long long)word(descriptor + 0x00), (unsigned long long)word(descriptor + 0x08),
                    (unsigned long long)word(descriptor + 0x10), (unsigned long long)word(descriptor + 0x18),
                    (unsigned long long)word(descriptor + 0x20), (unsigned long long)object,
                    (unsigned long long)word(object + 0x00), (unsigned long long)word(object + 0x08),
                    (unsigned long long)word(object + 0x10), (unsigned long long)word(object + 0x18),
                    (unsigned long long)word(object + 0x20), (unsigned long long)word(object + 0x28),
                    (unsigned long long)word(object + 0x30), (unsigned long long)word(object + 0x38),
                    (unsigned long long)word(object + 0x40), (unsigned long long)word(object + 0x48),
                    (unsigned long long)word(object + 0x50), (unsigned long long)word(object + 0x58),
                    (unsigned long long)word(object + 0x60), (unsigned long long)word(object + 0x68),
                    (unsigned long long)word(object + 0x70), (unsigned long long)word(object + 0x78));
        }
    }
    u->uc_mcontext->__ss.__x[0] = result;
    u->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)(main_base + 0x31360d8);
    (void)x0;
    return 1;
}
