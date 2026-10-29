// Xbox D3D8 -> D3D11 shim core: window, device, swap chain, post chain and final pass, push-buffer capture, present.
#include "d3d.h"
#include "d3d_prof.h"
#include "overlay.h"
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <string>

static const char *aa_name() { return g_d3d.anti_alias == 2 ? "taa" : g_d3d.anti_alias ? "fxaa" : "off"; }

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

D3DState g_d3d;

static CRITICAL_SECTION g_d3d_cs;
static INIT_ONCE g_d3d_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_d3d_cs(PINIT_ONCE, PVOID, PVOID *)
{
    InitializeCriticalSectionAndSpinCount(&g_d3d_cs, 2000);
    return TRUE;
}
extern "C" volatile DWORD g_d3d_owner = 0;             // watchdog
static int g_d3d_depth;

// Take the D3D lock. A thread waiting for it gives up the CRI lock meanwhile, so D3D and CRI can't deadlock.
void d3d_lock()
{
    InitOnceExecuteOnce(&g_d3d_once, init_d3d_cs, nullptr, nullptr);
    if (!TryEnterCriticalSection(&g_d3d_cs)) {
        int depth = port_lock_suspend();
        EnterCriticalSection(&g_d3d_cs);
        if (depth) {
            // re-taking the CRI lock while holding the device would invert the order: drop, re-take in order
            LeaveCriticalSection(&g_d3d_cs);
            port_lock_resume(depth);
            EnterCriticalSection(&g_d3d_cs);           // CRI holders waiting here release nothing, but
        }                                              // every other D3D waiter has released CRI
    }
    if (g_d3d_depth++ == 0)
        g_d3d_owner = GetCurrentThreadId();
    prof_lock_enter();
}
void d3d_unlock()
{
    prof_lock_leave();
    if (--g_d3d_depth == 0)
        g_d3d_owner = 0;
    LeaveCriticalSection(&g_d3d_cs);
}

// ---------------------------------------------------------------- window (own thread + message loop)
static HANDLE g_window_ready;
static volatile LONG g_pending_resize;
static volatile LONG g_pending_w, g_pending_h;
static bool g_borderless;
static WINDOWPLACEMENT g_windowed_placement = {sizeof g_windowed_placement};
static DWORD g_windowed_style;
// soft-reboot handoff (reboot.cpp): the new process opens its window hidden where the old one was, shows it on its
// first frame, then signals the old process to exit
static HANDLE g_handoff_ev;
static RECT g_handoff_rect;
static int g_handoff_show;
static bool g_handoff_borderless;
enum { WM_HANDOFF_SHOW = WM_APP + 7 };

void d3d_window_handoff(int l, int t, int r, int b, int show, bool borderless, HANDLE ev)
{
    g_handoff_rect = {l, t, r, b};
    g_handoff_show = show == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    g_handoff_borderless = borderless;
    g_handoff_ev = ev;
}

bool d3d_window_state(int *l, int *t, int *r, int *b, int *show, bool *borderless)
{
    if (!g_d3d.hwnd)
        return false;
    WINDOWPLACEMENT wp = {sizeof wp};
    if (g_borderless)
        wp = g_windowed_placement;                      // the windowed placement F11 will return to
    else
        GetWindowPlacement(g_d3d.hwnd, &wp);
    *l = wp.rcNormalPosition.left, *t = wp.rcNormalPosition.top;
    *r = wp.rcNormalPosition.right, *b = wp.rcNormalPosition.bottom;
    *show = wp.showCmd;
    *borderless = g_borderless;
    return true;
}

static void request_resize(UINT w, UINT h)
{
    if (!w || !h)
        return;
    InterlockedExchange(&g_pending_w, (LONG)w);
    InterlockedExchange(&g_pending_h, (LONG)h);
    InterlockedExchange(&g_pending_resize, 1);
}

