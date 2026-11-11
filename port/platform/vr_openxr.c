/* vr_openxr.c: the VR fork's headset (vr_openxr.h). */
#include "vr_openxr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

/* the OpenXR structures are initialised by their type alone ({XR_TYPE_...}) */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../config/config.h"
#include "../game/video_options.h"
#include "../rhi/rhi_vr.h"

extern int g_icoShaderVr; /* port/shaders/embed_shaders.py: the stereo shader variants */

#define VR_LOG(...)                                                                                \
    do {                                                                                           \
        fprintf(stderr, "vr: " __VA_ARGS__);                                                       \
        fputc('\n', stderr);                                                                       \
    } while (0)

/* the OpenXR entry points, through xrGetInstanceProcAddr */
#define VR_XR_FUNCS                                                                                \
    X(CreateInstance)                                                                              \
    X(DestroyInstance)                                                                             \
    X(EnumerateInstanceExtensionProperties)                                                        \
    X(GetSystem)                                                                                   \
    X(GetSystemProperties)                                                                         \
    X(EnumerateViewConfigurationViews)                                                             \
    X(CreateSession)                                                                               \
    X(DestroySession)                                                                              \
    X(BeginSession)                                                                                \
    X(EndSession)                                                                                  \
    X(RequestExitSession)                                                                          \
    X(CreateReferenceSpace)                                                                        \
    X(DestroySpace)                                                                                \
    X(LocateSpace)                                                                                 \
    X(LocateViews)                                                                                 \
    X(EnumerateSwapchainFormats)                                                                   \
    X(CreateSwapchain)                                                                             \
    X(DestroySwapchain)                                                                            \
    X(EnumerateSwapchainImages)                                                                    \
    X(AcquireSwapchainImage)                                                                       \
    X(WaitSwapchainImage)                                                                          \
    X(ReleaseSwapchainImage)                                                                       \
    X(PollEvent)                                                                                   \
    X(WaitFrame)                                                                                   \
    X(BeginFrame)                                                                                  \
    X(EndFrame)

static struct {
    HMODULE loader;
    PFN_xrGetInstanceProcAddr gipa;
#define X(n) PFN_xr##n n;
    VR_XR_FUNCS
#undef X
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR;
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR;
    PFN_xrConvertTimeToWin32PerformanceCounterKHR ConvertTimeToQpc;
    PFN_xrGetDisplayRefreshRateFB GetDisplayRefreshRateFB;

    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local;
    XrSessionState state;
    volatile bool running;
    XrViewConfigurationView views[2];
    uint32_t eyeW, eyeH;
    char instExts[2048], devExts[2048];

    XrSwapchain chain;
    int64_t chainFormat;
    uint32_t chainImages;
    XrSwapchainImageVulkanKHR images[8];

    SDL_Thread *thread;
    volatile bool quit;
    SDL_Mutex *lock;
    SDL_Condition *cond;

    /* the token: the newest headset frame's views (under lock) */
    RdVrFrame token;
    uint64_t tokenAtNs;
    bool tokenTaken;
    uint64_t serial;
    /* each frame's located views as the runtime gave them (the layer's) */
    struct {
        uint64_t id;
        XrView v[2];
    } raw[8];
    /* the published picture (under lock) */
    bool pubReady;
    uint64_t pubId;
    RhiTexture pubTex;
    uint64_t pubTimeline;
    /* the last picture shown: its views stay the layer's while no new one comes */
    bool haveShown;
    XrView shownViews[2];

    /* the seat: the head's yaw and place at the first frame or a recentre */
    bool haveRef;
    volatile int recenter;
    float refYaw;
    XrVector3f refPos;

    /* statistics: a report every 900 headset frames */
    uint32_t frames, newPictures;
    double sumWait, maxWait, sumPic, maxPic, sumEnd, maxEnd;
    double lastEnd;
    double intervals[1024];
    uint32_t nIntervals;
    float refreshHz;
} g;

static bool xrOk(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    VR_LOG("%s failed (%d)", what, (int)r);
    return false;
}

