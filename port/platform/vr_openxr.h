/* vr_openxr.h: the VR fork's headset (OpenXR, Windows, Vulkan).
 *
 * On with ICO_VR=1 in the environment or [vr] enabled = true in config.toml,
 * when openxr_loader.dll (beside the program) finds a runtime with a
 * headset.  The headset's loop runs on a thread of its own, as the Uncharted
 * and Burnout VR ports' do: xrWaitFrame, xrBeginFrame, the head's views at
 * the predicted display time handed to the game thread as a token, the
 * picture drawn with them waited for (the renderer publishes it once its
 * lists are submitted: port/render/rd_vr.h), its two layers copied into the
 * runtime's swapchain (one image of two layers) on the headset's own queue,
 * xrEndFrame with the poses the picture was drawn with.  The game thread
 * (port/platform/window_present.c) presents once a token, at the interpolation's
 * alpha for the token's time, so every headset frame is a picture of its own
 * made at the head's pose for it.
 *
 * Order: ico_vr_early_init before rd_init (the runtime's Vulkan extensions,
 * the stereo mode, the eyes' size); ico_vr_start after it (the session, the
 * swapchain, the thread); ico_vr_stop at the end. */
#ifndef PORT_PLATFORM_VR_OPENXR_H
#define PORT_PLATFORM_VR_OPENXR_H

#include <stdbool.h>
#include <stdint.h>

#include "../render/rd_vr.h"

/* Before rd_init: true when VR is asked for and the runtime has a headset
 * (the renderer is then configured for it: rhi_vr_setup, rd_vr_configure). */
bool ico_vr_early_init(void);

/* After rd_init: the session, the swapchain and the headset's thread. */
bool ico_vr_start(void);

void ico_vr_stop(void);

/* VR asked and the instance up (from ico_vr_early_init on). */
bool ico_vr_enabled(void);

/* The session runs and frames flow. */
bool ico_vr_running(void);

/* The game thread: waits until the headset's thread hands a new frame's
 * views or the clock (SDL_GetTicksNS) reaches deadlineNs; true with *f and
 * *atNs (the time the headset's frame began, SDL ticks: the alpha's clock)
 * when a frame came. */
bool ico_vr_take_frame(RdVrFrame *f, uint64_t *atNs, uint64_t deadlineNs);

/* The seat again at the head's place and heading (the next frame's). */
void ico_vr_recenter(void);

#endif
