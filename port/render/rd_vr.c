/* rd_vr.c: the VR fork's renderer side (rd_vr.h). */
#include "rd_vr.h"
#include "rd_internal.h"
#include "ico_thread.h"
#include "../rhi/rhi_vr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct {
    bool on;
    uint32_t eyeW, eyeH;
    float unitsPerMetre;
    /* the game thread's next views, handed over with the post */
    IcoMutex *lock;
    RdVrFrame pending;
    /* the clip projection (+0x1C0), the game thread's */
    float clipProj[16];
    bool haveClip;
    /* the render thread's picture */
    RdVrFrame cur;
    /* the output ring */
    RhiTexture ring[3];
    RhiState ringState[3];
    uint32_t ringNext;
    /* recorded pictures waiting for their submit (render thread pushes,
     * submit thread pops, in order) */
    struct {
        RdVrFrame f;
        RhiTexture out;
    } queue[8];
    uint32_t qHead, qTail;
    RdVrPublishFn publish;
    void *publishUser;
} s_vr;

static void vrLock(void)
{
    if (!s_vr.lock) {
        s_vr.lock = ico_mutex_create();
    }
    ico_mutex_lock(s_vr.lock);
}

static void vrUnlock(void)
{
    ico_mutex_unlock(s_vr.lock);
}

void rd_vr_configure(bool on, uint32_t eyeW, uint32_t eyeH, float unitsPerMetre)
{
    s_vr.on = on;
    s_vr.eyeW = eyeW;
    s_vr.eyeH = eyeH;
    s_vr.unitsPerMetre = unitsPerMetre > 0.0f ? unitsPerMetre : 100.0f;
    if (!s_vr.lock) {
        s_vr.lock = ico_mutex_create();
    }
    rd__log("VR: %s, eyes %ux%u, %.1f game units a metre", on ? "on" : "off", eyeW, eyeH,
            (double)s_vr.unitsPerMetre);
}

bool rd_vr_active(void)
{
    return s_vr.on;
}

int rd_vr_wide_cull(void)
{
    return s_vr.on ? 1 : 0;
}

void rd_vr_eye_size(uint32_t *w, uint32_t *h)
{
    *w = s_vr.eyeW;
    *h = s_vr.eyeH;
}

void rd_vr_post(const RdVrFrame *f)
{
    vrLock();
    s_vr.pending = *f;
    vrUnlock();
}

void rd_vr_set_clip_projection(const float proj[16])
{
    vrLock();
    memcpy(s_vr.clipProj, proj, sizeof(s_vr.clipProj));
    s_vr.haveClip = true;
    vrUnlock();
}

void rd_vr_set_publish(RdVrPublishFn fn, void *user)
{
    s_vr.publish = fn;
    s_vr.publishUser = user;
}

void rd__vr_hand_over(RdVrFrame *out)
{
    vrLock();
    *out = s_vr.pending;
    s_vr.pending.valid = 0; /* taken: a further present of the same frame has none */
    vrUnlock();
}

/* the picture is flat: a movie, or the frame has no camera or draws no
   mesh, skinned or grid in the world's lists (0 .. 10) */
static bool pictureFlat(void)
{
    if (g_rd.videoShown) {
        return true;
    }
    const RdFrame *fr = rd__last_frame();
    if (!fr || !fr->hasCamera) {
        return true;
    }
    for (int l = 0; l <= 10 && l < RD_LIST_COUNT; l++) {
        const RdCmdList *cl = &fr->lists[l];
        for (uint32_t i = 0; i < cl->count; i++) {
            const uint8_t t = cl->cmds[i].type;
            if (t == RDC_MESH || t == RDC_SKINNED || t == RDC_GRID) {
                return false;
            }
        }
    }
    return true;
}

void rd__vr_picture_begin(const RdVrFrame *f)
{
    s_vr.cur = *f;
    s_vr.cur.flat = s_vr.on && pictureFlat() ? 1u : 0u;
    if (s_vr.cur.flat) {
        RhiRect b;
        rd__present_box(s_vr.eyeW, s_vr.eyeH, 4.0f / 3.0f, &b);
        s_vr.cur.box[0] = (uint32_t)b.x;
        s_vr.cur.box[1] = (uint32_t)b.y;
        s_vr.cur.box[2] = b.w;
        s_vr.cur.box[3] = b.h;
    }
}

bool rd__vr_flat_box(uint32_t outW, uint32_t outH, RhiRect *box)
{
    if (!s_vr.on || !s_vr.cur.flat) {
        return false;
    }
    rd__present_box(outW, outH, 4.0f / 3.0f, box);
    return true;
}

/* ------------------------------------------------------------ the maths
 * Column-major 4x4 (element [column * 4 + row]), column vectors, as
 * RdCamera and FrameCB hold them. */
