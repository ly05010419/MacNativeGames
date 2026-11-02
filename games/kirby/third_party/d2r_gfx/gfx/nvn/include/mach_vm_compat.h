/* mach_vm_* on every Apple target. The iOS SDK ships <mach/mach_vm.h> as a hard #error; there the same kernel calls
   are reached through the vm_* interface, which on arm64 takes the same 64-bit addresses and sizes. */
#pragma once
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
static inline kern_return_t mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, int f) {
    vm_address_t v = (vm_address_t)*a; kern_return_t kr = vm_allocate(t, &v, (vm_size_t)s, f); *a = v; return kr;
}
static inline kern_return_t mach_vm_deallocate(vm_map_t t, mach_vm_address_t a, mach_vm_size_t s) {
    return vm_deallocate(t, (vm_address_t)a, (vm_size_t)s);
}
static inline kern_return_t mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_offset_t mask, int f,
                                        mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy,
                                        vm_prot_t cur, vm_prot_t max, vm_inherit_t inh) {
    vm_address_t v = (vm_address_t)*a;
    kern_return_t kr = vm_map(t, &v, (vm_size_t)s, (vm_address_t)mask, f, obj, (vm_offset_t)off, copy, cur, max, inh);
    *a = v; return kr;
}
static inline kern_return_t mach_vm_read_overwrite(vm_map_t t, mach_vm_address_t a, mach_vm_size_t s, mach_vm_address_t data,
                                                   mach_vm_size_t *out) {
    vm_size_t n = 0; kern_return_t kr = vm_read_overwrite(t, (vm_address_t)a, (vm_size_t)s, (vm_address_t)data, &n);
    *out = n; return kr;
}
static inline kern_return_t mach_vm_region(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t *s, vm_region_flavor_t flavor,
                                           vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj) {
    vm_address_t v = (vm_address_t)*a; vm_size_t n = 0;
    kern_return_t kr = vm_region_64(t, &v, &n, flavor, info, cnt, obj);
    *a = v; *s = n; return kr;
}
static inline kern_return_t mach_vm_remap(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_offset_t mask, int f,
                                          vm_map_t src, mach_vm_address_t src_a, boolean_t copy, vm_prot_t *cur,
                                          vm_prot_t *max, vm_inherit_t inh) {
    vm_address_t v = (vm_address_t)*a;
    kern_return_t kr = vm_remap(t, &v, (vm_size_t)s, (vm_address_t)mask, f, src, (vm_address_t)src_a, copy, cur, max, inh);
    *a = v; return kr;
}
#else
#include <mach/mach_vm.h>
#endif
