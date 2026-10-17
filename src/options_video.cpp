// Options screen "Video" page next to the game's own settings. The retail OptionState (vtable 0x350614) list menu is
// reused: LB / RB (Q / W, PageUp / PageDown) switch pages; on the Video page left / right change our settings, which
// are saved to gladiusport.ini at once and applied live where possible (the rest say "restart").
#include "port.h"
#include "d3d.h"
#include "overlay.h"
#include "hw_detect.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

int port_player1_page_pressed();                        // xapi_input.cpp
int port_player1_lr_pressed();
int port_player1_ud_pressed();

static bool g_video;                                    // the Video page is showing
// The Options draw (0x127890) shows rows 0..2 as volume sliders; on the Video page its row counter starts at 3 so every
// row shows a value. The list shows VISIBLE_ROWS settings at a time; up / down scroll our own window.
enum { VISIBLE_ROWS = 6 };
static int g_top, g_cursor;                             // first setting shown, selected setting

static void draw_rows_from(int first) { *(int32_t *)0x127F57 = first; }
static void *g_screen;

static const char *ini_path()
{
    static char p[MAX_PATH];
    if (!p[0]) {
        GetModuleFileNameA(nullptr, p, MAX_PATH);
        strcpy(strrchr(p, 0x5C) + 1, port_ini_name());
    }
    return p;
}

struct Setting {
    const char *label, *key;
    int count;
    const char *names[5];
    const char *values[5];                              // ini values
    bool restart;
};
static const Setting k_set[] = {
    {"Anti-Aliasing", "AntiAlias", 3, {"Off", "FXAA", "TAA"}, {"off", "fxaa", "taa"}, false},
    {"Lighting", "Lighting", 2, {"Vertex", "Per-Pixel"}, {"vertex", "pixel"}, false},
    {"Shadows", "Shadows", 2, {"Off", "On"}, {"0", "1"}, false},
    {"Occlusion", "SSAO", 2, {"Off", "On"}, {"0", "1"}, false},
    {"Soft Particles", "SoftParticles", 2, {"Off", "On"}, {"0", "1"}, false},
    {"Bloom", "Bloom", 4, {"Off", "Low", "Medium", "High"}, {"0", "0.3", "0.6", "1.0"}, false},
    {"God Rays", "GodRays", 4, {"Off", "Low", "Medium", "High"}, {"0", "0.25", "0.5", "1.0"}, false},
    {"Effect Lights", "FxLights", 4, {"Off", "Low", "Medium", "High"}, {"0", "0.3", "0.6", "1.0"}, false},
    {"Materials", "Materials", 2, {"Off", "On"}, {"0", "1.0"}, false},
    {"Render Scale", "RenderScale", 5, {"Auto", "1x", "2x", "3x", "4x"}, {"auto", "1", "2", "3", "4"}, true},
    {"Shadow Detail", "ShadowMapSize", 3, {"Low", "Medium", "High"}, {"2048", "4096", "8192"}, true},
    {"HDR", "HDR", 2, {"Off", "On"}, {"0", "1"}, true},
    {"Resolution", "Resolution", 2, {"640x480", "Monitor"}, {"640x480", "auto"}, true},
    {"Screen Fit", "UiMode", 2, {"Fill", "Centered"}, {"fill", "center"}, true},
    {"UI Aspect", "UiAspect", 3, {"4:3", "16:9", "21:9"}, {"4:3", "16:9", "21:9"}, true},
    {"CPU Cores", "SingleCore", 2, {"Single", "Multi"}, {"1", "0"}, false},   // [cpu] section
};
enum { SET_CORES = 15 };
enum { NSET = sizeof k_set / sizeof k_set[0] };

