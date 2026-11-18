#include "image.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static int protection(void *p) {
    mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = mach_vm_region(mach_task_self(), &address, &size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (result != KERN_SUCCESS || address > (mach_vm_address_t)(uintptr_t)p) return -1;
    return info.protection;
}

int main(void) {
    const size_t page = 0x4000;
    uint8_t *base = mmap(NULL, 5 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) return 1;
    TmntImage image = {base, 5 * page, 3 * page, 4 * page, 32, 0};
    if (tmnt_image_seal(&image, page + 1, page + 8) != TMNT_IMAGE_LAYOUT ||
        image.base != base || protection(base) != (VM_PROT_READ | VM_PROT_WRITE)) return 2;
    if (tmnt_image_seal(&image, 12, 2 * page + 12) != TMNT_IMAGE_OK) return 3;
    const int expected[] = {VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ,
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ, VM_PROT_READ | VM_PROT_EXECUTE};
    for (size_t i = 0; i < 5; ++i) {
        if (protection(base + i * page) != expected[i]) {
            fprintf(stderr, "unexpected page protection at index %zu\n", i);
            return 4;
        }
    }
    if (tmnt_image_seal(&image, 12, 2 * page) != TMNT_IMAGE_ARGUMENT) return 5;
    tmnt_image_unmap(&image);
    return image.base != NULL || image.mapped_size != 0 || image.sealed != 0;
}
