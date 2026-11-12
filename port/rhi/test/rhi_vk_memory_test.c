/* rhi_vk_memory_test.c: the memory types each allocation kind tries
 * (vk/vk_memory.c vkr_memory_candidates), over hand-built memory property
 * tables: no device, no Vulkan call.
 *
 * The rules: uploads take a device-local, host-visible type first, then
 * system memory (host-visible, not device-local), then any host-visible
 * type; a device-local, host-visible heap under 1 GB (the PCIe BAR window
 * without resizable BAR, 214 MB on the RTX 3070 of issue 87) takes uploads
 * of up to 16 MB only; a window that refused an allocation (`full`) is
 * tried last; nothing is listed twice. */
#include "vk/vk_internal.h"
#include <stdio.h>
#include <string.h>

#define MB (1024ull * 1024ull)

static int s_failures;

static void expect(const char *what, const uint32_t *got, uint32_t n, const uint32_t *want,
                   uint32_t wn)
{
    bool same = n == wn;
    for (uint32_t i = 0; same && i < n; i++) {
        same = got[i] == want[i];
    }
    if (!same) {
        printf("FAIL %s: got", what);
        for (uint32_t i = 0; i < n; i++) {
            printf(" %u", got[i]);
        }
        printf(", want");
        for (uint32_t i = 0; i < wn; i++) {
            printf(" %u", want[i]);
        }
        printf("\n");
        s_failures++;
    }
}

#define DL VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
#define HV VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
#define HC VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
#define CA VK_MEMORY_PROPERTY_HOST_CACHED_BIT

static void addHeap(VkPhysicalDeviceMemoryProperties *p, unsigned long long bytes, bool dl)
{
    p->memoryHeaps[p->memoryHeapCount].size = bytes;
    p->memoryHeaps[p->memoryHeapCount].flags = dl ? VK_MEMORY_HEAP_DEVICE_LOCAL_BIT : 0;
    p->memoryHeapCount++;
}

static void addType(VkPhysicalDeviceMemoryProperties *p, VkMemoryPropertyFlags f, uint32_t heap)
{
    p->memoryTypes[p->memoryTypeCount].propertyFlags = f;
    p->memoryTypes[p->memoryTypeCount].heapIndex = heap;
    p->memoryTypeCount++;
}

/* an NVIDIA desktop card on Windows without resizable BAR, as the reporter
   of issue 87 had: heap 0 VRAM, heap 1 system memory, heap 2 the 214 MB
   window.  Types: 0 VRAM, 1 system (coherent), 2 system (coherent,
   cached), 3 the window */
static VkPhysicalDeviceMemoryProperties nvidiaNoRebar(unsigned long long windowBytes)
{
    VkPhysicalDeviceMemoryProperties p;
    memset(&p, 0, sizeof(p));
    addHeap(&p, 8018 * MB, true);
    addHeap(&p, 16313 * MB, false);
    addHeap(&p, windowBytes, true);
    addType(&p, DL, 0);
    addType(&p, HV | HC, 1);
    addType(&p, HV | HC | CA, 1);
    addType(&p, DL | HV | HC, 2);
    return p;
}

