// Game resolution ([video] Resolution = 640x480 | auto | WxH). The engine hard-codes 640x480; every site that holds
// it is rewritten at startup. 3D widens horizontally (Hor+); 2D maps -1..1 onto the whole frame.
#include "d3d.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

UINT g_game_res_w = 640, g_game_res_h = 480;

// 16-bit width (W) / height (H) immediates and fields
static const struct { DWORD addr; WORD retail; char what; const char *note; } INT_SITES[] = {
    {0x06DC7C, 480, 'H', "startup init"},           {0x06DC81, 640, 'W', "startup init"},
    {0x1BD0AA, 640, 'W', "SetScissors default"},    {0x1BD0AF, 480, 'H', "SetScissors default"},
    {0x1CD475, 640, 'W', "back-buffer rect"},       {0x1CD47D, 480, 'H', "back-buffer rect"},
    {0x1CE9A4, 640, 'W', "render init -> sys_SetScreenDimensions"},
    {0x1CE9A9, 480, 'H', "render init -> sys_SetScreenDimensions"},
    {0x1D632B, 640, 'W', "vertical-blank surface"}, {0x1D6332, 480, 'H', "vertical-blank surface"},
    {0x1D6C51, 480, 'H', "movie texture"},          {0x1D6C56, 640, 'W', "movie texture"},
    {0x1D6C9E, 480, 'H', "movie texture"},          {0x1D6CA3, 640, 'W', "movie texture"},
    {0x21B77B, 640, 'W', "video fallback"},         {0x21B785, 480, 'H', "video fallback"},
    {0x37FD38, 640, 'W', "g_ScreenWidth"},          {0x37FD3C, 480, 'H', "g_ScreenHeight"},
    {0x385820, 640, 'W', "mode table"}, {0x385824, 480, 'H', "mode table"},
    {0x38582C, 640, 'W', "mode table"}, {0x385830, 480, 'H', "mode table"},
    {0x385838, 640, 'W', "mode table"}, {0x38583C, 480, 'H', "mode table"},
    {0x385844, 640, 'W', "mode table"}, {0x385848, 480, 'H', "mode table"},
};
// D3D_SetViewportConstants(0, 0, 640.0f, 480.0f, ...) float pushes (end-of-frame full-screen viewport for the 2D layer):
// exactly the screen size, like gfx_SetViewport's own full-screen viewport - any other height shifts the 2D layer.
static const DWORD VP_HEIGHT_F = 0x1CD407, VP_WIDTH_F = 0x1CD40C;

// "auto" (desktop size) or "WxH" -> w, h.
static bool parse_res(const char *s, UINT *w, UINT *h)
{
    if (!_stricmp(s, "auto")) {
        *w = GetSystemMetrics(SM_CXSCREEN);
        *h = GetSystemMetrics(SM_CYSCREEN);
        return *w && *h;
    }
    const char *x = strchr(s, 'x');
    if (!x) x = strchr(s, 'X');
    if (!x) return false;
    *w = (UINT)atoi(s);
    *h = (UINT)atoi(x + 1);
    return *w >= 320 && *h >= 240 && *w <= 8192 && *h <= 8192;
}

