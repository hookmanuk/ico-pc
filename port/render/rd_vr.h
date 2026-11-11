/* rd_vr.h: the VR fork's renderer side.
 *
 * Under the stereo mode (port/rhi/rhi_vr.h) every pass draws both eyes at
 * once (multiview).  The game's frame is replayed once a headset frame, as a
 * present between ticks is (rd_present), from the camera the interpolation
 * blends (rd_interp.c); each eye is that camera turned and moved by the
 * head, through the headset's own field of view.  The eyes differ only in
 * the per-view matrices of FrameCB (shader_consts.h IcoFrameCB.vr*), which
 * the shaders pick by the view index:
 *   vrScreen[v]  the camera's GS screen (homogeneous, as the VU programs'
 *                h = screen matrix x position) to eye v's GS screen, both
 *                on the 4:3 native frame (512 x 448 GS pixels about 2048):
 *                D_v = S_eye(v) x E(v) x S^-1, S the camera's screen matrix
 *                (RdCamera.proj), E(v) the camera's view space to eye v's
 *                (the head pose from the recentred seat, 100 game units a
 *                metre), S_eye(v) the eye's frustum (its tangents) onto the
 *                frame with the game's own depth row
 *   vrClip[v]    the same for the VU programs' clip space (rows 20..23):
 *                G D_v G^-1, G the clip projection over the screen matrix
 *                (rd_vr_set_clip_projection)
 *   vrMode.x     1 when the pass draws the frame camera's world (the VU
 *                draws take vrScreen and vrClip; a screen prim the game
 *                projected through the frame camera takes vrScreen when its
 *                DrawCB.vr.x is 1), 0 elsewhere (no transform)
 * The picture of each headset frame is drawn into a two-layer output of the
 * eyes' size (one of a ring of three) and published to the headset's
 * thread once its lists are submitted (rd_vr_set_publish). */
#ifndef PORT_RENDER_RD_VR_H
#define PORT_RENDER_RD_VR_H

#include "rd.h"
#include "rhi.h"
#include "../shaders/shader_consts.h"

/* A headset frame's views, from the headset's thread (port/platform/
 * vr_openxr.c), in the recentred seat's space (OpenXR axes: x right, y up,
 * z back; metres). */
typedef struct RdVrFrame {
    uint64_t id;          /* the headset frame's serial (0: none) */
    int64_t displayTime;  /* XrTime */
    float eye[2][7];      /* per eye: orientation x, y, z, w, position x, y, z */
    float tan[2][4];      /* per eye: tan of the left, right, up and down angles */
    uint8_t valid;
    /* the renderer's: the picture is flat (a movie, a frame without the
     * camera's world: boot signs, the title, menus, loading), drawn in the
     * 4:3 box below and shown on a screen in front of the seat */
    uint8_t flat;
    uint32_t box[4]; /* the flat picture's box in the eyes' image: x, y, w, h */
} RdVrFrame;

/* The headset's output: two-layer RGBA8, eyeW x eyeH. */
void rd_vr_configure(bool on, uint32_t eyeW, uint32_t eyeH, float unitsPerMetre);
bool rd_vr_active(void);
void rd_vr_eye_size(uint32_t *w, uint32_t *h);

/* The game thread, before rd_present_post: the views of the picture it
 * posts next (taken with the post). */
void rd_vr_post(const RdVrFrame *f);

/* rd_vr_set_clip_projection: rd.h (the game's GsBase.c calls it) */

/* A picture's eyes, ready on the GPU once the backend's timeline reaches
 * `timeline` (port/rhi/rhi_vr.h rhi_vr_copy_eyes). */
typedef void (*RdVrPublishFn)(const RdVrFrame *f, RhiTexture eyes, uint64_t timeline,
                              void *user);
void rd_vr_set_publish(RdVrPublishFn fn, void *user);

/* Development: an RGBA8 image written as a PNG (rd_png.c) */
bool rd_vr_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

/* ---- internal (port/render) ---- */
void rd__vr_hand_over(RdVrFrame *out);         /* the game thread, at the post */
void rd__vr_picture_begin(const RdVrFrame *f); /* the render thread, a picture's start */
/* the picture is flat (rd__vr_picture_begin decided it), and its box in the
 * output; false, untouched, for an eye picture */
bool rd__vr_flat_box(uint32_t outW, uint32_t outH, RhiRect *box);
/* FrameCB's per-view matrices: world 1 for a pass of the frame camera's
 * world (cam the picture's camera), else the identity and vrMode.x 0 */
void rd__vr_fill_frame_cb(IcoFrameCB *cb, const RdCamera *cam, bool world);
/* the picture's output: a ring slot of the eyes' size, its state in *state
 * (written back by rd__vr_recorded) */
RhiTexture rd__vr_output(RhiTextureDesc *desc, RhiState *state);
/* the render thread: the picture's output recorded (in RHI_STATE_COPY_SRC);
 * the submit thread, after its lists are submitted: published */
void rd__vr_recorded(RhiTexture out, RhiState state);
void rd__vr_finished(void);
void rd__vr_shutdown(void);
/* the screen command c's kind for DrawCB.vr.x: 1 a prim the frame camera
 * projected (rd_interp.c frameProjected's test), else 0 */
/* 2 for a draw of the flat HUD (list 11 or 12, not the camera's, not over
 * the whole frame, or text), drawn on the screen before the seat
 * (FrameCB.vrHud); frame the RdFrame (its payload), list the draw's */
uint32_t rd__vr_screen_kind(const void *cmd); /* a const RdCmd * */
uint32_t rd__vr_screen_kind_in(const void *cmd, const void *frame, int list);
/* the picture draws its 2D on the HUD (VR on, the eyes' picture, not flat) */
bool rd__vr_hud_on(void);

#endif