// Presets (row 0: Low / Medium / High, Custom once changed by hand). The builder writes the recommended one at install;
// the same table is in builder\gladiusport_builder.py (PRESETS) - keep them equal.
enum { NROWS = NSET + 1, P_CUSTOM = 3 };
static const char *const k_tier_key[] = {"low", "medium", "high", "custom"};
static const char *const k_tier_name[] = {"Low", "Medium", "High", "Custom"};
struct PresetVal {
    const char *key;
    const char *v[3];                                   // low, medium, high
};
static const PresetVal k_preset[] = {
    {"AntiAlias", {"fxaa", "taa", "taa"}},     {"Lighting", {"vertex", "pixel", "pixel"}},
    {"Shadows", {"0", "1", "1"}},              {"SSAO", {"0", "1", "1"}},
    {"SoftParticles", {"1", "1", "1"}},        {"Bloom", {"0.3", "0.6", "1.0"}},
    {"GodRays", {"0", "0.5", "1.0"}},          {"FxLights", {"0.3", "0.6", "1.0"}},
    {"Materials", {"1.0", "1.0", "1.0"}},      {"RenderScale", {"1", "1", "1"}},
    {"ShadowMapSize", {"2048", "4096", "8192"}}, {"HDR", {"1", "1", "1"}},
    {"Resolution", {"auto", "auto", "auto"}},  {"SingleCore", {"0", "0", "0"}},
};

// help panel (overlay, 640x480 game coordinates): what the highlighted setting does and costs
struct Help {
    const char *text, *cpu, *gpu;
};
static const Help k_help[NSET] = {
    {"Smooths jagged edges. FXAA: fast filter. TAA: blends frames, smoothest, a little softer.", "None", "Low (FXAA) / Medium (TAA)"},
    {"Per-Pixel: the game's lights per pixel - specular, rim light, surface relief.", "None", "Medium"},
    {"Real sun shadows from the arena and fighters (replaces the round blob shadows).", "Medium", "Medium"},
    {"Ambient occlusion: soft contact shading in corners and under objects.", "None", "Medium"},
    {"Smoke, dust and spell effects fade where they meet walls and floors.", "None", "Low"},
    {"Glow around light brighter than white - spells, fire, glints.", "None", "Low"},
    {"Sun shafts through gaps when the sky's sun is on screen (needs Bloom).", "None", "Low"},
    {"Spells and fire light up nearby fighters and the arena.", "Low", "Low"},
    {"Water ripples, lava and brazier glow, swaying foliage and banners.", "None", "Low"},
    {"Internal resolution at 640x480 - higher is sharper. Locked to Native at Monitor resolution. *", "None", "High"},
    {"Shadow map size - higher gives sharper shadow edges. * after a restart.", "None", "Medium (VRAM)"},
    {"High dynamic range: bright glows roll off to white instead of clipping. * after a restart.", "None", "Low"},
    {"640x480: the original 4:3 frame. Monitor: the game at your monitor's shape. * after a restart.", "None", "Low"},
    {"Fill: the whole window. Centered: the game framed at UI Aspect with black bars (Monitor only). *", "None", "None"},
    {"Frame shape for Centered - 4:3 keeps the original look, 16:9 / 21:9 widescreen (Monitor only). *", "None", "None"},
    {"EXPERIMENTAL - Multi: game threads on all cores, smoother frame pacing. Please report any crash.",
     "Lower stutter (Multi)", "None"},
};

static bool g_help_shown;
static int preset_recommended();

static void draw_help()
{
    g_help_shown = true;
    char text[160], impact[96];
    if (g_cursor == 0) {                                // Preset row: what was detected
        const HwInfo &hw = port_hw_info();
        char gpu[48];
        _snprintf(gpu, sizeof gpu, "%s", hw.gpu);
        gpu[sizeof gpu - 1] = 0;
        _snprintf(text, sizeof text, "Recommended here: %s (%s, %u GB, %d cores / %d threads). Changes = Custom.",
                  k_tier_name[preset_recommended()], gpu, (hw.vram_mb + 512) / 1024, hw.cores, hw.threads);
        _snprintf(impact, sizeof impact, "Sets every option below at once (Shadow Detail / HDR after a restart)");
    } else {
        const Help &h = k_help[g_cursor - 1];
        _snprintf(text, sizeof text, "%s", h.text);
        _snprintf(impact, sizeof impact, "CPU impact: %s      GPU impact: %s", h.cpu, h.gpu);
    }
    text[sizeof text - 1] = 0;
    // word-wrap the description to the panel
    enum { MAX_LINES = 3 };
    const float max_w = 572;
    char lines[MAX_LINES][160];
    int n = 0;
    for (const char *p = text; *p && n < MAX_LINES; n++) {
        while (*p == ' ')
            p++;
        int fit = (int)strlen(p), last_space = -1;
        char buf[160];
        for (int k = 1; p[k - 1]; k++) {
            memcpy(buf, p, k);
            buf[k] = 0;
            if (p[k - 1] == ' ')
                last_space = k - 1;
            if (ov_text_width(buf, true) > max_w) {
                fit = last_space > 0 ? last_space : k - 1;
                break;
            }
        }
        if (n == MAX_LINES - 1)
            fit = (int)strlen(p);                       // last line: the rest
        memcpy(lines[n], p, fit);
        lines[n][fit] = 0;
        p += fit;
    }
    float h = 4 + 15.0f * (n + 1);
    ov_begin();
    ov_box(24, 4, 592, h, 0xB0000000);
    ov_frame(24, 4, 592, h, 1, 0x80C8A060);
    for (int i = 0; i < n; i++)
        ov_text(lines[i], 34, 14 + 15.0f * i, OV_LEFT, 0xFFF0E0C0, true);
    ov_text(impact, 34, 14 + 15.0f * n, OV_LEFT, 0xFFC8A060, true);
    ov_panel(true);
    ov_end(true);
}

