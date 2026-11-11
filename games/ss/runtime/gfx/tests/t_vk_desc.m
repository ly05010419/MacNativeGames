// Vulkan descriptors outlive the objects written into them (GitHub issue 68): a set names an image view, a sampler and a uniform buffer in element 1 of an 8-texture
// array and a buffer binding, as Super Mario Galaxy 2's shaders take them; the objects are destroyed and their host structs freed and overwritten, and the set bound
// for a draw still binds the texture, sampler state and buffer it was written with. Resetting the pool releases them
#include "../vk/hle_vk.m"
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    __weak id<MTLTexture> wtex; __weak id<MTLSamplerState> wsmp; __weak id<MTLBuffer> wbuf;
    VkDevice dev = &g_device; VkDescriptorPool pool; VkDescriptorSet ds; VkDescriptorSetLayout L;
    @autoreleasepool {
        dev_init();
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {4, 4, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
        VkImage im; CHECK(h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS, "image created");
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                     .components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A},   // a view texture of its own, not the image's
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView view; CHECK(h_vkCreateImageView(dev, &vci, NULL, &view) == VK_SUCCESS && view->tex && view->tex != im->tex, "swizzled view has its own texture");
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR};
        VkSampler smp; CHECK(h_vkCreateSampler(dev, &sci, NULL, &smp) == VK_SUCCESS && smp->s, "sampler created");
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VkBuffer buf; h_vkCreateBuffer(dev, &bci, NULL, &buf);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 4096, .memoryTypeIndex = 1};
        VkDeviceMemory mem; h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(h_vkBindBufferMemory(dev, buf, mem, 512) == VK_SUCCESS, "buffer bound");
        wtex = view->tex; wsmp = smp->s; wbuf = mem->buf;

        VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                              {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
        h_vkCreateDescriptorSetLayout(dev, &lci, NULL, &L);
        VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1};
        h_vkCreateDescriptorPool(dev, &pci, NULL, &pool);
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &L};
        h_vkAllocateDescriptorSets(dev, &ai, &ds);
        VkDescriptorImageInfo ii = {smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi = {buf, 64, 128};
        VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .dstArrayElement = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi}};
        h_vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

        // The game destroys everything the set names, then the freed host structs are reused (the allocator's free-list word, then other objects' data)
        h_vkDestroyImageView(dev, view, NULL); h_vkDestroyImage(dev, im, NULL); h_vkDestroySampler(dev, smp, NULL); h_vkDestroyBuffer(dev, buf, NULL); h_vkFreeMemory(dev, mem, NULL);
        void *junk[4096];
        for (int i = 0; i < 4096; i++) { size_t n = 8 + 8 * (i % 8); junk[i] = malloc(n); memset(junk[i], 0xa5, n); }
        CHECK(wtex && wsmp && wbuf, "the descriptors keep the texture, sampler state and buffer alive");
        const desc_t *e = &ds->d[L->b[L->idx[0]].base + 1], *u = &ds->d[L->b[L->idx[1]].base];
        CHECK(e->tex == wtex && e->smp == wsmp, "element 1 holds the view's texture and the sampler state");
        CHECK(u->buf == wbuf && u->off == 512 + 64, "the buffer descriptor holds the memory's buffer at the buffer's offset plus its own");

        // A fragment shader taking c:0.0=0*8 and u:0.1=0, bound for a draw as pre_draw does
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = [g_dev newTextureWithDescriptor:td]; rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        shinfo si = {.key = "t_vk_desc", .frag = 1, .nres = 2, .res = {{'c', 0, 0, 8, 0}, {'u', 0, 0, 1, 1}}};
        rs_t s = {0}; s.cb = [g_q commandBuffer]; s.re = [s.cb renderCommandEncoderWithDescriptor:rpd]; s.set[0].s = ds;
        bind_res(&s, &si, 1);
        [s.re endEncoding]; s.re = nil; [s.cb commit]; [s.cb waitUntilCompleted];
        CHECK(s.cb.status == MTLCommandBufferStatusCompleted, "the bound pass completed");
        s.cb = nil;
        for (int i = 0; i < 4096; i++) free(junk[i]);
    }
    @autoreleasepool { h_vkResetDescriptorPool(dev, pool, 0); }
    CHECK(!wtex && !wsmp && !wbuf, "resetting the pool releases what its sets held");
    // A stencil-aspect view of a depth-stencil image without the mutable-format flag (PR #106): an X32_Stencil8 view, which Metal's validation allows only with the format-view usage
    @autoreleasepool {
        VkFormat dsf[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
        for (int i = 0; i < 2; i++) {
            VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = dsf[i], .extent = {4, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
            VkImage im; CHECK(h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS && (im->tex.usage & MTLTextureUsagePixelFormatView), "a depth-stencil image has the format-view usage");
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dsf[i],
                                         .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
            VkImageView sv; CHECK(h_vkCreateImageView(dev, &vci, NULL, &sv) == VK_SUCCESS && sv->tex.pixelFormat == MTLPixelFormatX32_Stencil8 && sv->tex.parentTexture == im->tex, "its stencil-aspect view is a stencil-only view of it");
            h_vkDestroyImageView(dev, sv, NULL); h_vkDestroyImage(dev, im, NULL);
        }
    }
    printf("Vulkan descriptors: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