static void toggle_borderless(HWND h)
{
    if (!g_borderless) {
        g_windowed_style = GetWindowLongA(h, GWL_STYLE);
        GetWindowPlacement(h, &g_windowed_placement);
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(h, GWL_STYLE, (g_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_borderless = true;
    } else {
        SetWindowLongA(h, GWL_STYLE, g_windowed_style);
        SetWindowPlacement(h, &g_windowed_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        g_borderless = false;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    bool port_devtools_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);   // devtools_host.cpp: console keys
    if (port_devtools_wndproc(h, msg, wp, lp))
        return 0;
    switch (msg) {
    case WM_HANDOFF_SHOW:                               // soft reboot: first frame drawn - take over from the old window
        ShowWindow(h, g_handoff_show);
        if (g_handoff_borderless && !g_borderless)
            toggle_borderless(h);
        SetForegroundWindow(h);
        if (g_handoff_ev) {
            SetEvent(g_handoff_ev);
            CloseHandle(g_handoff_ev);
            g_handoff_ev = nullptr;
        }
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED)
            request_resize(LOWORD(lp), HIWORD(lp));
        break;
    case WM_LBUTTONDOWN:
        if (port_is_editor()) {                         // level editor: pick the prop under the cursor
            RECT rc;
            GetClientRect(h, &rc);
            void port_editor_pick(int x, int y, int w, int hgt);   // editor.cpp
            port_editor_pick((short)LOWORD(lp), (short)HIWORD(lp), rc.right, rc.bottom);
            SetCapture(h);                              // keep the drag when the cursor leaves the window
        }
        break;
    case WM_MOUSEMOVE:
        if (port_is_editor()) {                         // dragging a picked prop / mesh tools
            RECT rc;
            GetClientRect(h, &rc);
            void port_editor_drag(int x, int y, int w, int hgt);   // editor.cpp
            void port_editor_hover(int x, int y, int w, int hgt);
            if (wp & MK_LBUTTON)
                port_editor_drag((short)LOWORD(lp), (short)HIWORD(lp), rc.right, rc.bottom);
            else
                port_editor_hover((short)LOWORD(lp), (short)HIWORD(lp), rc.right, rc.bottom);
        }
        break;
    case WM_LBUTTONUP:
        if (port_is_editor()) {
            void port_editor_release();                 // editor.cpp
            port_editor_release();
            ReleaseCapture();
        }
        break;
    case WM_MOUSEWHEEL:
        if (port_is_editor()) {                         // turn the selected prop
            void port_editor_wheel(int delta);          // editor.cpp
            port_editor_wheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !(lp & 0x40000000)) {   // keyboard-only quit (not a controller button), confirmed
            if (MessageBoxA(h, "Quit Gladius?", "Gladius", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                port_log("quit (Esc)\n");
                ExitProcess(0);
            }
            return 0;
        }
        if (port_is_editor() && (((wp == 'Z' || wp == 'Y') && (GetKeyState(VK_CONTROL) & 0x8000)) ||
                                 (wp >= VK_NUMPAD0 && wp <= VK_DIVIDE))) {
            bool port_editor_key(int vk);               // editor.cpp: mesh undo / redo, numpad placement keys
            if (port_editor_key((int)wp))
                return 0;
        }
        if (wp == 'R' && (GetKeyState(VK_CONTROL) & 0x8000) && !(lp & 0x40000000) && port_is_editor()) {
            void port_editor_reload();                  // editor.cpp: rescan data\ + reload the level
            port_editor_reload();
            return 0;
        }
        if (wp == 'E' && (GetKeyState(VK_CONTROL) & 0x8000) && !(lp & 0x40000000) && port_is_editor()) {
            void port_editor_show();                    // editor.cpp
            port_editor_show();
            return 0;
        }
        if (wp == 'G' && (GetKeyState(VK_CONTROL) & 0x8000) && !(lp & 0x40000000)) {
            void port_godcam_toggle();                  // godcam.cpp: the developers' free camera
            port_godcam_toggle();
            return 0;
        }
        if (wp == '0' && !(lp & 0x40000000)) {         // debug views: scene, AO, AO unblurred, AO noise, depth bend
            static int view;
            view = (view + 1) % 5;
            void d3d_ssao_set_show(int mode);
            d3d_ssao_set_show(view >= 1 && view <= 3 ? view : 0);
            g_d3d.show_depth = view == 4 ? -1.0f : 0.0f;
            port_log("debug view %d (key 0)\n", view);
        }
        if (wp >= '4' && wp <= '9')                     // number row 4..9 = F4..F9 (keyboards whose F keys are
            wp = VK_F4 + (wp - '4');                    // media keys without Fn; the game's keys skip the number row)
        if (wp >= VK_F1 && wp <= VK_F24 && !(lp & 0x40000000))
            port_log("video: key F%u\n", (unsigned)(wp - VK_F1 + 1));
        if (wp == VK_F11)
            toggle_borderless(h);
        else if (wp == VK_F4) {                         // live A/B of the HD texture pack (re-uploads every texture)
            g_d3d.hd_textures = !g_d3d.hd_textures;
            void d3d_textures_all_dirty();
            d3d_textures_all_dirty();
            port_log("video: HD textures %s (F4)\n", g_d3d.hd_textures ? "on" : "off");
        } else if (wp == VK_F5) {
            g_d3d.shadows = !g_d3d.shadows;
            port_log("video: shadows %s (F5)\n", g_d3d.shadows ? "on" : "off");
        } else if (wp == VK_F6) {
            g_d3d.ssao = !g_d3d.ssao;
            port_log("video: SSAO %s (F6)\n", g_d3d.ssao ? "on" : "off");
        } else if (wp == VK_F7) {
            g_d3d.soft_particles = !g_d3d.soft_particles;
            port_log("video: soft particles %s (F7)\n", g_d3d.soft_particles ? "on" : "off");
        } else if (wp == VK_F8) {                       // live A/B of per-pixel lighting
            g_d3d.pixel_lighting = !g_d3d.pixel_lighting;
            port_log("video: lighting per %s (F8)\n", g_d3d.pixel_lighting ? "pixel" : "vertex");
        } else if (wp == VK_F9) {                         // live A/B of the post anti-aliasing
            g_d3d.anti_alias = (g_d3d.anti_alias + 1) % 3;   // off -> FXAA -> TAA
            port_log("video: anti-aliasing %s (F9)\n", aa_name());
        }
        break;
    case WM_CLOSE:
        port_log("window closed - exiting\n");
        ExitProcess(0);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI window_thread(void *)
{
    WNDCLASSEXA wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // the exe's icon (the XBE title image, written by xbe2pe.py); exes converted without one keep the default
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "GladiusPC";
    RegisterClassExA(&wc);
    char ini[MAX_PATH];                                 // [video] WindowWidth / WindowHeight: client size at start
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    extern UINT g_game_res_w, g_game_res_h;             // game_resolution.cpp (set before the window opens)
    int ww = GetPrivateProfileIntA("video", "WindowWidth", 0, ini), wh = GetPrivateProfileIntA("video", "WindowHeight", 0, ini);
    if (ww < 320 || wh < 240) {                         // default: 1280x960 for 640x480, else the game resolution's
        ww = 1280, wh = 960;                            // aspect, as large as fits 90% of the work area
        if (g_game_res_w != 640 || g_game_res_h != 480) {
            RECT wa;
            SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
            double fit = 0.9 * (wa.right - wa.left) / g_game_res_w, fh = 0.9 * (wa.bottom - wa.top) / g_game_res_h;
            if (fh < fit) fit = fh;
            if (fit > 1) fit = 1;
            ww = (int)(g_game_res_w * fit), wh = (int)(g_game_res_h * fit);
        }
    }
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_d3d.hwnd = CreateWindowA("GladiusPC", port_is_editor() ? "Gladius Editor" : "Gladius",
                               WS_OVERLAPPEDWINDOW | (g_handoff_ev ? 0 : WS_VISIBLE),
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                               wc.hInstance, nullptr);
    if (g_handoff_ev) {                                 // soft reboot: the old window's spot, shown on the first frame
        WINDOWPLACEMENT wp = {sizeof wp};
        wp.showCmd = SW_HIDE;
        wp.rcNormalPosition = g_handoff_rect;
        SetWindowPlacement(g_d3d.hwnd, &wp);
    }
    SetEvent(g_window_ready);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}

void d3d_window_start()
{
    if (g_d3d.hwnd)
        return;
    g_window_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, window_thread, nullptr, 0, nullptr));
    WaitForSingleObject(g_window_ready, INFINITE);
}

// ---------------------------------------------------------------- device
static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void d3d_video_options_load()
{
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());

    char scale_text[32];
    GetPrivateProfileStringA("video", "RenderScale", "1", scale_text, sizeof scale_text, ini);
    g_d3d.auto_render_scale = !_stricmp(scale_text, "auto");
    int scale = g_d3d.auto_render_scale ? 1 : clamp_int(atoi(scale_text), 1, 4);
    extern UINT g_game_res_w, g_game_res_h;            // game_resolution.cpp (read before the device)
    if (g_game_res_h && (g_game_res_w != 640 || g_game_res_h != 480)) {   // Resolution = monitor: already native
        if (scale != 1 || g_d3d.auto_render_scale)
            port_log("video: RenderScale %s ignored - Resolution %ux%u is native already\n", scale_text, g_game_res_w,
                     g_game_res_h);
        scale = 1;
        g_d3d.auto_render_scale = false;
    }
    g_d3d.render_scale = (UINT)scale;
    g_d3d.ss_x = g_d3d.game_ss_x * g_d3d.render_scale;
    g_d3d.ss_y = g_d3d.game_ss_y * g_d3d.render_scale;

    char filter[32];
    GetPrivateProfileStringA("video", "FinalFilter", "linear", filter, sizeof filter, ini);
    g_d3d.final_filter = _stricmp(filter, "point") && _stricmp(filter, "nearest") ? 1 : 0;

    int sharp = GetPrivateProfileIntA("video", "Sharpen", 0, ini);
    sharp = clamp_int(sharp, 0, 100);
    g_d3d.sharpen = sharp / 100.0f;

    char tex_filter[32];
    GetPrivateProfileStringA("video", "TextureFilter", "linear", tex_filter, sizeof tex_filter, ini);
    if (!_stricmp(tex_filter, "point") || !_stricmp(tex_filter, "nearest")) {
        g_d3d.texture_filter = 0;
    } else if (!_stricmp(tex_filter, "anisotropic") || !_stricmp(tex_filter, "anisotropy")) {
        g_d3d.texture_filter = 2;
    } else {
        g_d3d.texture_filter = 1;
    }
    g_d3d.anisotropy = (UINT)clamp_int(GetPrivateProfileIntA("video", "Anisotropy", 8, ini), 1, 16);
    char aa[32];
    GetPrivateProfileStringA("video", "AntiAlias", "off", aa, sizeof aa, ini);
    g_d3d.anti_alias = !_stricmp(aa, "fxaa") ? 1 : !_stricmp(aa, "taa") ? 2 : 0;
    char lighting[32];
    GetPrivateProfileStringA("video", "Lighting", "vertex", lighting, sizeof lighting, ini);
    g_d3d.pixel_lighting = !_stricmp(lighting, "pixel");
    {
        char v[32];
        GetPrivateProfileStringA("video", "Specular", "0.6", v, sizeof v, ini);
        g_d3d.specular = (float)atof(v);
        GetPrivateProfileStringA("video", "SpecPower", "24", v, sizeof v, ini);
        g_d3d.spec_power = (float)atof(v);
        GetPrivateProfileStringA("video", "Rim", "0.25", v, sizeof v, ini);
        g_d3d.rim = (float)atof(v);
        GetPrivateProfileStringA("video", "SunRelight", "0.5", v, sizeof v, ini);
        g_d3d.sun_relight = (float)atof(v);
        GetPrivateProfileStringA("video", "Bump", "0.05", v, sizeof v, ini);
        g_d3d.bump = (float)atof(v);
        GetPrivateProfileStringA("video", "FxLights", "0.6", v, sizeof v, ini);
        g_d3d.fx_lights = (float)atof(v);
        GetPrivateProfileStringA("video", "Bloom", "0.6", v, sizeof v, ini);
        g_d3d.bloom = (float)atof(v);
        GetPrivateProfileStringA("video", "BloomThreshold", "1.0", v, sizeof v, ini);
        g_d3d.bloom_threshold = (float)atof(v);
        GetPrivateProfileStringA("video", "GodRays", "0.5", v, sizeof v, ini);
        g_d3d.god_rays = (float)atof(v);
        GetPrivateProfileStringA("video", "Saturation", "1.0", v, sizeof v, ini);
        g_d3d.saturation = (float)atof(v);
        GetPrivateProfileStringA("video", "Contrast", "1.0", v, sizeof v, ini);
        g_d3d.contrast = (float)atof(v);
        GetPrivateProfileStringA("video", "Warmth", "0", v, sizeof v, ini);
        g_d3d.warmth = (float)atof(v);
        GetPrivateProfileStringA("video", "Vignette", "0", v, sizeof v, ini);
        g_d3d.vignette = (float)atof(v);
        GetPrivateProfileStringA("video", "Materials", "1.0", v, sizeof v, ini);
        g_d3d.materials = (float)atof(v);
        GetPrivateProfileStringA("video", "HdrWhite", "4.0", v, sizeof v, ini);
        g_d3d.hdr_white = (float)atof(v) > 1.01f ? (float)atof(v) : 1.01f;
    }
    g_d3d.hdr = GetPrivateProfileIntA("video", "HDR", 1, ini) != 0;
    g_d3d.soft_particles = GetPrivateProfileIntA("video", "SoftParticles", 0, ini) != 0;
    char num[32];
    GetPrivateProfileStringA("video", "SoftRange", "1.0", num, sizeof num, ini);
    g_d3d.soft_range = (float)atof(num);
    if (g_d3d.soft_range <= 0) g_d3d.soft_range = 1.0f;
    GetPrivateProfileStringA("debug", "ShowDepth", "0", num, sizeof num, ini);
    g_d3d.show_depth = (float)atof(num);
}

static bool create_scene_targets()
{
    if (g_d3d.auto_render_scale && g_d3d.bb_width && g_d3d.bb_height) {
        UINT base_w = g_d3d.width;
        UINT base_h = g_d3d.height;
        UINT sx = base_w ? g_d3d.bb_width / base_w : 1;
        UINT sy = base_h ? g_d3d.bb_height / base_h : 1;
        UINT scale = sx < sy ? sx : sy;
        g_d3d.render_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        g_d3d.ss_x = g_d3d.game_ss_x * g_d3d.render_scale;
        g_d3d.ss_y = g_d3d.game_ss_y * g_d3d.render_scale;
    }
    g_d3d.scene_width = g_d3d.width * g_d3d.ss_x;
    g_d3d.scene_height = g_d3d.height * g_d3d.ss_y;
    if (!g_d3d.scene_width) g_d3d.scene_width = g_d3d.bb_width;
    if (!g_d3d.scene_height) g_d3d.scene_height = g_d3d.bb_height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_d3d.scene_width;
    td.Height = g_d3d.scene_height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = g_d3d.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;   // [video] HDR
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.scene_tex);
    if (FAILED(hr)) {
        port_log("D3D11 scene texture %ux%u failed %08X\n", td.Width, td.Height, hr);
        return false;
    }
    g_d3d.dev->CreateRenderTargetView(g_d3d.scene_tex, nullptr, &g_d3d.rtv);
    g_d3d.dev->CreateShaderResourceView(g_d3d.scene_tex, nullptr, &g_d3d.scene_srv);
    td.Format = DXGI_FORMAT_R32_FLOAT;                  // G-buffer: linear view depth
    if (SUCCEEDED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.gbuf_tex))) {
        g_d3d.dev->CreateRenderTargetView(g_d3d.gbuf_tex, nullptr, &g_d3d.gbuf_rtv);
        g_d3d.dev->CreateShaderResourceView(g_d3d.gbuf_tex, nullptr, &g_d3d.gbuf_srv);
    }
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;          // G-buffer: view-space normals (lit / relit draws)
    if (SUCCEEDED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.gnrm_tex))) {
        g_d3d.dev->CreateRenderTargetView(g_d3d.gnrm_tex, nullptr, &g_d3d.gnrm_rtv);
        g_d3d.dev->CreateShaderResourceView(g_d3d.gnrm_tex, nullptr, &g_d3d.gnrm_srv);
    }
    td.Format = DXGI_FORMAT_R16G16_FLOAT;               // motion vectors (uv offset to last frame, TAA)
    if (SUCCEEDED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_d3d.gmv_tex))) {
        g_d3d.dev->CreateRenderTargetView(g_d3d.gmv_tex, nullptr, &g_d3d.gmv_rtv);
        g_d3d.dev->CreateShaderResourceView(g_d3d.gmv_tex, nullptr, &g_d3d.gmv_srv);
    }
    return g_d3d.rtv && g_d3d.scene_srv;
}