// 2D menu particles: FX_Render2d (0x851D0) renders through s2dCamera (0x3D3050). The 2D UI is authored 4:3 and
// stretched, so these particles get a 4:3 projection and sys_aspect (0x4C96CC) reads 4/3 while they render.
typedef void(__fastcall *CameraFn)(void *cam, void *edx);
typedef void(__fastcall *CameraActivateFn)(void *cam, void *edx, int force);
typedef void(__cdecl *ProjectionFn)(float l, float r, float b, float t, float n, float f, void *out);
static void *const S2D_CAMERA = (void *)0x3D3050;
// Every Camera_BuildViewFrustum call goes through here; s2dCamera's projection is then rebuilt at 4:3.
// Preview cameras (+0x286 own viewport, +0x287 width-fixed: the school / shop character views) take their vertical
// extent from the viewport's height / width - a tall UI box at 640x480, a much squarer one once the UI is stretched
// over a wide screen, which zoomed the character in. Their vertical extent is set back to what the 640x480 box gives,
// and the horizontal one widened to the box's real shape (no distortion).
static const DWORD BUILD_FRUSTUM_CALLS[] = {0x1AB538, 0x1ABD9E, 0x1AC73B, 0x1AC8DB};
static void __fastcall build_frustum(void *cam, void *edx)
{
    ((CameraFn)0x1AB030)(cam, edx);                     // Camera_BuildViewFrustum
    BYTE *c = (BYTE *)cam;
    if (cam != S2D_CAMERA && c[0x286] && c[0x287]) {
        const int *rc = (const int *)(c + 0x25C);       // x, y, w, h in screen pixels
        if (rc[2] <= 0 || rc[3] <= 0)
            return;
        float n = *(float *)(c + 0x274), far_ = *(float *)(c + 0x278);
        float t = tanf(*(float *)(c + 0xA0) * 0.5f) * n;
        float r = (rc[3] * 480.0f / g_game_res_h) / (rc[2] * 640.0f / g_game_res_w);   // the box's H/W at 640x480
        float hh = r >= 0.75f ? r * t : 0.75f * t, hw = hh * rc[2] / rc[3];
        *(float *)(c + 0x26C) = (float)rc[3] / rc[2];
        *(float *)(c + 0x27C) = hw;
        *(float *)(c + 0x280) = hh;
        ((ProjectionFn)0x1BC5A0)(-hw, hw, -hh, hh, n, far_, c + 0xF0);
        return;
    }
    if (cam != S2D_CAMERA)
        return;
    float n = *(float *)(c + 0x274), far_ = *(float *)(c + 0x278);
    float hw = tanf(*(float *)(c + 0xA0) * 0.5f) * n, hh = hw * 0.75f;
    *(float *)(c + 0x26C) = 0.75f;                      // H/W
    *(float *)(c + 0x27C) = hw;
    *(float *)(c + 0x280) = hh;
    ((ProjectionFn)0x1BC5A0)(-hw, hw, -hh, hh, n, far_, c + 0xF0);
}

// FX_Render2d's Camera_Activate(s2dCamera): force a full update every time (a netplay sync may overwrite it).
bool d3d_trace_frame();
static void __fastcall s2d_activate(void *cam, void *edx, int force)
{
    if (d3d_trace_frame())
        port_log("order: ---- 2D camera activate\n");
    ((BYTE *)cam)[0x258] = 0;
    ((CameraActivateFn)0x1AC960)(cam, edx, force);      // Camera_Activate
}

// FX::Fire2d (0x86F00) places 2D emitters using the screen H/W at 0x4C96D0; point its operand at a constant 0.75 so
// emitters stay on the 4:3 UI.
static const float k_fire2d_hw = 0.75f;
static const DWORD FIRE2D_HW_OPERAND = 0x087089;

typedef void(__cdecl *FxRenderFn)(int arg);
bool d3d_trace_frame();
// fx_Render with sys_aspect at 4/3.
static void __cdecl s2d_fx_render(int arg)
{
    if (d3d_trace_frame())
        port_log("order: ---- 2D fx_render\n");
    float *sys_aspect = (float *)0x4C96CC, saved = *sys_aspect;
    *sys_aspect = 4.0f / 3.0f;
    ((FxRenderFn)0x1AF8E0)(arg);
    *sys_aspect = saved;
}

// Movies: MoviePlay builds a 640x480 quad (0x1D5D40); scale its four vertices to the frame afterwards.
typedef void(__fastcall *MovieQuadFn)(void *self, void *edx, void *verts, int w, int h);
static void __fastcall movie_quad(void *self, void *edx, void *verts, int w, int h)
{
    ((MovieQuadFn)0x1D5D40)(self, edx, verts, w, h);
    float sx = g_game_res_w / 640.0f, sy = g_game_res_h / 480.0f;
    for (int i = 0; i < 4; i++) {
        float *v = (float *)((BYTE *)self + 0xEC + i * 0x18);
        v[0] = (v[0] + 0.5f) * sx - 0.5f;
        v[1] = (v[1] + 0.5f) * sy - 0.5f;
    }
}

