/* vk_vr.c: the VR fork's backend side (port/rhi/rhi_vr.h): the stereo mode's
 * switch, the OpenXR runtime's extensions, the headset's queue and its copy
 * of the eyes into the runtime's swapchain images.
 *
 * The headset's thread (port/platform/vr_openxr.c) is the only caller of
 * rhi_vr_copy_eyes and rhi_vr_wait_value; it records into command buffers of
 * its own pool and submits to its own queue (index 1 of the graphics
 * family), so nothing here touches the backend's queue, lists or pools. */
#include "vk_internal.h"
#include "../rhi_vr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_stereoAsked;
static char s_instExts[2048];
static char s_devExts[2048];

void rhi_vr_setup(const RhiVrSetup *setup)
{
    s_stereoAsked = setup && setup->stereo;
    snprintf(s_instExts, sizeof(s_instExts), "%s",
             setup && setup->instanceExtensions ? setup->instanceExtensions : "");
    snprintf(s_devExts, sizeof(s_devExts), "%s",
             setup && setup->deviceExtensions ? setup->deviceExtensions : "");
}

bool vkr_vr_stereo_asked(void)
{
    return s_stereoAsked;
}

/* the runtime's extension names (space-separated) appended to list[*n] (at
 * most max), each once */
static void appendNames(const char *text, const char **list, uint32_t *n, uint32_t max,
                        char *store, size_t storeSize)
{
    snprintf(store, storeSize, "%s", text);
    char *p = store;
    while (*p) {
        while (*p == ' ') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }
        char *name = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
        bool have = false;
        for (uint32_t i = 0; i < *n; i++) {
            have = have || strcmp(list[i], name) == 0;
        }
        if (!have && *n < max) {
            list[(*n)++] = name;
        }
    }
}

void vkr_vr_instance_extensions(const char **list, uint32_t *n, uint32_t max)
{
    static char store[2048];
    appendNames(s_instExts, list, n, max, store, sizeof(store));
}

void vkr_vr_device_extensions(const char **list, uint32_t *n, uint32_t max)
{
    static char store[2048];
    appendNames(s_devExts, list, n, max, store, sizeof(store));
}

bool rhi_vr_stereo(void)
{
    return g_vkr.stereo;
}

bool rhi_vr_device(RhiVrDevice *out)
{
    if (!out || !g_vkr.device) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->instance = (void *)g_vkr.instance;
    out->physicalDevice = (void *)g_vkr.phys;
    out->device = (void *)g_vkr.device;
    out->queueFamily = g_vkr.queueFamily;
    out->queueIndex = g_vkr.vrQueueIndex;
    return true;
}

uint64_t rhi_vr_submitted(void)
{
    return __atomic_load_n(&g_vkr.timelineValue, __ATOMIC_ACQUIRE);
}

uint32_t rhi_vr_vk_format(RhiFormat f)
{
    return (uint32_t)vkr_vk_format(f);
}

bool rhi_vr_wait_value(uint64_t value, uint64_t timeoutNs)
{
    if (g_vkr.syncKind != VKR_SYNC_TIMELINE || !g_vkr.waitSemaphores) {
        return true;
    }
    const VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &g_vkr.timeline,
        .pValues = &value,
    };
    return g_vkr.waitSemaphores(g_vkr.device, &wi, timeoutNs) == VK_SUCCESS;
}

/* the headset's command buffers: a ring of three, each with its fence */
#define VR_CMDS 3u
static VkCommandPool s_pool;
static VkCommandBuffer s_cmds[VR_CMDS];
static VkFence s_fences[VR_CMDS];
static uint32_t s_next;

static bool vrCommandsReady(void)
{
    if (s_pool) {
        return true;
    }
    if (!g_vkr.vrQueue) {
        return false;
    }
    const VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_vkr.queueFamily,
    };
    if (vkCreateCommandPool(g_vkr.device, &pci, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE;
        return false;
    }
    const VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VR_CMDS,
    };
    if (vkAllocateCommandBuffers(g_vkr.device, &ai, s_cmds) != VK_SUCCESS) {
        return false;
    }
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                   .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (uint32_t i = 0; i < VR_CMDS; i++) {
        if (vkCreateFence(g_vkr.device, &fci, NULL, &s_fences[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

static void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    const VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2},
    };
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

bool rhi_vr_copy_eyes(RhiTexture src, uint64_t dstImage, uint32_t dstFormat, uint32_t w,
                      uint32_t h, uint64_t after)
{
    (void)dstFormat;
    VkrTexture *s = vkr_get_texture(src);
    if (!s || !dstImage || !vrCommandsReady()) {
        return false;
    }
    const uint32_t i = s_next;
    s_next = (s_next + 1u) % VR_CMDS;
    vkWaitForFences(g_vkr.device, 1, &s_fences[i], VK_TRUE, UINT64_MAX);
    vkResetFences(g_vkr.device, 1, &s_fences[i]);
    vkResetCommandBuffer(s_cmds[i], 0);
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBuffer cb = s_cmds[i];
    vkBeginCommandBuffer(cb, &bi);
    const VkImage dst = (VkImage)(uintptr_t)dstImage;
    imageBarrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);
    const uint32_t cw = w < s->width ? w : s->width, ch = h < s->height ? h : s->height;
    const uint32_t layers = s->layers < 2u ? s->layers : 2u;
    const VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
        .extent = {cw, ch, 1},
    };
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(cb);
    const bool timeline = g_vkr.syncKind == VKR_SYNC_TIMELINE && after != 0;
    const VkTimelineSemaphoreSubmitInfo ti = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &after,
    };
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = timeline ? &ti : NULL,
        .waitSemaphoreCount = timeline ? 1u : 0u,
        .pWaitSemaphores = timeline ? &g_vkr.timeline : NULL,
        .pWaitDstStageMask = timeline ? &stage : NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    if (!timeline && after) {
        rhi_vr_wait_value(after, 100000000ull);
    }
    return vkQueueSubmit(g_vkr.vrQueue, 1, &si, s_fences[i]) == VK_SUCCESS;
}

void vkr_vr_shutdown(void)
{
    if (!s_pool) {
        return;
    }
    if (g_vkr.vrQueue) {
        vkQueueWaitIdle(g_vkr.vrQueue);
    }
    for (uint32_t i = 0; i < VR_CMDS; i++) {
        if (s_fences[i]) {
            vkDestroyFence(g_vkr.device, s_fences[i], NULL);
            s_fences[i] = VK_NULL_HANDLE;
        }
    }
    vkDestroyCommandPool(g_vkr.device, s_pool, NULL);
    s_pool = VK_NULL_HANDLE;
}