static void release_scene_targets()
{
    if (g_d3d.rtv) { g_d3d.rtv->Release(); g_d3d.rtv = nullptr; }
    if (g_d3d.scene_srv) { g_d3d.scene_srv->Release(); g_d3d.scene_srv = nullptr; }
    if (g_d3d.scene_tex) { g_d3d.scene_tex->Release(); g_d3d.scene_tex = nullptr; }
    if (g_d3d.gbuf_srv) { g_d3d.gbuf_srv->Release(); g_d3d.gbuf_srv = nullptr; }
    if (g_d3d.gbuf_rtv) { g_d3d.gbuf_rtv->Release(); g_d3d.gbuf_rtv = nullptr; }
    if (g_d3d.gbuf_tex) { g_d3d.gbuf_tex->Release(); g_d3d.gbuf_tex = nullptr; }
    if (g_d3d.gnrm_srv) { g_d3d.gnrm_srv->Release(); g_d3d.gnrm_srv = nullptr; }
    if (g_d3d.gnrm_rtv) { g_d3d.gnrm_rtv->Release(); g_d3d.gnrm_rtv = nullptr; }
    if (g_d3d.gnrm_tex) { g_d3d.gnrm_tex->Release(); g_d3d.gnrm_tex = nullptr; }
    if (g_d3d.gmv_srv) { g_d3d.gmv_srv->Release(); g_d3d.gmv_srv = nullptr; }
    if (g_d3d.gmv_rtv) { g_d3d.gmv_rtv->Release(); g_d3d.gmv_rtv = nullptr; }
    if (g_d3d.gmv_tex) { g_d3d.gmv_tex->Release(); g_d3d.gmv_tex = nullptr; }
}

static bool recreate_present_target()
{
    if (g_d3d.present_rtv) {
        g_d3d.present_rtv->Release();
        g_d3d.present_rtv = nullptr;
    }
    ID3D11Texture2D *bb = nullptr;
    HRESULT hr = g_d3d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    if (FAILED(hr) || !bb) {
        port_log("D3D11 back buffer get failed %08X\n", hr);
        return false;
    }
    hr = g_d3d.dev->CreateRenderTargetView(bb, nullptr, &g_d3d.present_rtv);
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    g_d3d.bb_width = bd.Width;
    g_d3d.bb_height = bd.Height;
    bb->Release();
    if (FAILED(hr) || !g_d3d.present_rtv) {
        port_log("D3D11 present RTV %ux%u failed %08X\n", bd.Width, bd.Height, hr);
        return false;
    }
    return true;
}