static void mul4(const double *a, const double *b, double *o)
{
    double t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) {
                s += a[k * 4 + r] * b[c * 4 + k];
            }
            t[c * 4 + r] = s;
        }
    }
    memcpy(o, t, sizeof(t));
}

static bool inv4(const double *m, double *o)
{
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30) || !isfinite(det)) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        o[i] = inv[i] / det;
    }
    return true;
}

static void ident(double *m)
{
    memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1.0;
}

static void toFloat(const double *d, float *f)
{
    for (int i = 0; i < 16; i++) {
        f[i] = (float)d[i];
    }
}

/* E(v): the camera's view space (x right, y down, z forward, game units) to
 * eye v's.  OpenXR's eye pose (orientation q, position t in the seat's
 * space: x right, y up, z back, metres); C = diag(1, -1, -1) between the two
 * axis sets, s the units a metre: E = [C R^T C | -s C R^T t] */
static void eyeFromCamera(const float *pose, double s, double *e)
{
    const double x = pose[0], y = pose[1], z = pose[2], w = pose[3];
    /* R: the eye's axes in the seat's space, columns */
    const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z),     2 * (x * z - w * y),
                         2 * (x * y - w * z),     1 - 2 * (x * x + z * z), 2 * (y * z + w * x),
                         2 * (x * z + w * y),     2 * (y * z - w * x),     1 - 2 * (x * x + y * y)};
    /* r[c * 3 + k]: column c, row k.  R^T[i][j] = R[j][i] = r[i * 3 + j] */
    const double cs[3] = {1.0, -1.0, -1.0};
    ident(e);
    for (int i = 0; i < 3; i++) {     /* row */
        for (int j = 0; j < 3; j++) { /* column */
            e[j * 4 + i] = cs[i] * r[i * 3 + j] * cs[j];
        }
    }
    const double t[3] = {pose[4], pose[5], pose[6]};
    for (int i = 0; i < 3; i++) {
        double rt = 0.0;
        for (int k = 0; k < 3; k++) {
            rt += r[i * 3 + k] * t[k]; /* (R^T t)_i */
        }
        e[12 + i] = -s * cs[i] * rt;
    }
}

/* S_eye: the eye's frustum (tangents l, r of x / z, u, d of up / down)
 * onto the 4:3 native frame (hw x hh GS pixels each side of 2048), the
 * camera's depth row (Z = zf + zn z, W = z: S[10] zn, S[14] zf) kept */
static void eyeScreen(const float *tan, const double *S, double *o)
{
    const double l = tan[0], r = tan[1], u = tan[2], d = tan[3];
    const double hw = 256.0, hh = (double)(g_rd.gsH ? g_rd.gsH : 448u) * 0.5;
    memset(o, 0, 16 * sizeof(double));
    o[0] = 2.0 * hw / (r - l);
    o[5] = 2.0 * hh / (u - d);
    o[8] = 2048.0 - hw * (r + l) / (r - l);
    o[9] = 2048.0 + hh * (u + d) / (u - d);
    o[10] = S[10];
    o[11] = 1.0;
    o[14] = S[14];
}