static double nowMs(void)
{
    return (double)SDL_GetTicksNS() / 1e6;
}

/* ------------------------------------------------------------ the loader */
static bool loadLoader(void)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) {
        wcscpy(slash + 1, L"openxr_loader.dll");
        g.loader = LoadLibraryW(exe);
    }
    if (!g.loader) {
        g.loader = LoadLibraryW(L"openxr_loader.dll");
    }
    if (!g.loader) {
        return false;
    }
    g.gipa = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(g.loader, "xrGetInstanceProcAddr");
    return g.gipa != NULL;
}

static void loadFunctions(XrInstance inst)
{
#define X(n) g.gipa(inst, "xr" #n, (PFN_xrVoidFunction *)&g.n);
    VR_XR_FUNCS
#undef X
}

static void loadExt(const char *name, PFN_xrVoidFunction *out)
{
    *out = NULL;
    g.gipa(g.instance, name, out);
}

bool ico_vr_enabled(void)
{
    return g.instance != XR_NULL_HANDLE;
}

bool ico_vr_running(void)
{
    return g.running && g.thread != NULL;
}

/* ------------------------------------------------------------ early init */
bool ico_vr_early_init(void)
{
    if (g.instance != XR_NULL_HANDLE) {
        return true; /* a start-up retry: the instance stays */
    }
    const char *e = getenv("ICO_VR");
    const bool asked = (e && e[0] == '1') || ico_config_get_bool("vr.enabled", 0);
    if (!asked) {
        return false;
    }
    if (!loadLoader()) {
        VR_LOG("no openxr_loader.dll: VR off");
        return false;
    }
    g.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&g.CreateInstance);
    g.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
           (PFN_xrVoidFunction *)&g.EnumerateInstanceExtensionProperties);
    bool haveRefresh = false, haveQpc = false, haveVk = false;
    {
        uint32_t n = 0;
        g.EnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
        XrExtensionProperties *p = calloc(n ? n : 1, sizeof(*p));
        for (uint32_t i = 0; i < n; i++) {
            p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        }
        g.EnumerateInstanceExtensionProperties(NULL, n, &n, p);
        for (uint32_t i = 0; i < n; i++) {
            haveRefresh |= strcmp(p[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0;
            haveQpc |=
                strcmp(p[i].extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
            haveVk |= strcmp(p[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
        }
        free(p);
    }
    if (!haveVk) {
        VR_LOG("the runtime has no XR_KHR_vulkan_enable: VR off");
        return false;
    }
    const char *exts[4];
    uint32_t nExt = 0;
    exts[nExt++] = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    if (haveQpc) {
        exts[nExt++] = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
    }
    if (haveRefresh) {
        exts[nExt++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    }
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName),
             "ICO VR");
    snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "ico-pc");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = nExt;
    ci.enabledExtensionNames = exts;
    if (!xrOk(g.CreateInstance(&ci, &g.instance), "xrCreateInstance")) {
        g.instance = XR_NULL_HANDLE;
        return false;
    }
    loadFunctions(g.instance);
    loadExt("xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&g.GetVulkanInstanceExtensionsKHR);
    loadExt("xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&g.GetVulkanDeviceExtensionsKHR);
    loadExt("xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&g.GetVulkanGraphicsDeviceKHR);
    loadExt("xrGetVulkanGraphicsRequirementsKHR",
            (PFN_xrVoidFunction *)&g.GetVulkanGraphicsRequirementsKHR);
    if (haveQpc) {
        loadExt("xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction *)&g.ConvertTimeToQpc);
    }
    if (haveRefresh) {
        loadExt("xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&g.GetDisplayRefreshRateFB);
    }
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrOk(g.GetSystem(g.instance, &si, &g.system), "xrGetSystem (no headset?)")) {
        g.DestroyInstance(g.instance);
        g.instance = XR_NULL_HANDLE;
        return false;
    }
    {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        g.GetSystemProperties(g.instance, g.system, &sp);
        VR_LOG("headset: %s (vendor %u), max swapchain %ux%u", sp.systemName, sp.vendorId,
               sp.graphicsProperties.maxSwapchainImageWidth,
               sp.graphicsProperties.maxSwapchainImageHeight);
    }
    uint32_t nv = 0;
    g.views[0].type = g.views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!xrOk(g.EnumerateViewConfigurationViews(g.instance, g.system,
                                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv,
                                                g.views),
              "xrEnumerateViewConfigurationViews") ||
        nv != 2) {
        g.DestroyInstance(g.instance);
        g.instance = XR_NULL_HANDLE;
        return false;
    }
    g.eyeW = g.views[0].recommendedImageRectWidth;
    g.eyeH = g.views[0].recommendedImageRectHeight;
    {
        /* ICO_VR_SCALE: the eyes' size over the runtime's recommendation */
        const char *s = getenv("ICO_VR_SCALE");
        const double k = s ? atof(s) : 1.0;
        if (k > 0.1 && k < 4.0) {
            g.eyeW = (uint32_t)(g.eyeW * k + 0.5);
            g.eyeH = (uint32_t)(g.eyeH * k + 0.5);
        }
    }
    {
        uint32_t n = 0;
        g.GetVulkanInstanceExtensionsKHR(g.instance, g.system, 0, &n, NULL);
        if (n < sizeof(g.instExts)) {
            g.GetVulkanInstanceExtensionsKHR(g.instance, g.system, n, &n, g.instExts);
        }
        n = 0;
        g.GetVulkanDeviceExtensionsKHR(g.instance, g.system, 0, &n, NULL);
        if (n < sizeof(g.devExts)) {
            g.GetVulkanDeviceExtensionsKHR(g.instance, g.system, n, &n, g.devExts);
        }
    }
    VR_LOG("eyes %ux%u (recommended %ux%u, max %ux%u); Vulkan instance extensions: %s; device "
           "extensions: %s",
           g.eyeW, g.eyeH, g.views[0].recommendedImageRectWidth,
           g.views[0].recommendedImageRectHeight, g.views[0].maxImageRectWidth,
           g.views[0].maxImageRectHeight, g.instExts, g.devExts);
    RhiVrSetup setup = {true, g.instExts, g.devExts};
    rhi_vr_setup(&setup);
    rd_vr_configure(true, g.eyeW, g.eyeH, 100.0f);
    g_icoShaderVr = 1;
    ico_video_vr_frame(1); /* the 4:3 native frame (rd_vr.h) */
    /* the screen-prim planes are the camera's screen's (rd_screen_planes.c):
       the eyes interpolate the corners instead */
    SDL_setenv_unsafe("ICO_RD_NO_SPRITE_PLANES", "1", 1);
    return true;
}

/* ------------------------------------------------------- the time clocks */
/* an XrTime as SDL_GetTicksNS's clock */
__attribute__((unused)) static uint64_t xrToSdl(XrTime t)
{
    if (!g.ConvertTimeToQpc) {
        return SDL_GetTicksNS();
    }
    LARGE_INTEGER q, now, freq;
    if (!XR_SUCCEEDED(g.ConvertTimeToQpc(g.instance, t, &q))) {
        return SDL_GetTicksNS();
    }
    QueryPerformanceCounter(&now);
    const uint64_t sdlNow = SDL_GetTicksNS();
    QueryPerformanceFrequency(&freq);
    const double d = (double)(q.QuadPart - now.QuadPart) * 1e9 / (double)freq.QuadPart;
    return (uint64_t)((double)sdlNow + d);
}

/* --------------------------------------------------------------- the seat */
static float yawOf(XrQuaternionf o)
{
    /* the heading of the head's forward (-z) about +y */
    const float fx = -2.0f * (o.x * o.z + o.w * o.y), fz = -(1.0f - 2.0f * (o.x * o.x + o.y * o.y));
    return atan2f(-fx, -fz);
}

static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b)
{
    XrQuaternionf r = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return r;
}

/* the views at time t (LOCAL), the seat's frame of them into f (true when
 * the runtime gave both) */
static bool locate(XrTime t, RdVrFrame *f, XrView raw[2])
{
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g.local;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    raw[0].type = raw[1].type = XR_TYPE_VIEW;
    raw[0].next = raw[1].next = NULL;
    if (!XR_SUCCEEDED(g.LocateViews(g.session, &li, &vs, 2, &n, raw)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        return false;
    }
    /* the head: halfway between the eyes, the left eye's turn */
    const XrPosef l = raw[0].pose, r = raw[1].pose;
    const XrVector3f head = {(l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
                             (l.position.z + r.position.z) * 0.5f};
    if (!g.haveRef || g.recenter) {
        g.refYaw = yawOf(l.orientation);
        g.refPos = head;
        g.haveRef = true;
        g.recenter = 0;
        VR_LOG("seat recentred (heading %.1f degrees)", (double)(g.refYaw * 57.2958f));
    }
    const float h = -g.refYaw * 0.5f;
    const XrQuaternionf undo = {0.0f, sinf(h), 0.0f, cosf(h)};
    const float c = cosf(-g.refYaw), s = sinf(-g.refYaw);
    for (int v = 0; v < 2; v++) {
        const XrQuaternionf q = qmul(undo, raw[v].pose.orientation);
        const float dx = raw[v].pose.position.x - g.refPos.x, dy = raw[v].pose.position.y - g.refPos.y,
                    dz = raw[v].pose.position.z - g.refPos.z;
        f->eye[v][0] = q.x;
        f->eye[v][1] = q.y;
        f->eye[v][2] = q.z;
        f->eye[v][3] = q.w;
        f->eye[v][4] = c * dx + s * dz;
        f->eye[v][5] = dy;
        f->eye[v][6] = -s * dx + c * dz;
        f->tan[v][0] = tanf(raw[v].fov.angleLeft);
        f->tan[v][1] = tanf(raw[v].fov.angleRight);
        f->tan[v][2] = tanf(raw[v].fov.angleUp);
        f->tan[v][3] = tanf(raw[v].fov.angleDown);
    }
    f->displayTime = t;
    f->valid = 1;
    return true;
}

void ico_vr_recenter(void)
{
    g.recenter = 1;
}

/* ------------------------------------------------------------- the events */
static void readRefresh(void)
{
    float hz = 0.0f;
    if (g.GetDisplayRefreshRateFB && XR_SUCCEEDED(g.GetDisplayRefreshRateFB(g.session, &hz)) &&
        hz > 10.0f) {
        g.refreshHz = hz;
        VR_LOG("headset refresh rate %.1f Hz", (double)hz);
    }
}

static void pollEvents(void)
{
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (g.PollEvent(g.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *c = (const XrEventDataSessionStateChanged *)&ev;
            g.state = c->state;
            VR_LOG("session state %d", (int)c->state);
            if (c->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g.running = xrOk(g.BeginSession(g.session, &bi), "xrBeginSession");
                readRefresh();
            } else if (c->state == XR_SESSION_STATE_STOPPING) {
                g.EndSession(g.session);
                g.running = false;
            } else if (c->state == XR_SESSION_STATE_EXITING ||
                       c->state == XR_SESSION_STATE_LOSS_PENDING) {
                g.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            g.recenter = 1; /* the runtime's seated reset: the seat follows */
        } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            readRefresh();
        }
        ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
}

/* ---------------------------------------------------- the renderer's side */
static void publish(const RdVrFrame *f, RhiTexture eyes, uint64_t timeline, void *user)
{
    (void)user;
    SDL_LockMutex(g.lock);
    g.pubReady = true;
    g.pubId = f->id;
    g.pubTex = eyes;
    g.pubTimeline = timeline;
    SDL_BroadcastCondition(g.cond);
    SDL_UnlockMutex(g.lock);
}

bool ico_vr_take_frame(RdVrFrame *f, uint64_t *atNs, uint64_t deadlineNs)
{
    if (!ico_vr_running()) {
        return false;
    }
    SDL_LockMutex(g.lock);
    for (;;) {
        if (!g.tokenTaken && g.token.valid) {
            *f = g.token;
            *atNs = g.tokenAtNs;
            g.tokenTaken = true;
            SDL_UnlockMutex(g.lock);
            return true;
        }
        const uint64_t now = SDL_GetTicksNS();
        if (now >= deadlineNs || g.quit || !g.running) {
            SDL_UnlockMutex(g.lock);
            return false;
        }
        const uint64_t ms = (deadlineNs - now + 999999u) / 1000000u;
        SDL_WaitConditionTimeout(g.cond, g.lock, (Sint32)(ms ? ms : 1));
    }
}

/* ------------------------------------------------------- the swapchain */
static bool makeSwapchain(void)
{
    uint32_t n = 0;
    g.EnumerateSwapchainFormats(g.session, 0, &n, NULL);
    int64_t fmts[64];
    n = n < 64u ? n : 64u;
    g.EnumerateSwapchainFormats(g.session, n, &n, fmts);
    /* RGBA8: the picture's bytes copied as they are; sRGB first (the
       picture's values are display values) */
    const int64_t want[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    g.chainFormat = 0;
    for (int w = 0; w < 2 && !g.chainFormat; w++) {
        for (uint32_t i = 0; i < n; i++) {
            if (fmts[i] == want[w]) {
                g.chainFormat = want[w];
                break;
            }
        }
    }
    if (!g.chainFormat) {
        VR_LOG("the runtime offers no RGBA8 swapchain format");
        return false;
    }
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g.chainFormat;
    ci.sampleCount = 1;
    ci.width = g.eyeW;
    ci.height = g.eyeH;
    ci.faceCount = 1;
    ci.arraySize = 2;
    ci.mipCount = 1;
    if (!xrOk(g.CreateSwapchain(g.session, &ci, &g.chain), "xrCreateSwapchain")) {
        return false;
    }
    uint32_t ni = 0;
    g.EnumerateSwapchainImages(g.chain, 0, &ni, NULL);
    ni = ni < 8u ? ni : 8u;
    for (uint32_t i = 0; i < ni; i++) {
        g.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        g.images[i].next = NULL;
    }
    if (!xrOk(g.EnumerateSwapchainImages(g.chain, ni, &ni,
                                         (XrSwapchainImageBaseHeader *)g.images),
              "xrEnumerateSwapchainImages")) {
        return false;
    }
    g.chainImages = ni;
    VR_LOG("swapchain %ux%u x2 layers, format %lld (%s), %u images", g.eyeW, g.eyeH,
           (long long)g.chainFormat, g.chainFormat == VK_FORMAT_R8G8B8A8_SRGB ? "sRGB" : "UNORM", ni);
    return true;
}

/* the picture p into the swapchain: acquired, filled on the headset's queue,
   released */
static bool showPicture(RhiTexture tex, uint64_t timeline)
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xrOk(g.AcquireSwapchainImage(g.chain, &ai, &idx), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    g.WaitSwapchainImage(g.chain, &wi);
    const bool ok = idx < g.chainImages &&
                    rhi_vr_copy_eyes(tex, (uint64_t)(uintptr_t)g.images[idx].image,
                                     (uint32_t)g.chainFormat, g.eyeW, g.eyeH, timeline);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g.ReleaseSwapchainImage(g.chain, &ri);
    return ok;
}

static void report(double period)
{
    if (g.nIntervals < 2) {
        return;
    }
    double sum = 0.0, sorted[1024];
    uint32_t late = 0;
    for (uint32_t i = 0; i < g.nIntervals; i++) {
        sum += g.intervals[i];
        sorted[i] = g.intervals[i];
        late += g.intervals[i] > 1.5 * period;
    }
    for (uint32_t i = 1; i < g.nIntervals; i++) {
        const double v = sorted[i];
        uint32_t j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    const double p99 = sorted[g.nIntervals * 99 / 100];
    const double n = (double)g.frames;
    VR_LOG("headset %.1f FPS (display %.1f Hz), 1%% low %.1f FPS, late %.2f%%, new pictures %.1f%%; "
           "loop ms avg/max: wait frame %.2f/%.1f, picture %.2f/%.1f, end %.2f/%.1f",
           1000.0 * g.nIntervals / sum, 1000.0 / period, 1000.0 / p99,
           100.0 * late / g.nIntervals, 100.0 * g.newPictures / n, g.sumWait / n, g.maxWait,
           g.sumPic / n, g.maxPic, g.sumEnd / n, g.maxEnd);
    g.frames = g.newPictures = 0;
    g.sumWait = g.maxWait = g.sumPic = g.maxPic = g.sumEnd = g.maxEnd = 0.0;
    g.nIntervals = 0;
}

static int frameLoop(void *arg)
{
    (void)arg;
    while (!g.quit) {
        pollEvents();
        if (!g.running) {
            SDL_Delay(10);
            continue;
        }
        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
        double t0 = nowMs();
        if (!xrOk(g.WaitFrame(g.session, &fwi, &fs), "xrWaitFrame")) {
            SDL_Delay(10);
            continue;
        }
        double t1 = nowMs();
        XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
        if (!xrOk(g.BeginFrame(g.session, &fbi), "xrBeginFrame")) {
            continue;
        }
        RdVrFrame f;
        memset(&f, 0, sizeof(f));
        XrView raw[2];
        const bool views = fs.shouldRender && locate(fs.predictedDisplayTime, &f, raw);
        uint64_t id = 0;
        if (views) {
            SDL_LockMutex(g.lock);
            id = ++g.serial;
            f.id = id;
            g.token = f;
            g.tokenAtNs = SDL_GetTicksNS();
            g.tokenTaken = false;
            g.raw[id % 8].id = id;
            g.raw[id % 8].v[0] = raw[0];
            g.raw[id % 8].v[1] = raw[1];
            SDL_BroadcastCondition(g.cond);
            SDL_UnlockMutex(g.lock);
        }
        /* the picture drawn with these views: up to two and a half display
           periods (a slow picture makes this frame late, which the runtime
           sees and fills; a loading screen's missing one keeps the last) */
        bool fresh = false;
        RhiTexture tex = {0};
        uint64_t timeline = 0;
        if (views) {
            const double period = fs.predictedDisplayPeriod > 0
                                      ? (double)fs.predictedDisplayPeriod / 1e6
                                      : 11.1;
            const double until = nowMs() + 2.5 * period;
            SDL_LockMutex(g.lock);
            while (!g.quit && !(g.pubReady && g.pubId == id)) {
                const double left = until - nowMs();
                if (left <= 0.0) {
                    break;
                }
                SDL_WaitConditionTimeout(g.cond, g.lock, (Sint32)(left + 1.0));
            }
            if (g.pubReady && g.pubId == id) {
                fresh = true;
                tex = g.pubTex;
                timeline = g.pubTimeline;
                g.pubReady = false;
            }
            SDL_UnlockMutex(g.lock);
        }
        double t2 = nowMs();
        if (fresh && showPicture(tex, timeline)) {
            g.shownViews[0] = g.raw[id % 8].v[0];
            g.shownViews[1] = g.raw[id % 8].v[1];
            g.haveShown = true;
            g.newPictures++;
        }
        XrCompositionLayerProjectionView pv[2];
        XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
        XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if (fs.shouldRender && g.haveShown) {
            for (int v = 0; v < 2; v++) {
                pv[v] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[v].pose = g.shownViews[v].pose;
                pv[v].fov = g.shownViews[v].fov;
                pv[v].subImage.swapchain = g.chain;
                pv[v].subImage.imageRect.offset.x = 0;
                pv[v].subImage.imageRect.offset.y = 0;
                pv[v].subImage.imageRect.extent.width = (int32_t)g.eyeW;
                pv[v].subImage.imageRect.extent.height = (int32_t)g.eyeH;
                pv[v].subImage.imageArrayIndex = (uint32_t)v;
            }
            layer.space = g.local;
            layer.viewCount = 2;
            layer.views = pv;
            fei.layerCount = 1;
            fei.layers = layers;
        }
        xrOk(g.EndFrame(g.session, &fei), "xrEndFrame");
        double t3 = nowMs();
        /* statistics */
        g.frames++;
        g.sumWait += t1 - t0;
        g.maxWait = g.maxWait > t1 - t0 ? g.maxWait : t1 - t0;
        g.sumPic += t2 - t1;
        g.maxPic = g.maxPic > t2 - t1 ? g.maxPic : t2 - t1;
        g.sumEnd += t3 - t2;
        g.maxEnd = g.maxEnd > t3 - t2 ? g.maxEnd : t3 - t2;
        if (g.lastEnd > 0.0 && g.nIntervals < 1024) {
            g.intervals[g.nIntervals++] = t3 - g.lastEnd;
        }
        g.lastEnd = t3;
        if (g.frames >= 900) {
            report(fs.predictedDisplayPeriod > 0 ? (double)fs.predictedDisplayPeriod / 1e6 : 11.1);
        }
    }
    return 0;
}

/* --------------------------------------------------------------- start */
bool ico_vr_start(void)
{
    if (!ico_vr_enabled()) {
        return false;
    }
    if (!rhi_vr_stereo()) {
        VR_LOG("the renderer has no stereo mode: VR off");
        return false;
    }
    RhiVrDevice d;
    if (!rhi_vr_device(&d)) {
        return false;
    }
    {
        XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        g.GetVulkanGraphicsRequirementsKHR(g.instance, g.system, &req);
        VkPhysicalDevice want = VK_NULL_HANDLE;
        g.GetVulkanGraphicsDeviceKHR(g.instance, g.system, (VkInstance)d.instance, &want);
        if (want != (VkPhysicalDevice)d.physicalDevice) {
            VR_LOG("the runtime asks for another GPU than the renderer's: the session may fail");
        }
    }
    XrGraphicsBindingVulkanKHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = (VkInstance)d.instance;
    gb.physicalDevice = (VkPhysicalDevice)d.physicalDevice;
    gb.device = (VkDevice)d.device;
    gb.queueFamilyIndex = d.queueFamily;
    gb.queueIndex = d.queueIndex;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g.system;
    if (!xrOk(g.CreateSession(g.instance, &sci, &g.session), "xrCreateSession")) {
        return false;
    }
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xrOk(g.CreateReferenceSpace(g.session, &rs, &g.local), "xrCreateReferenceSpace")) {
        return false;
    }
    if (!makeSwapchain()) {
        return false;
    }
    g.lock = SDL_CreateMutex();
    g.cond = SDL_CreateCondition();
    rd_vr_set_publish(publish, NULL);
    g.quit = false;
    g.thread = SDL_CreateThread(frameLoop, "vr headset", NULL);
    VR_LOG("session made (queue family %u, queue %u); the headset's thread started",
           d.queueFamily, d.queueIndex);
    /* the session's READY comes through the events: wait a little for it so
       the first frames are the headset's */
    for (int i = 0; i < 200 && !g.running; i++) {
        SDL_Delay(5);
    }
    return true;
}

void ico_vr_stop(void)
{
    if (!g.thread) {
        return;
    }
    g.quit = true;
    SDL_LockMutex(g.lock);
    SDL_BroadcastCondition(g.cond);
    SDL_UnlockMutex(g.lock);
    SDL_WaitThread(g.thread, NULL);
    g.thread = NULL;
    if (g.session && g.running) {
        g.RequestExitSession(g.session);
    }
    if (g.chain) {
        g.DestroySwapchain(g.chain);
    }
    if (g.local) {
        g.DestroySpace(g.local);
    }
    if (g.session) {
        g.DestroySession(g.session);
    }
    g.running = false;
    VR_LOG("stopped");
}