static void hide_help()
{
    if (!g_help_shown)
        return;
    g_help_shown = false;
    ov_panel(false);
    ov_end(false);
}

// current choice: the live renderer state where there is one, else the ini value
static int current(int i)
{
    const Setting &s = k_set[i];
    float f = -1;
    switch (i) {
    case 0: return g_d3d.anti_alias;
    case 1: return g_d3d.pixel_lighting ? 1 : 0;
    case 2: return g_d3d.shadows ? 1 : 0;
    case 3: return g_d3d.ssao ? 1 : 0;
    case 4: return g_d3d.soft_particles ? 1 : 0;
    case 5: f = g_d3d.bloom; break;
    case 6: f = g_d3d.god_rays; break;
    case 7: f = g_d3d.fx_lights; break;
    case 8: return g_d3d.materials > 0 ? 1 : 0;
    }
    if (i == SET_CORES)                                 // [cpu] SingleCore (default 1)
        return GetPrivateProfileIntA("cpu", "SingleCore", 1, ini_path()) ? 0 : 1;
    if (i >= 12) {                                      // text settings: match the ini value (default = first)
        char v[32];
        GetPrivateProfileStringA("video", s.key, s.values[0], v, sizeof v, ini_path());
        for (int k = 0; k < s.count; k++)
            if (!_stricmp(v, s.values[k]))
                return k;
        return 0;
    }
    if (f < 0) {
        char v[32];
        GetPrivateProfileStringA("video", s.key, "", v, sizeof v, ini_path());
        if (i == 11 && !v[0])
            return 1;                                   // HDR defaults on
        if (i == 9)                                     // render scale: auto | 1..4 (missing = 1)
            return !_stricmp(v, "auto") ? 0 : v[0] ? (int)atof(v) < 1 ? 1 : (int)atof(v) > 4 ? 4 : (int)atof(v) : 1;
        f = (float)atof(v);
    }
    int best = 0;
    float bd = 1e30f;
    for (int k = 0; k < s.count; k++) {
        float d = fabsf((float)atof(s.values[k]) - f);
        if (d < bd)
            bd = d, best = k;
    }
    return best;
}

static bool monitor_res()                               // Resolution = monitor: render scale is native, locked
{
    char v[32];
    GetPrivateProfileStringA("video", "Resolution", "640x480", v, sizeof v, ini_path());
    return _stricmp(v, "640x480") != 0;
}

static void apply(int i, int c)
{
    const Setting &s = k_set[i];
    if (i == SET_CORES) {                               // [cpu]: applied to the running game threads at once
        WritePrivateProfileStringA("cpu", s.key, s.values[c], ini_path());
        port_set_single_core(c == 0);
        port_log("options: SingleCore = %s (now)\n", s.values[c]);
        return;
    }
    WritePrivateProfileStringA("video", s.key, s.values[c], ini_path());
    if (i == 12 && c == 1)                              // Monitor: render scale back to 1x (they would multiply)
        WritePrivateProfileStringA("video", "RenderScale", "1", ini_path());
    float f = (float)atof(s.values[c]);
    switch (i) {
    case 0: g_d3d.anti_alias = c; break;
    case 1: g_d3d.pixel_lighting = c == 1; break;
    case 2: g_d3d.shadows = c == 1; break;
    case 3: g_d3d.ssao = c == 1; break;
    case 4: g_d3d.soft_particles = c == 1; break;
    case 5: g_d3d.bloom = f; break;
    case 6: g_d3d.god_rays = f; break;
    case 7: g_d3d.fx_lights = f; break;
    case 8: g_d3d.materials = f; break;
    }
    port_log("options: %s = %s (%s)\n", s.key, s.values[c], s.restart ? "after a restart" : "now");
}