void rd__vr_fill_frame_cb(IcoFrameCB *cb, const RdCamera *cam, bool world)
{
    double id[16];
    ident(id);
    for (int v = 0; v < 2; v++) {
        toFloat(id, cb->vrScreen[v]);
        toFloat(id, cb->vrClip[v]);
        toFloat(id, cb->vrHud[v]);
    }
    cb->vrMode[0] = cb->vrMode[1] = cb->vrMode[2] = cb->vrMode[3] = 0.0f;
    {
        /* development: ICO_VR_TEST_TINT=1 tints the right eye in the
           present's blit (blit.hlsl), a check of the view index */
        static int s_tint = -1;
        if (s_tint < 0) {
            const char *e = getenv("ICO_VR_TEST_TINT");
            s_tint = e && e[0] == '1';
        }
        cb->vrMode[3] = s_tint ? 1.0f : 0.0f;
    }
    const RdVrFrame *f = &s_vr.cur;
    if (!s_vr.on || !world || !cam || !f->valid || f->flat) {
        return;
    }
    double S[16], Si[16];
    for (int i = 0; i < 16; i++) {
        S[i] = cam->proj[i];
    }
    if (!inv4(S, Si)) {
        return;
    }
    double G[16], Gi[16];
    bool clip = false;
    {
        double P[16];
        vrLock();
        const bool have = s_vr.haveClip;
        for (int i = 0; i < 16; i++) {
            P[i] = s_vr.clipProj[i];
        }
        vrUnlock();
        /* G = P S_view^-1 restricted to the screen: P maps the view to
         * clip, S the view to screen, so G = P S^-1 */
        if (have) {
            mul4(P, Si, G);
            clip = inv4(G, Gi);
        }
    }
    for (int v = 0; v < 2; v++) {
        double E[16], Se[16], D[16];
        eyeFromCamera(f->eye[v], (double)s_vr.unitsPerMetre, E);
        {
            /* development: ICO_VR_TEST_EYE1=<units> moves eye 1 that far
               right (a check that the eyes differ) */
            static double s_test = -1.0;
            if (s_test < 0.0) {
                const char *e = getenv("ICO_VR_TEST_EYE1");
                s_test = e ? atof(e) : 0.0;
            }
            if (v == 1 && s_test > 0.0) {
                E[12] -= s_test;
            }
        }
        eyeScreen(f->tan[v], S, Se);
        {
            /* development: ICO_VR_TEST_GAMEFOV=1 draws both eyes from the
               camera itself through its own picture (no head, no eye
               distance), the flat game's picture in each eye: D the
               identity up to rounding (a check of everything but the
               eyes' own transform) */
            static int s_gameFov = -1;
            if (s_gameFov < 0) {
                const char *e = getenv("ICO_VR_TEST_GAMEFOV");
                s_gameFov = e && e[0] == '1';
            }
            if (s_gameFov) {
                ident(E);
                memcpy(Se, S, sizeof(Se));
            }
        }
        mul4(Se, E, D);
        mul4(D, Si, D);
        toFloat(D, cb->vrScreen[v]);
        if (v == 0) {
            /* the particles' extent (vu_particle.hlsl): the eye's scale over
             * the camera's, across and down */
            cb->vrMode[1] = S[0] != 0.0 ? (float)(Se[0] / S[0]) : 1.0f;
            cb->vrMode[2] = S[5] != 0.0 ? (float)(Se[5] / S[5]) : 1.0f;
        }
        if (clip) {
            double Dc[16];
            mul4(G, D, Dc);
            mul4(Dc, Gi, Dc);
            toFloat(Dc, cb->vrClip[v]);
        }
    }
    cb->vrMode[0] = 1.0f;
}

uint32_t rd__vr_screen_kind(const void *cmd)
{
    const RdCmd *c = cmd;
    if (!s_vr.on || !c) {
        return 0;
    }
    if (c->type == RDC_SCREEN) {
        /* the interpolation's test (rd_interp.c frameProjected) but for
           the sprites: a sprite the camera projected is a billboard at one
           depth (its corners expanded on the CPU with one Z, rd_replay.c
           expand), whose corners the eye's transform places on its plane */
        return c->b[4] == RD_SCREEN_FRAME_CAMERA && c->b[1] == RD_SPACE_WORLD && c->b[3] == 0
                   ? 1u
                   : 0u;
    }
    if (c->type == RDC_SHADOW_STRIP) {
        /* a shadow volume the frame camera projected (rd_interp.c
           frameProjected's test) */
        return c->b[0] == RD_SHADOW_TRIS && c->b[1] == RD_SHADOW_FRAME_CAMERA ? 1u : 0u;
    }
    return 0;
}

/* ------------------------------------------------------- the output ring */
static uint32_t s_ringCur;

RhiTexture rd__vr_output(RhiTextureDesc *desc, RhiState *state)
{
    const uint32_t i = s_vr.ringNext;
    s_vr.ringNext = (i + 1u) % 3u;
    if (!s_vr.ring[i].id) {
        s_vr.ring[i] = rhi_create_texture(desc);
        s_vr.ringState[i] = RHI_STATE_UNDEFINED;
    }
    s_ringCur = i;
    *state = s_vr.ringState[i];
    return s_vr.ring[i];
}

void rd__vr_recorded(RhiTexture out, RhiState state)
{
    if (s_vr.ring[s_ringCur].id == out.id) {
        s_vr.ringState[s_ringCur] = state;
    }
    vrLock();
    const uint32_t n = (s_vr.qHead + 1u) % 8u;
    if (n != s_vr.qTail) {
        s_vr.queue[s_vr.qHead].f = s_vr.cur;
        s_vr.queue[s_vr.qHead].out = out;
        s_vr.qHead = n;
    }
    vrUnlock();
}

void rd__vr_finished(void)
{
    vrLock();
    if (s_vr.qTail == s_vr.qHead) {
        vrUnlock();
        return;
    }
    const RdVrFrame f = s_vr.queue[s_vr.qTail].f;
    const RhiTexture out = s_vr.queue[s_vr.qTail].out;
    s_vr.qTail = (s_vr.qTail + 1u) % 8u;
    vrUnlock();
    if (s_vr.publish && f.valid) {
        s_vr.publish(&f, out, rhi_vr_submitted(), s_vr.publishUser);
    }
}

void rd__vr_shutdown(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_vr.ring[i].id) {
            rhi_destroy_texture(s_vr.ring[i]);
            s_vr.ring[i] = (RhiTexture){0};
        }
    }
}

bool rd_vr_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    return rd_write_png(path, rgba, w, h, w * 4u, 0);
}