// Repoint the E8 call at `site` (expected to call expect_target) to fn.
static bool redirect_call(DWORD site, DWORD expect_target, void *fn, const char *what)
{
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != expect_target) {
        port_log("video: %s - unexpected call at %06X, not hooked\n", what, site);
        return false;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)fn - (site + 5));
    return true;
}

// Camera viewport: full-screen cameras (+0x286 = 0) keep their pixel viewport at +0x25C. After a netplay state sync
// it holds the host's screen size, so reset it to this machine's screen before every apply (0x1ABFE0).
typedef void(__fastcall *CameraApplyFn)(void *cam, void *edx, int arg);
static void __fastcall camera_apply(BYTE *cam, void *edx, int arg)
{
    if (!cam[0x286]) {
        int *r = (int *)(cam + 0x25C);
        int w = *(const int *)0x37FD38, h = *(const int *)0x37FD3C;  // g_ScreenWidth / Height (machine-local)
        if (w > 0 && h > 0 && (r[0] || r[1] || r[2] != w || r[3] != h)) {
            static int logged;
            if (logged++ < 10)
                port_log("video: camera %p viewport %d,%d %dx%d -> this screen %dx%d\n", cam, r[0], r[1], r[2], r[3], w, h);
            r[0] = 0, r[1] = 0, r[2] = w, r[3] = h;
        }
    }
    ((CameraApplyFn)0x1ABFE0)(cam, edx, arg);
}

// Route Camera_Activate's tail jump (0x1AC974) through camera_apply.
static void install_camera_apply()
{
    BYTE *p = (BYTE *)0x1AC974;
    if (p[0] != 0xE9 || 0x1AC974 + 5 + *(int32_t *)(p + 1) != 0x1ABFE0) {
        port_log("video: camera apply jump at 1AC974 unexpected - viewport fix not installed\n");
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)camera_apply - (0x1AC974 + 5));
}

// gfx_SetViewport (0x1BCA50, cdecl x, y, w, h in screen pixels, y from the bottom): a sub-screen viewport (the 3D
// character previews of the school / shop screens) is noted for TAA - outside the box, and its empty background,
// the screen is static 2D, not sky - and each distinct one is logged once.
float g_sub_viewport[4];                                // left, top, right, bottom as 0..1 of the screen
DWORD g_sub_viewport_frame = 0xFFFFFFFF;                // d3d frame it was set in
static void __cdecl note_viewport(const int *a)
{
    static int seen[16][4], n;
    int w = *(const int *)0x37FD38, h = *(const int *)0x37FD3C;
    if (w <= 0 || h <= 0 || (a[0] == 0 && a[1] == 0 && a[2] == w && a[3] == h))
        return;
    g_sub_viewport[0] = (float)a[0] / w, g_sub_viewport[2] = (float)(a[0] + a[2]) / w;
    g_sub_viewport[1] = (float)(h - a[1] - a[3]) / h, g_sub_viewport[3] = (float)(h - a[1]) / h;
    g_sub_viewport_frame = g_d3d.frame;
    if (n >= 16)
        return;
    for (int i = 0; i < n; i++)
        if (!memcmp(seen[i], a, sizeof seen[i]))
            return;
    memcpy(seen[n++], a, sizeof seen[0]);
    port_log("video: viewport %d,%d %dx%d on a %dx%d screen\n", a[0], a[1], a[2], a[3], w, h);
}

static __declspec(naked) void set_viewport_hook()
{
    __asm {
        pushad
        lea eax, [esp + 36]                             // the caller's arguments (above pushad + return address)
        push eax
        call note_viewport
        add esp, 4
        popad
        mov eax, dword ptr ds:[0x37FFF4]                // displaced first instruction
        push 0x1BCA55
        ret
    }
}