static int preset_current()                             // [video] Preset (missing = Custom)
{
    char v[16];
    GetPrivateProfileStringA("video", "Preset", "custom", v, sizeof v, ini_path());
    for (int t = 0; t < 3; t++)
        if (!_stricmp(v, k_tier_key[t]))
            return t;
    return P_CUSTOM;
}

static int preset_recommended()                         // [video] Recommended (the builder's detection), else ours
{
    char v[16];
    GetPrivateProfileStringA("video", "Recommended", "", v, sizeof v, ini_path());
    for (int t = 0; t < 3; t++)
        if (!_stricmp(v, k_tier_key[t]))
            return t;
    return port_hw_info().tier;
}

static void apply_preset(int t)
{
    const HwInfo &hw = port_hw_info();
    for (const PresetVal &p : k_preset) {
        const char *val = p.v[t];
        if (!strcmp(p.key, "Resolution") && t == HW_LOW && hw.screen_h > 1080)
            val = "640x480";                            // Low on a big monitor: the original frame
        for (int i = 0; i < NSET; i++)
            if (!strcmp(k_set[i].key, p.key))
                for (int c = 0; c < k_set[i].count; c++)
                    if (!strcmp(k_set[i].values[c], val) && c != current(i))
                        apply(i, c);
    }
    WritePrivateProfileStringA("video", "Preset", k_tier_key[t], ini_path());
    port_log("options: %s preset applied\n", k_tier_name[t]);
}

typedef void(__thiscall *ThisFn)(void *);
static void build_video(BYTE *self)
{
    ((ThisFn)0x125520)(self);                           // clear the list
    if (g_cursor < g_top) g_top = g_cursor;
    if (g_cursor >= g_top + VISIBLE_ROWS) g_top = g_cursor - VISIBLE_ROWS + 1;
    for (int r = g_top; r < NROWS && r < g_top + VISIBLE_ROWS; r++) {
        char label[64];
        if (r == 0) {                                   // Preset row
            BYTE *item = ((BYTE * (__thiscall *)(void *, const char *, const char *, const char *))0x125660)(
                self, "Preset", "Preset", "");
            if (item && item[8] > 1) {
                int t = preset_current();
                char val[32];
                _snprintf(val, sizeof val, "%s%s", k_tier_name[t], t == preset_recommended() ? " (Rec)" : "");
                ((void(__thiscall *)(void *, const char *))0x13E10)(*(BYTE **)(item + 0x14) + 4, val);
            }
            continue;
        }
        int i = r - 1;
        const Setting &s = k_set[i];
        // short labels: * = applies after a restart
        _snprintf(label, sizeof label, "%s%s%s", r == g_top && g_top > 0 ? "^ " : "", s.label, s.restart ? "*" : "");
        if (r == g_top + VISIBLE_ROWS - 1 && r < NROWS - 1)
            strncat(label, " v", sizeof label - strlen(label) - 1);
        BYTE *item = ((BYTE * (__thiscall *)(void *, const char *, const char *, const char *))0x125660)(
            self, s.label, label, "");
        if (item && item[8] > 1) {
            int c = current(i);
            char val[32];
            const char *shown = s.names[c];
            if (i == 9 && monitor_res()) {
                shown = "Native";                       // locked while Resolution is Monitor
            } else if (i == 9 && c > 0) {               // render scale: the scene's real height too ("1440P 3X")
                extern UINT g_game_res_h;
                UINT ssy = g_d3d.game_ss_y ? g_d3d.game_ss_y : 1;   // the game's own vertical supersampling
                _snprintf(val, sizeof val, "%uP %dX", (g_game_res_h ? g_game_res_h : 480) * ssy * c, c);
                shown = val;
            }
            ((void(__thiscall *)(void *, const char *))0x13E10)(*(BYTE **)(item + 0x14) + 4, shown);
        }
    }
    ((void(__thiscall *)(void *, int))0x125940)(self, 0);   // the retail rebuild's tail
    BYTE *lay = self + 0x4C;
    DWORD keep = *(DWORD *)(self + 0x60);
    (*(void(__thiscall **)(void *, DWORD))(*(BYTE **)lay + 4))(lay, *(DWORD *)(lay + 0x10));
    (*(void(__thiscall **)(void *, DWORD))(*(BYTE **)lay + 8))(lay, keep);
    *(DWORD *)(self + 0x98) = *(DWORD *)(self + 0x9C);
    BYTE *it = *(BYTE **)(self + 0x10);                 // select the cursor's row (list +0x10, next at +0)
    for (int r = g_top; it && r < g_cursor; r++)
        it = *(BYTE **)it;
    if (it)
        (*(void(__thiscall **)(void *, void *))(*(BYTE **)self + 0x18))(self, it);
}

