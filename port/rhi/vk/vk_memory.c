/* vk_memory.c: the memory types an allocation of each kind (RhiMemory)
 * tries, in order.  Pure: no device and no Vulkan call, so
 * test/rhi_vk_memory_test.c compiles it on its own.  vkr_allocate
 * (vk_resource.c) walks the list and takes the next type when the driver
 * refuses one, so a full heap costs speed, not the picture. */
#include "vk_internal.h"
#include <stdlib.h>

/* every type of typeBits whose flags hold `want` and none of `avoid`, in
   index order, that is not listed yet and not in skip */
static void vkr_add_types(const VkPhysicalDeviceMemoryProperties *props, uint32_t typeBits,
                          VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid, uint32_t skip,
                          uint32_t *listed, uint32_t *out, uint32_t *n)
{
    for (uint32_t i = 0; i < props->memoryTypeCount && i < 32u; i++) {
        const VkMemoryPropertyFlags f = props->memoryTypes[i].propertyFlags;
        const uint32_t bit = 1u << i;
        if ((typeBits & bit) && !(*listed & bit) && !(skip & bit) && (f & want) == want &&
            (f & avoid) == 0) {
            *listed |= bit;
            out[(*n)++] = i;
        }
    }
}

bool vkr_memory_small_window(const VkPhysicalDeviceMemoryProperties *props, uint32_t type)
{
    const VkMemoryPropertyFlags dlhv =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (type >= props->memoryTypeCount || (props->memoryTypes[type].propertyFlags & dlhv) != dlhv) {
        return false;
    }
    const uint32_t heap = props->memoryTypes[type].heapIndex;
    return heap < props->memoryHeapCount &&
           props->memoryHeaps[heap].size < VKR_SMALL_WINDOW_HEAP_BYTES;
}

uint32_t vkr_memory_candidates(RhiMemory kind, uint32_t typeBits, VkDeviceSize size,
                               const VkPhysicalDeviceMemoryProperties *props, uint32_t full,
                               uint32_t out[VK_MAX_MEMORY_TYPES])
{
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags hc = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags hcached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t n = 0, listed = 0, skip = full;

    switch (kind) {
    case RHI_MEM_DEVICE:
        vkr_add_types(props, typeBits, dl, 0, skip, &listed, out, &n);
        vkr_add_types(props, typeBits, 0, 0, skip, &listed, out, &n);
        break;
    case RHI_MEM_UPLOAD:
        /* device-local and host-visible (resizable BAR, UMA, or the BAR
           window) first, then plain host memory; always coherent so writes
           need no flush.  A window under 1 GB (no resizable BAR: about
           256 MB that the driver and every upload buffer share) takes only
           the small buffers, so the renderer's growing upload rings do not
           fill it */
        if (size > VKR_SMALL_WINDOW_MAX_BYTES) {
            for (uint32_t i = 0; i < props->memoryTypeCount && i < 32u; i++) {
                if (vkr_memory_small_window(props, i)) {
                    skip |= 1u << i;
                }
            }
        }
        {
            /* host-cached system memory first: the renderer writes its
               uniform blocks, streams and vertices into the upload rings
               every picture, and write-combined stores across the bus to a
               resizable-BAR window cost the render thread far more than the
               GPU's cached reads across it (RTX 5090, ICO VR stage 16 at
               120 Hz: render thread 6.5 -> 5.0 ms a picture, GPU 1.8 ->
               1.9; flat 2.4 -> 1.9).  ICO_RHI_UPLOAD_CACHED=0: the
               device-local window first, as before */
            static int s_cached = -1;
            if (s_cached < 0) {
                const char *e = getenv("ICO_RHI_UPLOAD_CACHED");
                s_cached = !(e && e[0] == '0');
            }
            if (s_cached) {
                vkr_add_types(props, typeBits, hv | hc | hcached, dl, skip, &listed, out, &n);
            }
        }
        vkr_add_types(props, typeBits, hv | hc | dl, 0, skip, &listed, out, &n);
        vkr_add_types(props, typeBits, hv | hc, dl, skip, &listed, out, &n);
        vkr_add_types(props, typeBits, hv | hc, 0, skip, &listed, out, &n);
        break;
    case RHI_MEM_READBACK:
        vkr_add_types(props, typeBits, hv | hc | hcached, 0, skip, &listed, out, &n);
        vkr_add_types(props, typeBits, hv | hcached, 0, skip, &listed, out, &n);
        vkr_add_types(props, typeBits, hv | hc, 0, skip, &listed, out, &n);
        break;
    }
    /* last resorts: the types the rules accept that were left out (the
       small window for a large upload, a type that was full before), in
       the rules' order */
    if (skip != 0) {
        const uint32_t again = skip & ~listed;
        uint32_t m = n;
        uint32_t tmp[VK_MAX_MEMORY_TYPES];
        uint32_t k = vkr_memory_candidates(kind, typeBits & again, 0, props, 0, tmp);
        for (uint32_t j = 0; j < k && m < VK_MAX_MEMORY_TYPES; j++) {
            out[m++] = tmp[j];
        }
        n = m;
    }
    return n;
}