static void install_viewport_log()
{
    static const BYTE expect[5] = {0xA1, 0xF4, 0xFF, 0x37, 0x00};   // mov eax, [0x37FFF4]
    BYTE *p = (BYTE *)0x1BCA50;
    if (memcmp(p, expect, 5)) {
        port_log("video: gfx_SetViewport at 1BCA50 unexpected - viewport log off\n");
        return;
    }
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)set_viewport_hook - (0x1BCA50 + 5));
}

// Apply [video] Resolution / UiMode / UiAspect and install the 2D, particle and movie hooks.
void port_game_resolution(const char *ini)
{
    install_camera_apply();                             // also at 640x480: a netplay host may run another size
    install_viewport_log();
    char val[32];
    GetPrivateProfileStringA("video", "Resolution", "640x480", val, sizeof val, ini);
    UINT w, h;
    if (!parse_res(val, &w, &h)) {
        port_log("video: Resolution '%s' not understood - 640x480\n", val);
        return;
    }
    if (w == 640 && h == 480)
        return;
    int bad = 0;
    for (const auto &s : INT_SITES)
        if (*(const WORD *)s.addr != s.retail || *(const WORD *)(s.addr + 2) != 0) {
            port_log("video: resolution site %06X (%s) has unexpected bytes %08X - resolution NOT changed\n", s.addr,
                     s.note, *(const DWORD *)s.addr);
            bad++;
        }
    if (*(const float *)VP_HEIGHT_F != 480.0f || *(const float *)VP_WIDTH_F != 640.0f) {
        port_log("video: viewport-constant floats at %06X / %06X unexpected - resolution NOT changed\n", VP_HEIGHT_F,
                 VP_WIDTH_F);
        bad++;
    }
    if (bad)
        return;
    // UiMode = fill (2D stretched over the frame) | center (pillarbox at UiAspect).
    char mode[16], asp[16];
    GetPrivateProfileStringA("video", "UiMode", "fill", mode, sizeof mode, ini);
    GetPrivateProfileStringA("video", "UiAspect", "4:3", asp, sizeof asp, ini);
    float ratio = !strcmp(asp, "16:9") ? 16.0f / 9 : !strcmp(asp, "21:9") ? 21.0f / 9 : 4.0f / 3;
    if (!_stricmp(mode, "center")) {
        UINT cw = (UINT)(h * ratio + 0.5f);
        if (cw < w) w = cw;
    }
    for (const auto &s : INT_SITES)
        *(WORD *)s.addr = (WORD)(s.what == 'W' ? w : h);
    *(float *)VP_WIDTH_F = (float)w;
    *(float *)VP_HEIGHT_F = (float)h;
    g_game_res_w = w;
    g_game_res_h = h;
    bool fx = redirect_call(0x851DE, 0x1AC960, (void *)s2d_activate, "2D menu particles (FX_Render2d)");
    for (DWORD site : BUILD_FRUSTUM_CALLS)
        fx = fx && redirect_call(site, 0x1AB030, (void *)build_frustum, "2D menu particles (frustum)");
    fx = fx && redirect_call(0x851EF, 0x1AF8E0, (void *)s2d_fx_render, "2D menu particles (fx_Render)");
    if (fx && *(const DWORD *)FIRE2D_HW_OPERAND == 0x4C96D0)
        *(const float **)FIRE2D_HW_OPERAND = &k_fire2d_hw;
    else if (fx)
        port_log("video: FX::Fire2d H/W operand at %06X unexpected - 2D emitters not remapped\n", FIRE2D_HW_OPERAND);
    redirect_call(0x1D61E2, 0x1D5D40, (void *)movie_quad, "movie quad");
    redirect_call(0x1D668E, 0x1D5D40, (void *)movie_quad, "movie quad");
    port_log("video: game resolution %ux%u (%d sites + viewport constants; was 640x480), UiMode %s%s\n", w, h,
             (int)(sizeof INT_SITES / sizeof INT_SITES[0]), mode, fx ? ", 2D particles at 4:3" : "");
}
