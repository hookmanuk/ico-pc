/* rhi_vr.h: the VR fork's surface beside rhi.h (Vulkan only; every call is
 * "not offered" elsewhere).
 *
 * The stereo mode (rhi_vr_setup before rhi_init, stereo true): every texture
 * the RHI makes has two array layers, one an eye, and every view of it is a
 * 2D array of both; every render pass and pipeline is multiview (view mask
 * 0b11), so one draw draws both eyes and a shader picks its eye's data by
 * the view index (port/shaders/common.hlsli ICO_VR).  Copies copy both
 * layers, an upload writes the same texels into both, a readback reads the
 * left eye's.  The window's swapchain images stay one layer: nothing draws
 * into them under the stereo mode (port/render/rd_vr.c).
 *
 * The OpenXR runtime's Vulkan needs (XR_KHR_vulkan_enable): its instance and
 * device extensions, space-separated, enabled with the backend's own; the
 * device is made with a second queue of the graphics family for the
 * headset's thread (rhi_vr_device: the queue the runtime is given, and the
 * one rhi_vr_copy_eyes submits to), so the runtime never shares the
 * backend's queue. */
#ifndef PORT_RHI_RHI_VR_H
#define PORT_RHI_RHI_VR_H

#include "rhi.h"

typedef struct RhiVrSetup {
    bool stereo;
    const char *instanceExtensions; /* space-separated, may be NULL */
    const char *deviceExtensions;   /* space-separated, may be NULL */
} RhiVrSetup;

/* Before rhi_init; the strings are copied. */
void rhi_vr_setup(const RhiVrSetup *setup);

/* The stereo mode is on (the device was made with it). */
bool rhi_vr_stereo(void);

/* The handles the runtime's session binds (XrGraphicsBindingVulkanKHR). */
typedef struct RhiVrDevice {
    void *instance;
    void *physicalDevice;
    void *device;
    uint32_t queueFamily;
    uint32_t queueIndex; /* the headset's queue */
} RhiVrDevice;

bool rhi_vr_device(RhiVrDevice *out);

/* The backend's submission counter: the value the last submit signals on the
 * backend's timeline (0 without one).  Read after the picture's lists are
 * submitted (port/render/rd_present.c rd__present_finish). */
uint64_t rhi_vr_submitted(void);

/* The headset's thread: both layers of src (a stereo texture in
 * RHI_STATE_COPY_SRC) copied into layers 0 and 1 of dstImage (a VkImage, the
 * runtime's swapchain image of dstFormat, w x h, array size 2), once the
 * backend's timeline reaches `after`; dstImage is left in
 * COLOR_ATTACHMENT_OPTIMAL as the runtime expects.  Submitted on the
 * headset's queue; false when nothing was submitted. */
bool rhi_vr_copy_eyes(RhiTexture src, uint64_t dstImage, uint32_t dstFormat, uint32_t w,
                      uint32_t h, uint64_t after);

/* The headset's thread: waits up to timeoutNs for the backend's timeline to
 * reach value; true when it did. */
bool rhi_vr_wait_value(uint64_t value, uint64_t timeoutNs);

/* Development (the headset thread's eye dump): both layers of src (RGBA8 in
 * RHI_STATE_COPY_SRC) read into left and right (width x height x 4 bytes
 * each) once the timeline reaches `after`; waits for the copy. */
bool rhi_vr_read_eyes(RhiTexture src, uint64_t after, uint8_t *left, uint8_t *right);

/* The VkFormat of an RHI format (the runtime's swapchain format choice). */
uint32_t rhi_vr_vk_format(RhiFormat f);

#endif