bool d3d_device_create(UINT width, UINT height)
{
    prof_init();
    d3d_window_start();
    d3d_video_options_load();
    g_d3d.width = width ? width : 640;
    g_d3d.height = height ? height : 480;
    RECT cr;
    GetClientRect(g_d3d.hwnd, &cr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = cr.right - cr.left;
    sd.BufferDesc.Height = cr.bottom - cr.top;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_d3d.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &g_d3d.swap, &g_d3d.dev, &fl, &g_d3d.ctx);
    if (FAILED(hr)) {                       // pre-Windows 10: blt-model swap chain
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &g_d3d.swap, &g_d3d.dev, &fl, &g_d3d.ctx);
    }
    if (FAILED(hr)) {
        port_log("D3D11CreateDeviceAndSwapChain failed %08X\n", hr);
        return false;
    }
    if (!recreate_present_target())
        return false;
    if (!create_scene_targets())
        return false;
    void d3d_ssao_init();                               // d3d_ssao.cpp: compile now, not on the first battle frame
    d3d_ssao_init();
    d3d_shadow_init();
    g_d3d.stats_frame = true;                           // frame 0
    const char *tf = g_d3d.texture_filter == 2 ? "anisotropic" : g_d3d.texture_filter ? "linear" : "point";
    port_log("D3D11 device: feature level %X, window %ux%u, game back buffer %ux%u, scene %ux%u, final=%s sharpen=%.2f texture=%s/%ux aa=%s\n",
             fl, sd.BufferDesc.Width, sd.BufferDesc.Height, g_d3d.width, g_d3d.height, g_d3d.scene_width,
             g_d3d.scene_height, g_d3d.final_filter ? "linear" : "point", g_d3d.sharpen, tf, g_d3d.anisotropy,
             aa_name());
    return true;
}

static void d3d_apply_resize()
{
    if (!g_d3d.swap)
        return;
    RECT cr;
    GetClientRect(g_d3d.hwnd, &cr);
    UINT cw = cr.right > cr.left ? (UINT)(cr.right - cr.left) : 0;
    UINT ch = cr.bottom > cr.top ? (UINT)(cr.bottom - cr.top) : 0;
    if (cw && ch && (cw != g_d3d.bb_width || ch != g_d3d.bb_height))
        request_resize(cw, ch);
    if (!InterlockedExchange(&g_pending_resize, 0))
        return;
    UINT w = (UINT)InterlockedCompareExchange(&g_pending_w, 0, 0);
    UINT h = (UINT)InterlockedCompareExchange(&g_pending_h, 0, 0);
    if (!w || !h || (w == g_d3d.bb_width && h == g_d3d.bb_height))
        return;

    ID3D11RenderTargetView *no_rtv = nullptr;
    ID3D11ShaderResourceView *no_srv = nullptr;
    g_d3d.ctx->OMSetRenderTargets(0, &no_rtv, nullptr);
    g_d3d.ctx->PSSetShaderResources(0, 1, &no_srv);
    if (g_d3d.present_rtv) { g_d3d.present_rtv->Release(); g_d3d.present_rtv = nullptr; }
    HRESULT hr = g_d3d.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        port_log("D3D11 ResizeBuffers %ux%u failed %08X\n", w, h, hr);
        return;
    }
    if (!recreate_present_target())
        return;

    UINT old_scene_w = g_d3d.scene_width, old_scene_h = g_d3d.scene_height;
    if (g_d3d.auto_render_scale) {
        release_scene_targets();
        if (!create_scene_targets())
            return;
        if (old_scene_w != g_d3d.scene_width || old_scene_h != g_d3d.scene_height)
            d3d_scene_targets_changed();
    }
    d3d_state_invalidate();
    port_log("D3D11 resize: window %ux%u, scene %ux%u, render scale %u\n",
             g_d3d.bb_width, g_d3d.bb_height, g_d3d.scene_width, g_d3d.scene_height, g_d3d.render_scale);
}

// ---------------------------------------------------------------- final pass
static ID3D11VertexShader *g_final_vs;
static ID3D11PixelShader *g_final_ps;
static ID3D11SamplerState *g_final_sampler[2];
static ID3D11Buffer *g_final_cb;
static ID3D11BlendState *g_final_blend;
static ID3D11DepthStencilState *g_final_depth;
static ID3D11RasterizerState *g_final_raster;
static bool g_final_failed;

static const char *k_final_hlsl = R"(
Texture2D sceneTex : register(t0);
Texture2D<float> gbufTex : register(t1);
Texture2D bloomTex : register(t2);                      // d3d_bloom.cpp (half resolution), bound when on
SamplerState sceneSampler : register(s0);
SamplerState linSampler : register(s1);
cbuffer FinalCB : register(b0) { float4 texel_sharp;  // xy = 1/source size, z = sharpen, w = depth view scale
                                 float4 hdr;           // x = on, y = white (input level that reaches pure white), z = bloom
                                 float4 grade;         // saturation, contrast, warmth, vignette
                                 float4 down; };       // xy = scene texels per output pixel (> 1: shrinking)
// Shrinking the scene to the window (the game's 2x vertical, Monitor resolution: 2160 rows -> ~1000) with one bilinear
// tap skipped rows in a fixed pattern - moire: SSAO noise / fine detail became static horizontal bars. Average every
// texel the output pixel covers (up to 4 x 4 bilinear taps, each already 2 x 2).
float4 scene_at(float2 uv)
{
    if (down.x <= 1.01 && down.y <= 1.01) return sceneTex.Sample(sceneSampler, uv);
    int nx = (int)min(ceil(down.x * 0.5), 4), ny = (int)min(ceil(down.y * 0.5), 4);
    float2 step = texel_sharp.xy * down.xy / float2(nx, ny);
    float4 s = 0;
    for (int y = 0; y < ny; y++)
        for (int x = 0; x < nx; x++)
            s += sceneTex.SampleLevel(linSampler, uv + (float2(x, y) - (float2(nx, ny) - 1) * 0.5) * step, 0);
    return s / (nx * ny);
}
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target
{
    if (texel_sharp.w < 0) {                            // debug key 0, 3rd view: depth bend between rows (x 2000)
        int2 q = int2(i.uv / texel_sharp.xy);
        float d0 = abs(gbufTex.Load(int3(q, 0))), du = abs(gbufTex.Load(int3(q - int2(0, 1), 0)));
        float dd = abs(gbufTex.Load(int3(q + int2(0, 1), 0)));
        if (d0 > 60000) return float4(0, 0, 0.3, 1);
        return float4(saturate(abs(du + dd - 2 * d0) / d0 * 2000).xxx, 1);
    }
    if (texel_sharp.w > 0) {                            // [debug] ShowDepth: near = black, far = white
        float d = gbufTex.Load(int3(i.uv / texel_sharp.xy, 0));
        return float4((1 - exp(-d * texel_sharp.w)).xxx, 1);
    }
    float4 c = scene_at(i.uv);
    float a = texel_sharp.z;
    if (a > 0.001) {
        float2 t = texel_sharp.xy;
        float4 n = sceneTex.Sample(sceneSampler, i.uv + float2(0, -t.y));
        float4 s = sceneTex.Sample(sceneSampler, i.uv + float2(0,  t.y));
        float4 e = sceneTex.Sample(sceneSampler, i.uv + float2( t.x, 0));
        float4 w = sceneTex.Sample(sceneSampler, i.uv + float2(-t.x, 0));
        float4 blur = (n + s + e + w) * 0.25;
        c.rgb = max(c.rgb + (c.rgb - blur.rgb) * a, 0);
    }
    if (hdr.z > 0)                                      // bloom: overbright light bleeding around
        c.rgb += bloomTex.SampleLevel(linSampler, i.uv, 0).rgb * hdr.z;
    // grading ([video] Saturation / Contrast / Warmth / Vignette; neutral = 1, 1, 0, 0)
    float lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    c.rgb = max(lerp(lum.xxx, c.rgb, grade.x), 0);
    c.rgb = max((c.rgb - 0.5) * grade.y + 0.5, 0);
    c.rgb *= float3(1 + grade.z * 0.06, 1 + grade.z * 0.01, 1 - grade.z * 0.06);
    float2 vq = i.uv * 2 - 1;
    c.rgb *= 1 - grade.w * saturate(dot(vq, vq) * 0.5 - 0.1);
    // HDR scene (stacked additive effects, lighting extras above 1): everything up to 1 is shown exactly as
    // authored; above, the colour keeps its hue at full brightness and rolls towards white as the excess grows
    if (hdr.x > 0) {
        c.rgb = max(c.rgb, 0);
        float m = max(c.r, max(c.g, c.b));
        if (m > 1) {
            float e = (m - 1) / (hdr.y - 1);
            c.rgb = lerp(c.rgb / m, 1, saturate(e / (1 + e) * 2));
        }
    }
    return saturate(c);
}
)";

