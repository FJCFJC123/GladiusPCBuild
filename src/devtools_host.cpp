// Developer tools ([devtools] Enabled=1): loads devtools.dll (src/devtools) and feeds it the game window's messages,
// the per-frame game-thread hook and every port.log line.
#include "port.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "overlay.h"
#include "d3d.h"
#include "d3d_prof.h"
#include "container.h"
#include "gladiusnet_api.h"
#include "devtools/devtools_api.h"

static const DevToolsApi *g_dt;
void (*port_log_tap)(const char *line);                 // port_main.cpp: every port.log line

static void __cdecl dt_log(const char *fmt, ...)
{
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    port_log("%s", buf);
}

static bool __cdecl dt_net_running() { return gn_net_running(); }
static HWND __cdecl dt_window() { return g_d3d.hwnd; }
static void __cdecl dt_ov_begin() { ov_begin(); }
static void __cdecl dt_ov_box(float x, float y, float w, float h, uint32_t c) { ov_box(x, y, w, h, c); }
static void __cdecl dt_ov_text(const char *s, float x, float y, int al, uint32_t c, bool sm) { ov_text(s, x, y, al, c, sm); }
static float __cdecl dt_ov_text_width(const char *s, bool sm) { return ov_text_width(s, sm); }
static void __cdecl dt_ov_end(bool v) { ov_end(v); }
static void __cdecl dt_ov_panel(bool on) { ov_panel(on); }


static void __cdecl dt_frame_stats(DevFrameStats *o)
{
    const ProfLast &L = g_prof_last;
    memset(o, 0, sizeof *o);
    o->frame = L.frame, o->wall_ms = L.wall_ms, o->cpu_ms = L.cpu_ms, o->present_ms = L.present_ms, o->draws = L.draws;
    o->prof = g_prof_on && L.prof;
    o->prof_ini = g_prof_report;
    o->n_zones = PZ_COUNT < DEV_ZONES ? PZ_COUNT : DEV_ZONES;
    o->n_passes = GP_COUNT < DEV_PASSES ? GP_COUNT : DEV_PASSES;
    o->n_counters = PC_COUNT < DEV_COUNTERS ? PC_COUNT : DEV_COUNTERS;
    if (!o->prof) return;
    o->game_ms = L.game_ms, o->gpu_ms = L.gpu_ms, o->gpu_frame = L.gpu_frame;
    memcpy(o->zone_ms, L.zone_ms, o->n_zones * sizeof(float));
    memcpy(o->gpu_pass_ms, L.gpu_pass_ms, o->n_passes * sizeof(float));
    memcpy(o->counters, L.counters, o->n_counters * sizeof(uint32_t));
}

static void __cdecl dt_prof_live(bool on) { prof_set_live(on); }

static const char *__cdecl dt_prof_name(int kind, int i)
{
    return kind == 0 ? prof_zone_name(i) : kind == 1 ? prof_pass_name(i) : prof_counter_name(i);
}

static bool __cdecl dt_arena(int i, DevArena *o)
{
    size_t size, used, hwm;
    if (!container_usage(i, &o->name, &size, &used, &hwm, &o->failed)) return false;
    o->size = (uint32_t)size, o->used = (uint32_t)used, o->hwm = (uint32_t)hwm;
    return true;
}

static void __cdecl dt_audio_pause(bool on) { dsound_sync_pause(on); }

extern "C" volatile DWORD g_cri_owner, g_d3d_owner;    // cri_lock.cpp, d3d_core.cpp

static bool __cdecl dt_locks_held()
{
    DWORD me = GetCurrentThreadId();
    return g_cri_owner == me || g_d3d_owner == me;
}

bool port_read_game_file(const std::string &key, std::string &out);   // bec_read.cpp
const char *port_game_dir();                            // xapi_file.cpp

static void *__cdecl dt_read_game_file(const char *key, uint32_t *size)
{
    std::string d;
    *size = 0;
    if (!port_read_game_file(key, d)) return NULL;
    void *p = malloc(d.size() ? d.size() : 1);
    if (p) memcpy(p, d.data(), d.size()), *size = (uint32_t)d.size();
    return p;
}
static void __cdecl dt_free_game_file(void *p) { free(p); }

static char g_ini[MAX_PATH];
static DevHost g_host = {DEVTOOLS_ABI, g_ini, dt_log, dt_net_running, dt_window, dt_ov_begin, dt_ov_box, dt_ov_text,
                         dt_ov_text_width, dt_ov_end, dt_ov_panel, dt_frame_stats, dt_prof_live, dt_prof_name,
                         dt_arena, dt_audio_pause, dt_locks_held, NULL, dt_read_game_file, dt_free_game_file,
                         PORT_RELEASE != 0};

void port_devtools_init(const char *ini)
{
    if (!GetPrivateProfileIntA("devtools", "Enabled", 0, ini)) return;
    strncpy(g_ini, ini, MAX_PATH - 1);
    g_host.game_dir = port_game_dir();
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "devtools.dll");
    HMODULE m = LoadLibraryA(path);
    DevToolsEntry entry = m ? (DevToolsEntry)GetProcAddress(m, "devtools_api") : NULL;
    g_dt = entry ? entry(&g_host) : NULL;
    if (!g_dt) {
        port_log("devtools: devtools.dll %s - developer tools off\n", m ? "is for another version" : "not found");
        if (m) FreeLibrary(m);
        return;
    }
    port_log_tap = g_dt->log_line;
}

// Online lobby hosted / joined: the developer tools shut down for the rest of the run (memory readers and writers
// have no place next to an online match).
void port_devtools_frame()
{
    if (!g_dt) return;
    if (gn_net_in_match()) {
        const DevToolsApi *dt = g_dt;
        g_dt = NULL;                                    // wndproc / keyboard / crash calls stop first
        port_log_tap = NULL;
        dt->shutdown();
        port_log("devtools: off - online lobby (the developer tools stay off until the game restarts)\n");
        return;
    }
    g_dt->frame();
}

bool port_devtools_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return g_dt && g_dt->wndproc(h, msg, wp, lp);
}

bool port_devtools_keyboard_captured() { return g_dt && g_dt->keyboard_captured(); }

// port_main.cpp write_dump: the readable crash report (devtools crash.cpp), guarded - the process is already failing
bool port_devtools_crash(EXCEPTION_POINTERS *ep, const char *path, const char *why)
{
    if (!g_dt || !g_dt->crash_report) return false;
    __try {
        g_dt->crash_report(ep, path, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