static void __fastcall opt_refresh(BYTE *self)
{
    if (g_video && self == g_screen) {
        draw_rows_from(3);
        build_video(self);
    } else {
        draw_rows_from(0);
        ((ThisFn)0x132D50)(self);
    }
}

static bool pressed(int pad, DWORD mask)
{
    return ((int(__thiscall *)(void *, int, DWORD, int))0x74A40)((void *)0x3AD658, pad, mask, 1) != 0;
}

static void __fastcall opt_update(BYTE *self, void *edx, int pad)
{
    (void)edx;
    if (self != g_screen) {                             // a new Options screen: the game's page first
        g_screen = self, g_video = false, g_top = g_cursor = 0;
        draw_rows_from(0);
    }
    if (int d = port_player1_page_pressed()) {
        bool want = d > 0;
        if (want != g_video) {
            g_video = want;
            port_log("options: %s page\n", g_video ? "video" : "game");
            (*(ThisFn *)(*(BYTE **)self + 0x10))(self);
        }
    }
    if (!g_video || pressed(pad, 0x1080) || pressed(pad, 0x2020)) {   // game page, or leaving (B / A)
        hide_help();
        if (g_video) {
            g_video = false;
            (*(ThisFn *)(*(BYTE **)self + 0x10))(self);
        }
        ((void(__thiscall *)(void *, int))0x126FD0)(self, pad);
        return;
    }
    draw_help();
    if (int ud = port_player1_ud_pressed()) {           // up / down: our cursor, the window follows
        int c = g_cursor + ud;
        c = c < 0 ? 0 : c >= NROWS ? NROWS - 1 : c;
        if (c != g_cursor) {
            g_cursor = c;
            build_video(self);
        }
    }
    if (int lr = port_player1_lr_pressed()) {
        if (g_cursor == 0) {                            // Preset: Low / Medium / High (from Custom: the recommended)
            int t = preset_current();
            int n = t == P_CUSTOM ? preset_recommended() : t + lr;
            n = n < 0 ? 0 : n > 2 ? 2 : n;
            if (n != t) {
                apply_preset(n);
                build_video(self);
            }
            return;
        }
        int i = g_cursor - 1;
        if (i == 9 && monitor_res())                    // render scale is locked to native at Monitor resolution
            return;
        int c = current(i) + lr;
        c = c < 0 ? 0 : c >= k_set[i].count ? k_set[i].count - 1 : c;
        if (c != current(i)) {
            apply(i, c);
            if (preset_current() != P_CUSTOM)           // hand-tuned from here on
                WritePrivateProfileStringA("video", "Preset", "custom", ini_path());
            build_video(self);
        }
    }
}

void port_options_video_init()
{
    DWORD *vt = (DWORD *)0x350614;
    if (vt[4] != 0x132D50 || vt[12] != 0x126FD0 || vt[5] != 0x131840) {
        port_log("options: Options screen code unexpected - Video page not installed\n");
        return;
    }
    DWORD old;
    VirtualProtect(vt, 0x34, PAGE_EXECUTE_READWRITE, &old);
    vt[4] = (DWORD)opt_refresh;
    vt[12] = (DWORD)opt_update;
    VirtualProtect(vt, 0x34, old, &old);
    port_log("options: Video page installed (LB / RB on the Options screen)\n");
}