// ---------------------------------------------------------------- post chain (scene -> effects -> final pass)
// [video] AntiAlias=fxaa: FXAA 3.11-style edge AA at scene resolution into g_post_tex, then scaled by the final pass.
static ID3D11PixelShader *g_fxaa_ps;
static ID3D11Texture2D *g_post_tex;
static ID3D11RenderTargetView *g_post_rtv;
static ID3D11ShaderResourceView *g_post_srv;
static UINT g_post_w, g_post_h;

static const char *k_fxaa_hlsl = R"(
Texture2D T : register(t0);
SamplerState S : register(s0);
cbuffer FinalCB : register(b0) { float4 texel_sharp; float4 hdr; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
float L(float3 c) { return dot(saturate(c), float3(0.299, 0.587, 0.114)); }   // edges as displayed (HDR scene)
float LS(float2 uv) { return L(T.SampleLevel(S, uv, 0).rgb); }
static const float Q[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
float4 ps(V i) : SV_Target
{
    float2 r = texel_sharp.xy, uv = i.uv;
    float4 full = T.SampleLevel(S, uv, 0);
    float lM = L(full.rgb);
    float lN = LS(uv + float2(0, -r.y)), lS = LS(uv + float2(0, r.y));
    float lW = LS(uv + float2(-r.x, 0)), lE = LS(uv + float2(r.x, 0));
    float mx = max(lM, max(max(lN, lS), max(lW, lE))), mn = min(lM, min(min(lN, lS), min(lW, lE)));
    float range = mx - mn;
    if (range < max(0.0312, mx * 0.125))
        return full;
    float lNW = LS(uv + float2(-r.x, -r.y)), lNE = LS(uv + float2(r.x, -r.y));
    float lSW = LS(uv + float2(-r.x, r.y)), lSE = LS(uv + float2(r.x, r.y));
    float lNS = lN + lS, lWE = lW + lE, lWc = lNW + lSW, lEc = lNE + lSE, lNc = lNW + lNE, lSc = lSW + lSE;
    float eh = abs(-2 * lW + lWc) + abs(-2 * lM + lNS) * 2 + abs(-2 * lE + lEc);
    float ev = abs(-2 * lN + lNc) + abs(-2 * lM + lWE) * 2 + abs(-2 * lS + lSc);
    bool horz = eh >= ev;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float gs = 0.25 * max(abs(g1), abs(g2));
    float stp = horz ? r.y : r.x, lavg;
    if (steep1) { stp = -stp; lavg = 0.5 * (l1 + lM); } else lavg = 0.5 * (l2 + lM);
    float2 c = uv;
    if (horz) c.y += stp * 0.5; else c.x += stp * 0.5;
    float2 off = horz ? float2(r.x, 0) : float2(0, r.y);
    float2 u1 = c - off, u2 = c + off;
    float e1 = LS(u1) - lavg, e2 = LS(u2) - lavg;
    bool d1 = abs(e1) >= gs, d2 = abs(e2) >= gs;
    if (!d1) u1 -= off;
    if (!d2) u2 += off;
    [loop] for (int k = 2; k < 12 && !(d1 && d2); k++) {
        if (!d1) { e1 = LS(u1) - lavg; d1 = abs(e1) >= gs; if (!d1) u1 -= off * Q[k]; }
        if (!d2) { e2 = LS(u2) - lavg; d2 = abs(e2) >= gs; if (!d2) u2 += off * Q[k]; }
    }
    float dist1 = horz ? uv.x - u1.x : uv.y - u1.y, dist2 = horz ? u2.x - uv.x : u2.y - uv.y;
    bool dir1 = dist1 < dist2;
    float dmin = min(dist1, dist2), thick = dist1 + dist2;
    float poff = -dmin / thick + 0.5;
    bool correct = ((dir1 ? e1 : e2) < 0) != (lM < lavg);
    float fo = correct ? poff : 0;
    float la = (2 * (lNS + lWE) + lWc + lEc) / 12.0;
    float s1 = saturate(abs(la - lM) / range);
    float s2 = (-2 * s1 + 3) * s1 * s1;
    fo = max(fo, s2 * s2 * 0.75);
    float2 f = uv;
    if (horz) f.y += fo * stp; else f.x += fo * stp;
    return float4(T.SampleLevel(S, f, 0).rgb, full.a);
}
)";

static void post_release()
{
    if (g_post_srv) { g_post_srv->Release(); g_post_srv = nullptr; }
    if (g_post_rtv) { g_post_rtv->Release(); g_post_rtv = nullptr; }
    if (g_post_tex) { g_post_tex->Release(); g_post_tex = nullptr; }
    g_post_w = g_post_h = 0;
}

static bool post_target(UINT w, UINT h)                 // (re)created when the scene size changes
{
    if (g_post_tex && g_post_w == w && g_post_h == h)
        return true;
    post_release();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = g_d3d.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_post_tex)))
        return false;
    g_d3d.dev->CreateRenderTargetView(g_post_tex, nullptr, &g_post_rtv);
    g_d3d.dev->CreateShaderResourceView(g_post_tex, nullptr, &g_post_srv);
    g_post_w = w, g_post_h = h;
    return g_post_rtv && g_post_srv;
}