static void smallWindow(void)
{
    const VkPhysicalDeviceMemoryProperties p = nvidiaNoRebar(214 * MB);
    uint32_t out[VK_MAX_MEMORY_TYPES], n;

    /* a 4 MB ring: cached system memory (the CPU's writes cached), the
       window, then coherent system memory */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0xF, 4 * MB, &p, 0, out);
    expect("4 MB upload", out, n, (const uint32_t[]){2, 3, 1}, 3);
    /* 16 MB is the most the window takes */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0xF, 16 * MB, &p, 0, out);
    expect("16 MB upload", out, n, (const uint32_t[]){2, 3, 1}, 3);
    /* a 32 MB ring goes to system memory, the window last */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0xF, 32 * MB, &p, 0, out);
    expect("32 MB upload", out, n, (const uint32_t[]){2, 1, 3}, 3);
    /* the window refused once: every upload tries it last */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0xF, 64 * 1024, &p, 1u << 3, out);
    expect("64 KB upload, window full", out, n, (const uint32_t[]){2, 1, 3}, 3);
    /* the buffer's memoryTypeBits leave the window out: never listed */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0x7, 4 * MB, &p, 0, out);
    expect("4 MB upload, window not allowed", out, n, (const uint32_t[]){2, 1}, 2);
    /* device memory: VRAM, the window (device-local too), then the rest */
    n = vkr_memory_candidates(RHI_MEM_DEVICE, 0xF, 64 * MB, &p, 0, out);
    expect("device", out, n, (const uint32_t[]){0, 3, 1, 2}, 4);
    /* readback: cached first */
    n = vkr_memory_candidates(RHI_MEM_READBACK, 0xF, 4 * MB, &p, 0, out);
    expect("readback", out, n, (const uint32_t[]){2, 1, 3}, 3);
    if (!vkr_memory_small_window(&p, 3) || vkr_memory_small_window(&p, 0) ||
        vkr_memory_small_window(&p, 1) || vkr_memory_small_window(&p, 9)) {
        printf("FAIL small window: only type 3 is one\n");
        s_failures++;
    }
}

/* resizable BAR: the window is all of VRAM, every upload's second choice
   after cached system memory, whatever its size */
static void resizableBar(void)
{
    const VkPhysicalDeviceMemoryProperties p = nvidiaNoRebar(8018 * MB);
    uint32_t out[VK_MAX_MEMORY_TYPES], n;

    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0xF, 64 * MB, &p, 0, out);
    expect("64 MB upload, resizable BAR", out, n, (const uint32_t[]){2, 3, 1}, 3);
    if (vkr_memory_small_window(&p, 3)) {
        printf("FAIL resizable BAR: not a small window\n");
        s_failures++;
    }
}

/* one heap, every type device-local and host-visible (an integrated GPU,
   lavapipe): the order of the first rule, no system-memory type */
static void unified(void)
{
    VkPhysicalDeviceMemoryProperties p;
    uint32_t out[VK_MAX_MEMORY_TYPES], n;

    memset(&p, 0, sizeof(p));
    addHeap(&p, 16384 * MB, true);
    addType(&p, DL | HV | HC | CA, 0);
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0x1, 64 * MB, &p, 0, out);
    expect("unified upload", out, n, (const uint32_t[]){0}, 1);
    n = vkr_memory_candidates(RHI_MEM_DEVICE, 0x1, 64 * MB, &p, 0, out);
    expect("unified device", out, n, (const uint32_t[]){0}, 1);
    n = vkr_memory_candidates(RHI_MEM_READBACK, 0x1, 64 * MB, &p, 0, out);
    expect("unified readback", out, n, (const uint32_t[]){0}, 1);
}

/* a small window that is the only host-visible type (no plain system
   memory type in typeBits): a large upload still gets it, as the last
   resort */
static void onlyWindow(void)
{
    const VkPhysicalDeviceMemoryProperties p = nvidiaNoRebar(256 * MB);
    uint32_t out[VK_MAX_MEMORY_TYPES], n;

    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0x9, 64 * MB, &p, 0, out);
    expect("64 MB upload, only the window", out, n, (const uint32_t[]){3}, 1);
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0x9, 64 * MB, &p, 1u << 3, out);
    expect("64 MB upload, only the window, full", out, n, (const uint32_t[]){3}, 1);
    /* no host-visible type at all: nothing */
    n = vkr_memory_candidates(RHI_MEM_UPLOAD, 0x1, 4 * MB, &p, 0, out);
    expect("upload, no host-visible type", out, n, NULL, 0);
}

int main(void)
{
    smallWindow();
    resizableBar();
    unified();
    onlyWindow();
    if (s_failures) {
        printf("rhi_vk_memory: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("rhi_vk_memory: ok\n");
    return 0;
}
