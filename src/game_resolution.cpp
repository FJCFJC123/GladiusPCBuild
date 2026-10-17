// Game resolution ([video] Resolution = 640x480 | auto | WxH). The engine has 640x480 baked in as immediates and data;
// default720PFIX.XBE showed every site (2026-10-08: its four extra edits over default720P.XBE - the viewport-constant
// floats at 0x1CD407 / 0x1CD40C and the 2D centering at 0x1D5D67 / 0x1D5D83 - are what make the 2D fill the frame).
// Here the same sites get any width / height at startup, before the game runs. 3D needs nothing more: the camera
// (Camera_BuildViewFrustum 0x1AB030) keeps the vertical extent and widens horizontally (Hor+); 2D maps -1..1 onto the
// whole frame. Netplay: the screen size lives in game memory too - statesync.cpp keeps those ranges machine-local.
#include "port.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

UINT g_game_res_w = 640, g_game_res_h = 480;

// 16-bit immediates / int fields (upper half already zero): width (W) or height (H) at the given address
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
// D3D_SetViewportConstants(0, 0, 640.0f, 480.0f, 0, 1.0f) pushes at 0x1CD406 / 0x1CD40B (imm32 floats): the 2D
// mapping (UI shader c93 / c95). The FIX XBE used 728.0 for a 720 frame; the same 728/720 ratio is kept here.
static const DWORD VP_HEIGHT_F = 0x1CD407, VP_WIDTH_F = 0x1CD40C;

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

// --- 2D menu particles (the pink petals on Load School / Arenas, the border around the selected versus arena):
// FX_Render2d (0x851D0) activates s2dCamera (0x3D3050) and runs fx_Render (0x1AF8E0). The 2D UI is authored 4:3 and
// stretched over the frame (fill, center 16:9), so these particles must be the 640x480 render stretched the same
// way: s2dCamera gets the 4:3 projection (Camera_BuildViewFrustum 0x1AB030 is Hor+ - halfH = 0.75 f, halfW = halfH *
// W/H - rebuilt here with halfW = f via FUN_001BC5A0, cdecl l, r, b, t, near, far, out), and sys_aspect (0x4C96CC,
// W/H, which the sprite quad builders 0x1AFEC0 / 0x2007A0 scale corners by) reads 4/3 while fx_Render runs.
// Rebuilt every activation because s2dCamera is .data a netplay sync may overwrite.
typedef void(__fastcall *CameraFn)(void *cam, void *edx);
typedef void(__fastcall *CameraActivateFn)(void *cam, void *edx, int force);
typedef void(__cdecl *ProjectionFn)(float l, float r, float b, float t, float n, float f, void *out);
static void *const S2D_CAMERA = (void *)0x3D3050;
// Every Camera_BuildViewFrustum call goes through here; s2dCamera's result is then redone at 4:3. The full update
// (Camera_Update 0x1ABD90, run by Camera_Activate when +0x258 is clear) builds the frustum first and the combined
// matrices from it after, so they all see the 4:3 projection.
static const DWORD BUILD_FRUSTUM_CALLS[] = {0x1AB538, 0x1ABD9E, 0x1AC73B, 0x1AC8DB};
static void __fastcall build_frustum(void *cam, void *edx)
{
    ((CameraFn)0x1AB030)(cam, edx);                     // Camera_BuildViewFrustum
    if (cam != S2D_CAMERA)
        return;
    BYTE *c = (BYTE *)cam;
    float n = *(float *)(c + 0x274), far_ = *(float *)(c + 0x278);
    float hw = tanf(*(float *)(c + 0xA0) * 0.5f) * n, hh = hw * 0.75f;
    *(float *)(c + 0x26C) = 0.75f;                      // H/W
    *(float *)(c + 0x27C) = hw;
    *(float *)(c + 0x280) = hh;
    ((ProjectionFn)0x1BC5A0)(-hw, hw, -hh, hh, n, far_, c + 0xF0);
}

// FX_Render2d's Camera_Activate(s2dCamera): force the full update every time (s2dCamera is .data a netplay sync may
// overwrite with the host's)
bool d3d_trace_frame();
static void __fastcall s2d_activate(void *cam, void *edx, int force)
{
    if (d3d_trace_frame())
        port_log("order: ---- 2D camera activate\n");
    ((BYTE *)cam)[0x258] = 0;
    ((CameraActivateFn)0x1AC960)(cam, edx, force);      // Camera_Activate
}

// FX::Fire2d (0x86F00) places a 2D emitter from UI pixels (640x448): x = px / 80 - 4, y = ((448 - py) / 56 - 4) * H/W
// with H/W read from 0x4C96D0 by the mulss at 0x087085 - 0.75 at 640x480, matching the 4:3 particle camera. At any
// other aspect the emitter lands off the UI (the flourish around the selected versus arena). The operand is pointed
// at a constant 0.75.
static const float k_fire2d_hw = 0.75f;
static const DWORD FIRE2D_HW_OPERAND = 0x087089;

typedef void(__cdecl *FxRenderFn)(int arg);
bool d3d_trace_frame();
static void __cdecl s2d_fx_render(int arg)
{
    if (d3d_trace_frame())
        port_log("order: ---- 2D fx_render\n");
    float *sys_aspect = (float *)0x4C96CC, saved = *sys_aspect;
    *sys_aspect = 4.0f / 3.0f;
    ((FxRenderFn)0x1AF8E0)(arg);
    *sys_aspect = saved;
}

// --- Movies (SFD): MoviePlay builds its quad with FUN_001D5D40 (thiscall this, verts, w, h; ret 0xC) in 640x480
// screen pixels - centred ((640 - w) / 2, (480 - h) / 2), 640x480 big, texel UVs. Its 640 / 480 (0x1D5D68 / 0x1D5D84,
// the FIX XBE's "2D centering") and the 640 / 480 one caller passes (0x1D61D0 / 0x1D61D5) stay retail; the four
// vertices (stride 0x18 from this+0xEC: x, y) are scaled to the frame afterwards, so the movie fills it the way the
// 2D UI does (and the way the Protius SFD port draws it: a quad over the whole -1..1 2D canvas).
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

// --- Camera viewport (netplay with different resolutions): a camera keeps its viewport rect in pixels at +0x25C
// (x, y, w, h); Camera_Activate (0x1AC960) tail-jumps (0x1AC974) to the camera apply 0x1ABFE0, which hands that rect
// to gfx_SetViewport. Full-screen cameras (+0x286 = 0: frustum from g_ScreenWidth / Height) hold the screen size of
// the machine that built them - after a netplay state sync the HOST's. A client with a smaller screen then drew the
// host's 2560-wide view into its own frame (zoomed, half the view and HUD off-screen, 2026-10-10). Before every
// apply such a camera's rect is reset to this machine's screen.
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

static void install_camera_apply()
{
    BYTE *p = (BYTE *)0x1AC974;
    if (p[0] != 0xE9 || 0x1AC974 + 5 + *(int32_t *)(p + 1) != 0x1ABFE0) {
        port_log("video: camera apply jump at 1AC974 unexpected - viewport fix not installed\n");
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)camera_apply - (0x1AC974 + 5));
}

void port_game_resolution(const char *ini)
{
    install_camera_apply();                             // also at 640x480: a netplay host may run another size
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
    // [video] UiMode = fill (the whole game at the chosen resolution, 2D stretched over it) | center (the game at
    // UiAspect - a pillarbox: 3D, 2D and movies share the narrower frame, the final pass centres it with black bars)
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
    *(float *)VP_HEIGHT_F = (float)h * (728.0f / 720.0f);
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