static bool final_init()
{
    if (g_final_vs)
        return true;
    if (g_final_failed)
        return false;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(k_final_hlsl, strlen(k_final_hlsl), "final", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(k_final_hlsl, strlen(k_final_hlsl), "final", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &err))) {
        port_log("final pass shader compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        g_final_failed = true;
        return false;
    }
    ID3DBlob *fx = nullptr;
    if (SUCCEEDED(D3DCompile(k_fxaa_hlsl, strlen(k_fxaa_hlsl), "fxaa", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &fx, &err))) {
        g_d3d.dev->CreatePixelShader(fx->GetBufferPointer(), fx->GetBufferSize(), nullptr, &g_fxaa_ps);
        fx->Release();
    } else {
        port_log("FXAA shader compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
    }
    g_d3d.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_final_vs);
    g_d3d.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_final_ps);
    vs->Release();
    ps->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_d3d.dev->CreateBuffer(&bd, nullptr, &g_final_cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_d3d.dev->CreateSamplerState(&sd, &g_final_sampler[0]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_d3d.dev->CreateSamplerState(&sd, &g_final_sampler[1]);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = 15;
    g_d3d.dev->CreateBlendState(&bl, &g_final_blend);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    g_d3d.dev->CreateDepthStencilState(&dd, &g_final_depth);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    g_d3d.dev->CreateRasterizerState(&rd, &g_final_raster);
    return g_final_vs && g_final_ps && g_final_cb && g_final_sampler[0] && g_final_sampler[1];
}

static void final_pass_draw()
{
    if (!g_d3d.present_rtv || !g_d3d.scene_srv || !final_init())
        return;
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    ID3D11Texture2D *bb = nullptr;
    D3D11_TEXTURE2D_DESC bd = {};
    if (SUCCEEDED(g_d3d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) && bb) {
        bb->GetDesc(&bd);
        bb->Release();
    }
    d3d_taa_before_final();                             // a 3D frame without a 2D layer: resolve now
    ID3D11ShaderResourceView *bloom = d3d_bloom_run(g_d3d.scene_srv, g_d3d.scene_width, g_d3d.scene_height);
    ID3D11ShaderResourceView *src = g_d3d.scene_srv;
    if (g_d3d.anti_alias == 1 && g_fxaa_ps && post_target(g_d3d.scene_width, g_d3d.scene_height)) {
        prof_gpu_pass(GP_FXAA);
        D3D11_VIEWPORT pv = {0, 0, (float)g_post_w, (float)g_post_h, 0, 1};
        ctx->OMSetRenderTargets(1, &g_post_rtv, nullptr);
        ctx->RSSetViewports(1, &pv);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_final_vs, nullptr, 0);
        ctx->PSSetShader(g_fxaa_ps, nullptr, 0);
        float pcb[16] = {1.0f / (float)g_post_w, 1.0f / (float)g_post_h, 0, 0};
        ctx->UpdateSubresource(g_final_cb, 0, nullptr, pcb, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_final_cb);
        ctx->PSSetShaderResources(0, 1, &g_d3d.scene_srv);
        ctx->PSSetSamplers(0, 1, &g_final_sampler[1]);
        float pf[4] = {};
        ctx->OMSetBlendState(g_final_blend, pf, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_final_depth, 0);
        ctx->RSSetState(g_final_raster);
        ctx->Draw(3, 0);
        ID3D11RenderTargetView *no_rtv = nullptr;
        ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
        src = g_post_srv;
    }
    prof_gpu_pass(GP_FINAL);
    UINT out_w = bd.Width ? bd.Width : g_d3d.bb_width;
    UINT out_h = bd.Height ? bd.Height : g_d3d.bb_height;
    // keep the game's aspect (UiMode=center renders a narrower frame than the window): centred, black bars
    extern UINT g_game_res_w, g_game_res_h;
    float vw = (float)out_w, vh = (float)out_h;
    if (g_game_res_w * out_h != g_game_res_h * out_w) {
        float ga = (float)g_game_res_w / g_game_res_h;
        if (vw > vh * ga) vw = vh * ga;
        else vh = vw / ga;
        const float black[4] = {0, 0, 0, 1};
        ctx->ClearRenderTargetView(g_d3d.present_rtv, black);
    }
    D3D11_VIEWPORT vp = {floorf((out_w - vw) * 0.5f), floorf((out_h - vh) * 0.5f), vw, vh, 0, 1};
    ctx->OMSetRenderTargets(1, &g_d3d.present_rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_final_vs, nullptr, 0);
    ctx->PSSetShader(g_final_ps, nullptr, 0);
    float cb[16] = {1.0f / (float)g_d3d.scene_width, 1.0f / (float)g_d3d.scene_height, g_d3d.sharpen,
                    g_d3d.gbuf_srv ? g_d3d.show_depth : 0, g_d3d.hdr ? 1.0f : 0.0f, g_d3d.hdr_white,
                    bloom ? g_d3d.bloom : 0.0f, 0,
                    g_d3d.saturation, g_d3d.contrast, g_d3d.warmth, g_d3d.vignette,
                    (float)g_d3d.scene_width / vw, (float)g_d3d.scene_height / vh, 0, 0};
    ctx->UpdateSubresource(g_final_cb, 0, nullptr, cb, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_final_cb);
    ID3D11ShaderResourceView *fsrv[3] = {src, g_d3d.gbuf_srv, bloom};
    ctx->PSSetShaderResources(0, 3, fsrv);
    ID3D11SamplerState *s[2] = {g_final_sampler[g_d3d.final_filter ? 1 : 0], g_final_sampler[1]};
    ctx->PSSetSamplers(0, 2, s);
    float factor[4] = {};
    ctx->OMSetBlendState(g_final_blend, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_final_depth, 0);
    ctx->RSSetState(g_final_raster);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
}

// [debug] Screenshot=N[,M...]: save the back buffer of those frames as build\shot_<frame>.bmp
static char g_shot_path[MAX_PATH];             // port_screenshot(): one pending shot by full path

void port_screenshot(const char *path)
{
    strncpy(g_shot_path, path, MAX_PATH - 1);
}

static void screenshot_if_requested()
{
    static DWORD frames[16];
    static int nframes = -1;
    if (nframes < 0) {
        nframes = 0;
        char ini[MAX_PATH], val[128];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        GetPrivateProfileStringA("debug", "Screenshot", "", val, sizeof val, ini);
        for (char *t = strtok(val, ", "); t && nframes < 16; t = strtok(nullptr, ", "))
            frames[nframes++] = strtoul(t, nullptr, 10);
    }
    bool want = g_shot_path[0] != 0;
    for (int i = 0; i < nframes; i++)
        want |= frames[i] == g_d3d.frame;
    if (!want)
        return;
    if (g_d3d.gbuf_tex) {                               // G-buffer depth percentiles (game units) with each shot
        D3D11_TEXTURE2D_DESC gd;
        g_d3d.gbuf_tex->GetDesc(&gd);
        gd.Usage = D3D11_USAGE_STAGING;
        gd.BindFlags = 0;
        gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        gd.MiscFlags = 0;
        ID3D11Texture2D *gs = nullptr;
        if (SUCCEEDED(g_d3d.dev->CreateTexture2D(&gd, nullptr, &gs))) {
            g_d3d.ctx->CopyResource(gs, g_d3d.gbuf_tex);
            D3D11_MAPPED_SUBRESOURCE gm;
            if (SUCCEEDED(g_d3d.ctx->Map(gs, 0, D3D11_MAP_READ, 0, &gm))) {
                std::vector<float> v;
                for (UINT y = 0; y < gd.Height; y += 4)
                    for (UINT x = 0; x < gd.Width; x += 4) {
                        float d = ((const float *)((const BYTE *)gm.pData + y * gm.RowPitch))[x];
                        if (d > 0 && d < 65000)
                            v.push_back(d);
                    }
                g_d3d.ctx->Unmap(gs, 0);
                if (!v.empty()) {
                    std::sort(v.begin(), v.end());
                    auto pc = [&](double q) { return v[(size_t)(q * (v.size() - 1))]; };
                    port_log("gbuf depth frame %u: %u%% covered, p1 %.3f p10 %.3f p50 %.3f p90 %.3f max %.3f\n",
                             g_d3d.frame, (unsigned)(v.size() * 100 / ((gd.Width / 4) * (gd.Height / 4))), pc(0.01),
                             pc(0.1), pc(0.5), pc(0.9), v.back());
                } else {
                    port_log("gbuf depth frame %u: empty\n", g_d3d.frame);
                }
            }
            gs->Release();
        }
    }
    ID3D11Texture2D *bb, *staging;
    g_d3d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    g_d3d.dev->CreateTexture2D(&td, nullptr, &staging);
    g_d3d.ctx->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_d3d.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        _snprintf(strrchr(path, 0x5C) + 1, 64, "shot_%u.bmp", g_d3d.frame);
        if (g_shot_path[0]) { strcpy(path, g_shot_path); g_shot_path[0] = 0; }
        FILE *f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof ih;
            ih.biWidth = td.Width;
            ih.biHeight = -(LONG)td.Height;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + td.Width * td.Height * 4;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            for (UINT y = 0; y < td.Height; y++) {                 // RGBA -> BGRA
                const BYTE *row = (const BYTE *)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < td.Width; x++) {
                    BYTE px[4] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4], 255};
                    fwrite(px, 4, 1, f);
                }
            }
            fclose(f);
            port_log("screenshot: %s\n", path);
        }
        g_d3d.ctx->Unmap(staging, 0);
    }
    staging->Release();
    bb->Release();
}

void d3d_present()
{
    if (!g_d3d.swap)
        return;
    d3d_apply_resize();
    // frame timing: "cpu" = game + draw submission since the last Present returned, "present" = time blocked in Present
    static LARGE_INTEGER qpf, prev_end;
    static double cpu_sum, cpu_max, pres_sum, pres_max;
    static DWORD draw_sum, draw_max, timed;
    static int perf_log = -1;
    if (!qpf.QuadPart) {
        QueryPerformanceFrequency(&qpf);
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        perf_log = GetPrivateProfileIntA("debug", "PerfLog", 0, ini);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    {
        ProfScope pp(PZ_POST);
        d3d_shadow_frame_end();                         // recorded shadow casters are this frame's only
        final_pass_draw();
        overlay_draw();
    }
    screenshot_if_requested();
    prof_frame_close_gpu();                             // before Present: its commands go out with this frame
    {
        ProfScope ps(PZ_PRESENT);
        g_d3d.swap->Present(1, 0);
    }
    static bool handoff_posted;
    if (g_handoff_ev && !handoff_posted) {
        handoff_posted = true;
        PostMessageA(g_d3d.hwnd, WM_HANDOFF_SHOW, 0, 0);
    }
    d3d_state_invalidate();                             // overlay bound its own state; flip Present unbinds targets
    QueryPerformanceCounter(&t1);
    if (prev_end.QuadPart)
        prof_frame_end((t1.QuadPart - prev_end.QuadPart) * 1000.0 / qpf.QuadPart);
    port_thread_stats();
    prof_frame_begin_gpu();
    if (prev_end.QuadPart) {
        double cpu = (t0.QuadPart - prev_end.QuadPart) * 1000.0 / qpf.QuadPart;
        double pres = (t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
        g_prof_last.wall_ms = (float)((t1.QuadPart - prev_end.QuadPart) * 1000.0 / qpf.QuadPart);   // devtools graphs
        g_prof_last.cpu_ms = (float)cpu, g_prof_last.present_ms = (float)pres, g_prof_last.draws = g_d3d.draws;
        g_prof_last.frame = g_d3d.frame;
        cpu_sum += cpu, pres_sum += pres, draw_sum += g_d3d.draws, timed++;
        if (cpu > cpu_max) cpu_max = cpu;
        if (pres > pres_max) pres_max = pres;
        if (g_d3d.draws > draw_max) draw_max = g_d3d.draws;
    }
    prev_end = t1;
    // title: frame counter, fps and timing, twice a second
    static DWORD last_tick, last_frame;
    DWORD now = GetTickCount();
    if (now - last_tick >= 500 && timed) {
        char title[384];
        _snprintf(title, sizeof title,
                  "Gladius - frame %u - %.0f fps | cpu %.1f ms (max %.1f) | present %.1f ms (max %.1f) | draws %u (max %u)"
                  " || F4 HD %s  F5 shadows %s  F6 AO %s  F7 soft %s  F8 light %s  F9 AA %s",
                  g_d3d.frame, (g_d3d.frame - last_frame) * 1000.0 / (now - last_tick), cpu_sum / timed, cpu_max,
                  pres_sum / timed, pres_max, draw_sum / timed, draw_max, g_d3d.hd_textures ? "on" : "off",
                  g_d3d.shadows ? "on" : "off", g_d3d.ssao ? "on" : "off", g_d3d.soft_particles ? "on" : "off",
                  g_d3d.pixel_lighting ? "pixel" : "vertex", aa_name());
        title[sizeof title - 1] = 0;
        SetWindowTextA(g_d3d.hwnd, title);
        if (perf_log > 0)
            port_log("perf: %s\n", title + 10);
        last_tick = now;
        last_frame = g_d3d.frame;
        cpu_sum = cpu_max = pres_sum = pres_max = 0;
        draw_sum = draw_max = timed = 0;
    }
}

// ---------------------------------------------------------------- push-buffer capture + NV2A method decoder
static const DWORD PUSH_DWORDS = 2 * 1024 * 1024;     // 8 MB
static DWORD *g_push_base, *g_parse;
static std::map<DWORD, DWORD> g_methods;              // method -> count (this frame)

void d3d_push_init()
{
    g_push_base = (DWORD *)VirtualAlloc(nullptr, PUSH_DWORDS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_parse = g_push_base;
    *(DWORD **)XBE_D3D_DEVICE_PUT = g_push_base;
    *(DWORD **)XBE_D3D_DEVICE_LIMIT = g_push_base + PUSH_DWORDS - 0x10000;
}

static void decode(DWORD *p, DWORD *end)
{
    while (p < end) {
        DWORD h = *p++;
        if (!h)
            continue;                                  // NOP
        if (h & 3) {
            port_log("push: unexpected jump/call header %08X\n", h);
            return;
        }
        DWORD method = h & 0x1FFC, count = (h >> 18) & 0x7FF;
        if (g_d3d.stats_frame)
            g_methods[method] += 1;
        d3d_nv_method(method, p, count);
        if (d3d_tracing()) {
            char data[200] = "";
            for (DWORD i = 0; i < count && i < 8; i++)
                _snprintf(data + strlen(data), sizeof data - strlen(data) - 1, " %08X", p[i]);
            d3d_trace("push method %04X x%u%s:%s%s", method, count, (h & 0x40000000) ? " (non-inc)" : "", data,
                      count > 8 ? " ..." : "");
        }
        p += count;
    }
}

void d3d_drain()
{
    if (!g_push_base)
        return;
    ProfScope pz(PZ_DRAIN);
    DWORD *put = *(DWORD **)XBE_D3D_DEVICE_PUT;
    if (put < g_push_base || put > g_push_base + PUSH_DWORDS) {
        port_log("push: write pointer %p outside capture buffer\n", put);
        return;
    }
    decode(g_parse, put);
    g_parse = put;
    if (put > g_push_base + PUSH_DWORDS / 2) {         // consumed: rewind
        g_parse = g_push_base;
        *(DWORD **)XBE_D3D_DEVICE_PUT = g_push_base;
    }
}

DWORD *d3d_push_space(DWORD dwords)
{
    d3d_drain();
    DWORD *put = *(DWORD **)XBE_D3D_DEVICE_PUT;
    if (put + dwords + 16 > *(DWORD **)XBE_D3D_DEVICE_LIMIT) {
        g_parse = g_push_base;
        *(DWORD **)XBE_D3D_DEVICE_PUT = put = g_push_base;
    }
    return put;
}

// ---------------------------------------------------------------- call statistics
static std::map<std::string, DWORD> g_calls;

// [debug] TraceFrame=N: log every D3D call (and decoded push methods) made while frame N is built
static DWORD g_trace_frame = 0xFFFFFFFF;

bool d3d_tracing() { return g_d3d.frame == g_trace_frame; }

void d3d_trace(const char *fmt, ...)
{
    if (!d3d_tracing())
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    port_log("  [trace] %s\n", buf);
}

void d3d_trace_init()
{
    char ini[MAX_PATH], val[32];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    GetPrivateProfileStringA("debug", "TraceFrame", "", val, sizeof val, ini);
    if (val[0]) {
        g_trace_frame = strtoul(val, nullptr, 10);
        port_log("debug: tracing D3D calls of frame %u\n", g_trace_frame);
    }
}

// counted only on frames whose statistics are logged (std::string keys cost a heap allocation per call)
void d3d_count(const char *name)
{
    if (g_d3d.stats_frame)
        g_calls[name] += 1;
    d3d_trace("%s", name);
}

void d3d_stub_call(DWORD addr, const char *name, const char *fmt, ...)
{
    D3DLock lock;
    d3d_drain();
    if (g_d3d.stats_frame)
        g_calls[name] += 1;
    static std::set<const char *> seen_names;           // names are string literals (one per stub)
    bool first = seen_names.insert(name).second;
    if (!first && !d3d_tracing())
        return;
    char args[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(args, sizeof args - 1, fmt, ap);
    va_end(ap);
    args[sizeof args - 1] = 0;
    if (first) {
        port_log("D3D stub first call: %s(%s) [xbe %06X]\n", name, args, addr);
    }
    d3d_trace("%s(%s)", name, args);
}

void d3d_frame_stats()
{
    DWORD f = g_d3d.frame;
    auto logged = [](DWORD f) { return f < 3 || f == 60 || f == 300 || (f && f % 1800 == 0); };
    g_d3d.stats_frame = logged(f + 1) || g_trace_frame == f + 1;   // counting for the next frame
    if (logged(f)) {
        port_log("--- frame %u D3D calls:", f);
        for (auto &kv : g_calls)
            port_log(" %s=%u", kv.first.c_str(), kv.second);
        port_log("\n    push methods:");
        for (auto &kv : g_methods)
            port_log(" %04X=%u", kv.first, kv.second);
        port_log("\n");
    }
    g_calls.clear();
    g_methods.clear();
}

// ---------------------------------------------------------------- device-level entry points
struct XVBlankData {
    DWORD VBlank, Swap, Flags;
};
static DWORD g_fake_device[64];                       // D3D_g_pDevice target (engine reads no fields)

// VBlank: the engine and Sofdec movies pace themselves on the Xbox video interrupt. A timer thread provides it at
// 59.94 Hz; rendering and presentation don't depend on it.
static HANDLE g_vblank_event;
static volatile LONG g_vblank_count;
extern "C" volatile LONG g_swap_count = 0;

static DWORD WINAPI vblank_thread(void *)
{
    port_set_vtid(0x1F00);                              // runs the game's VBlank callback (CRI): fixed virtual id
    port_fix_game_tls();                                // runs the game's VBlank callback
    HANDLE timer = CreateWaitableTimerExA(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerA(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -166833;                            // 16.6833 ms in 100 ns units (59.94 Hz)
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        LONG n = InterlockedIncrement(&g_vblank_count);
        // schedule the next tick against an absolute clock so the rate does not drift
        QueryPerformanceCounter(&now);
        double next = (n + 1) * (1001.0 / 60000.0);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / f.QuadPart;
        due.QuadPart = -(LONGLONG)((next - elapsed > 0.0005 ? next - elapsed : 0.0005) * 1e7);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        PulseEvent(g_vblank_event);
        if (g_d3d.vblank_callback) {
            XVBlankData vb = {(DWORD)n, (DWORD)g_swap_count, 0};
            g_d3d.vblank_callback((DWORD)&vb);
        }
    }
}

static void vblank_start()
{
    if (g_vblank_event)
        return;
    g_vblank_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, vblank_thread, nullptr, 0, nullptr);
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
}

extern "C" {

HRESULT __stdcall d3d_Direct3D_CreateDevice(DWORD adapter, DWORD type, HWND focus, DWORD behavior, DWORD *pp,
                                            void **out)
{
    D3DLock lock;
    d3d_count("Direct3D_CreateDevice");
    UINT w = pp ? pp[0] : 640, h = pp ? pp[1] : 480;
    DWORD ms = pp ? pp[4] : 0x11;                       // MultiSampleType: low byte = x factor << 4 | y factor
    g_d3d.game_ss_x = (ms >> 4) & 0xF ? (ms >> 4) & 0xF : 1;
    g_d3d.game_ss_y = ms & 0xF ? ms & 0xF : 1;
    g_d3d.ss_x = g_d3d.game_ss_x;
    g_d3d.ss_y = g_d3d.game_ss_y;
    port_log("Direct3D_CreateDevice: back buffer %ux%u format %X multisample %04X (screen space x%u y%u)\n", w, h,
             pp ? pp[2] : 0, ms, g_d3d.game_ss_x, g_d3d.game_ss_y);
    if (!g_d3d.dev && !d3d_device_create(w, h))
        return E_FAIL;
    d3d_push_init();
    d3d_trace_init();
    vblank_start();
    *(DWORD **)XBE_D3D_G_PDEVICE = g_fake_device;
    if (out)
        *out = g_fake_device;
    return S_OK;
}

DWORD __stdcall d3d_D3DDevice_Swap(DWORD flags)
{
    D3DLock lock;
    port_thread_yield_point();
    d3d_drain();
    d3d_count("D3DDevice_Swap");
    d3d_frame_stats();
    d3d_present();
    g_d3d.frame++;
    d3d_taa_frame_begin();
    g_d3d.draws = 0;
    InterlockedIncrement(&g_swap_count);
    return g_d3d.frame;
}

void __stdcall d3d_D3DDevice_BlockUntilVerticalBlank()
{
    {
        D3DLock lock;                                    // not held across the wait (CRI lock order)
        d3d_drain();
        d3d_count("D3DDevice_BlockUntilVerticalBlank");
    }
    port_thread_yield_point();
    int depth = port_lock_suspend();
    WaitForSingleObject(g_vblank_event, 50);            // next VBlank tick (bounded)
    port_lock_resume(depth);
}

void __stdcall d3d_D3DDevice_SetVerticalBlankCallback(void (__cdecl *cb)(DWORD))
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_SetVerticalBlankCallback");
    if (cb != g_d3d.vblank_callback)
        port_log("SetVerticalBlankCallback(%p)\n", cb);
    g_d3d.vblank_callback = cb;
}

void __stdcall d3d_D3DDevice_InsertCallback(DWORD type, void (__cdecl *cb)(DWORD), DWORD context)
{
    {
        D3DLock lock;                                    // game callback runs unlocked
        d3d_drain();
        d3d_count("D3DDevice_InsertCallback");
    }
    if (cb)
        cb(context);                                   // GPU "reached this point" = now
}

// push-buffer space: engine writes NV2A methods at the returned pointer, EndPush commits
DWORD *__stdcall d3d_D3DDevice_BeginPush_4(DWORD count)
{
    D3DLock lock;
    d3d_count("D3DDevice_BeginPush_4");
    return d3d_push_space(count + 1);
}

void __stdcall d3d_D3DDevice_EndPush(DWORD *p)
{
    D3DLock lock;
    *(DWORD **)XBE_D3D_DEVICE_PUT = p;
    d3d_drain();
}

DWORD *__stdcall d3d_D3DDevice_MakeSpace()
{
    D3DLock lock;
    d3d_count("D3DDevice_MakeSpace");
    return d3d_push_space(0x4000);
}

DWORD *__stdcall d3d_D3DDevice_BeginStateBig(DWORD count)
{
    D3DLock lock;
    d3d_count("D3DDevice_BeginStateBig");
    return d3d_push_space(count);
}

}
