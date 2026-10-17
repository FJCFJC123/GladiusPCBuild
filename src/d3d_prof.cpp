// Renderer profiler (see d3d_prof.h).
#include "d3d.h"
#include "d3d_prof.h"
#include <stdio.h>
#include <string.h>

bool g_prof_on;
unsigned long long g_prof_ticks[PZ_COUNT];
unsigned long long g_prof_count[PC_COUNT];

static const char *const ZONE_NAMES[PZ_COUNT] = {"shim", "drain", "index", "vbhash", "vbupload", "transient", "const",
                                                 "texture", "texupload", "state", "lookup", "compile", "submit",
                                                 "fixed", "present", "netwait", "pace", "program", "light",
                                                 "shrec", "shapply", "post"};
static const char *const COUNTER_NAMES[PC_COUNT] = {"draw_idx", "draw_up", "draw_inline", "draw_fixed", "indices",
                                                    "vb_hashed", "vb_uploaded", "transient_b", "cb_uploads",
                                                    "tex_checks", "tex_uploads", "tex_bytes", "state_changes",
                                                    "compiles", "vb_len_uploads"};
static double g_tsc_ms;                 // milliseconds per TSC tick
static FILE *g_csv;
static int g_lock_depth;
static unsigned long long g_lock_t0;

// ------------------------------------------------------------------------------------------------ GPU timing
// Per frame: a disjoint query and a list of timestamps; segment i runs from ts[i] to ts[i + 1] and is "blended"
// or "opaque" by the draws in it. Results are read a few frames later (no stall): the CSV row of frame N carries
// the GPU numbers of the newest finished frame (gpu_frame column).
static const int GPU_RING = 8, GPU_SEGS = 256;
struct GpuFrame {
    ID3D11Query *disjoint, *ts[GPU_SEGS + 1];
    BYTE blended[GPU_SEGS];             // segment pass (GpuPass)
    int n;                              // segments started (timestamps issued = n, + 1 closing at frame end)
    DWORD frame;
    bool open, pending;
};
static GpuFrame g_gpu[GPU_RING];
static int g_gpu_cur = -1, g_gpu_label = -1;
static double g_gpu_ms, g_gpu_opaque_ms, g_gpu_blend_ms, g_gpu_pass_ms[GP_COUNT];
static const char *const PASS_NAMES[GP_COUNT] = {"opaque", "blend", "fx", "shadow", "ssao", "taa", "bloom", "fxaa",
                                                 "final", "overlay"};
static DWORD g_gpu_done_frame;
static int g_gpu_segs;

static ID3D11Query *gpu_ts(GpuFrame &f, int i)
{
    if (!f.ts[i]) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0};
        g_d3d.dev->CreateQuery(&qd, &f.ts[i]);
    }
    return f.ts[i];
}

static void gpu_collect()
{
    for (int k = 0; k < GPU_RING; k++) {
        GpuFrame &f = g_gpu[k];
        if (!f.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (g_d3d.ctx->GetData(f.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        f.pending = false;
        if (dj.Disjoint || !dj.Frequency || f.frame < g_gpu_done_frame)
            continue;
        UINT64 t[GPU_SEGS + 1];
        bool ok = true;
        for (int i = 0; i <= f.n && ok; i++)
            ok = g_d3d.ctx->GetData(f.ts[i], &t[i], sizeof t[i], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok)
            continue;
        double o = 0, b = 0, p[GP_COUNT] = {};
        for (int i = 0; i < f.n; i++) {
            double ms = (double)(t[i + 1] - t[i]) * 1000.0 / dj.Frequency;
            (f.blended[i] == GP_OPAQUE ? o : b) += ms;
            if (f.blended[i] < GP_COUNT)
                p[f.blended[i]] += ms;
        }
        memcpy(g_gpu_pass_ms, p, sizeof p);
        g_gpu_ms = (double)(t[f.n] - t[0]) * 1000.0 / dj.Frequency;
        g_gpu_opaque_ms = o, g_gpu_blend_ms = b;
        g_gpu_done_frame = f.frame;
        g_gpu_segs = f.n;
    }
}

void prof_frame_begin_gpu()
{
    if (!g_prof_on || !g_d3d.dev)
        return;
    int k = (int)(g_d3d.frame % GPU_RING);
    GpuFrame &f = g_gpu[k];
    if (f.pending)                                      // ring full of unread frames (GPU far behind): skip
        return;
    if (!f.disjoint) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        g_d3d.dev->CreateQuery(&qd, &f.disjoint);
    }
    g_d3d.ctx->Begin(f.disjoint);
    f.n = 0;
    f.frame = g_d3d.frame;
    f.open = true;
    g_gpu_cur = k;
    g_gpu_label = -1;
    prof_gpu_segment(false);                            // frame start (clears count as opaque work)
}

void prof_gpu_pass(int pass)
{
    if (!g_prof_on || g_gpu_cur < 0)
        return;
    GpuFrame &f = g_gpu[g_gpu_cur];
    if (!f.open || pass == g_gpu_label || f.n >= GPU_SEGS)
        return;
    g_d3d.ctx->End(gpu_ts(f, f.n));                    // start of segment n
    f.blended[f.n++] = (BYTE)pass;
    g_gpu_label = pass;
}

void prof_gpu_segment(bool blended)
{
    prof_gpu_pass(g_d3d.in_fx ? GP_FX : blended ? GP_BLEND : GP_OPAQUE);
}

static void gpu_frame_close()
{
    if (g_gpu_cur < 0)
        return;
    GpuFrame &f = g_gpu[g_gpu_cur];
    g_gpu_cur = -1;
    if (!f.open)
        return;
    f.open = false;
    if (!f.n) {                                        // no draws: still close the disjoint query
        g_d3d.ctx->End(gpu_ts(f, 0));
        f.blended[0] = 0;
        f.n = 0;
    }
    g_d3d.ctx->End(gpu_ts(f, f.n));                    // end of the last segment
    g_d3d.ctx->End(f.disjoint);
    f.pending = f.n > 0;
}

// ------------------------------------------------------------------------------------------------ CPU
void prof_lock_enter()
{
    if (g_lock_depth++ == 0 && g_prof_on)
        g_lock_t0 = __rdtsc();
}
void prof_lock_leave()
{
    if (--g_lock_depth == 0 && g_prof_on && g_lock_t0) {
        g_prof_ticks[PZ_SHIM] += __rdtsc() - g_lock_t0;
        g_lock_t0 = 0;
    }
}

void prof_init()
{
    static bool done;
    if (done)
        return;
    done = true;
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    g_prof_on = GetPrivateProfileIntA("debug", "Profile", 0, ini) != 0 || strstr(GetCommandLineA(), "-gnprof");
    if (!g_prof_on)
        return;
    LARGE_INTEGER f, q0, q1;                            // TSC rate against QPC over 50 ms
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    unsigned long long t0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&q1);
    unsigned long long t1 = __rdtsc();
    g_tsc_ms = ((q1.QuadPart - q0.QuadPart) * 1000.0 / f.QuadPart) / (double)(t1 - t0);
    const char *pad = strstr(GetCommandLineA(), "-gnpad=");
    int inst = pad ? atoi(pad + 7) : 0;
    char *slash = strrchr(ini, 0x5C);
    if (inst > 0) _snprintf(slash + 1, 32, "perf_pad%d.csv", inst);
    else strcpy(slash + 1, "perf.csv");
    g_csv = fopen(ini, "w");
    if (g_csv) {
        setvbuf(g_csv, nullptr, _IOFBF, 1 << 16);
        fprintf(g_csv, "frame,wall_ms,game_ms");
        for (int z = 0; z < PZ_COUNT; z++) fprintf(g_csv, ",%s_ms", ZONE_NAMES[z]);
        fprintf(g_csv, ",gpu_frame,gpu_ms,gpu_opaque_ms,gpu_blend_ms,gpu_segments");
        for (int c = 0; c < PC_COUNT; c++) fprintf(g_csv, ",%s", COUNTER_NAMES[c]);
        fprintf(g_csv, ",state,kero_phase,clock");
        for (int p = 0; p < GP_COUNT; p++) fprintf(g_csv, ",gpu_%s_ms", PASS_NAMES[p]);
        fprintf(g_csv, ",scene_w,scene_h\n");
    }
    port_log("prof: renderer profiler on (%.3f MHz TSC) -> %s\n", 1.0 / g_tsc_ms / 1000.0, slash + 1);
}

// rolling one-second summary for port.log
struct Sum {
    double wall, wall_max, game, shim, present, gpu, gpu_max, gpu_blend, z[PZ_COUNT], pass[GP_COUNT];
    unsigned long long c[PC_COUNT];
    DWORD frames, slow;                                 // slow = wall > 17.5 ms
};
static Sum g_sum;
static DWORD g_sum_tick;

void prof_frame_end(double wall_ms)
{
    if (!g_prof_on)
        return;
    gpu_frame_close();
    gpu_collect();
    if (g_lock_depth && g_lock_t0) {                    // Swap holds the lock around Present: count it up to here
        unsigned long long now = __rdtsc();
        g_prof_ticks[PZ_SHIM] += now - g_lock_t0;
        g_lock_t0 = now;
    }
    double z[PZ_COUNT];
    for (int i = 0; i < PZ_COUNT; i++)
        z[i] = g_prof_ticks[i] * g_tsc_ms;
    z[PZ_SHIM] = z[PZ_SHIM] > z[PZ_PRESENT] ? z[PZ_SHIM] - z[PZ_PRESENT] : 0;   // Present runs inside Swap's lock
    double game = wall_ms - z[PZ_SHIM] - z[PZ_PRESENT] - z[PZ_NETWAIT] - z[PZ_PACE];
    const BYTE *state = *(const BYTE *const *)(0x38E448 + 0x14);
    int sid = state ? *(const int *)(state + 8) : -1;
    const BYTE *kero = sid == 12 ? *(const BYTE *const *)0x3D2970 : nullptr;
    if (g_csv) {
        fprintf(g_csv, "%u,%.3f,%.3f", g_d3d.frame, wall_ms, game);
        for (int i = 0; i < PZ_COUNT; i++) fprintf(g_csv, ",%.3f", z[i]);
        fprintf(g_csv, ",%u,%.3f,%.3f,%.3f,%d", g_gpu_done_frame, g_gpu_ms, g_gpu_opaque_ms, g_gpu_blend_ms, g_gpu_segs);
        for (int c = 0; c < PC_COUNT; c++) fprintf(g_csv, ",%llu", g_prof_count[c]);
        SYSTEMTIME st;
        GetLocalTime(&st);                              // lines up with what a watcher calls out
        fprintf(g_csv, ",%d,%d,%02d:%02d:%02d.%03d", sid, kero ? kero[0x2C] : -1, st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
        for (int p = 0; p < GP_COUNT; p++) fprintf(g_csv, ",%.3f", g_gpu_pass_ms[p]);
        fprintf(g_csv, ",%u,%u\n", g_d3d.scene_width, g_d3d.scene_height);
    }
    Sum &s = g_sum;
    s.frames++;
    s.wall += wall_ms, s.game += game, s.shim += z[PZ_SHIM], s.present += z[PZ_PRESENT];
    s.wall_max = wall_ms > s.wall_max ? wall_ms : s.wall_max;
    s.gpu += g_gpu_ms, s.gpu_blend += g_gpu_blend_ms;
    s.gpu_max = g_gpu_ms > s.gpu_max ? g_gpu_ms : s.gpu_max;
    if (wall_ms > 17.5) s.slow++;
    for (int i = 0; i < PZ_COUNT; i++) s.z[i] += z[i];
    for (int p = 0; p < GP_COUNT; p++) s.pass[p] += g_gpu_pass_ms[p];
    for (int c = 0; c < PC_COUNT; c++) s.c[c] += g_prof_count[c];
    DWORD now = GetTickCount();
    if (now - g_sum_tick >= 1000 && s.frames) {
        double n = s.frames;
        port_log("prof: %u fr (%u slow) wall %.1f/%.1f | game %.1f shim %.1f present %.1f | gpu %.1f/%.1f (blend %.1f) "
                 "| drain %.2f idx %.2f vbhash %.2f vbup %.2f trans %.2f const %.2f tex %.2f state %.2f lookup %.2f "
                 "submit %.2f fixed %.2f net %.1f pace %.1f | draws %.0f idx %.0fk vbhash %.1fMB tex up %llu\n",
                 s.frames, s.slow, s.wall / n, s.wall_max, s.game / n, s.shim / n, s.present / n, s.gpu / n, s.gpu_max,
                 s.gpu_blend / n, s.z[PZ_DRAIN] / n, s.z[PZ_INDEX] / n, s.z[PZ_VBHASH] / n, s.z[PZ_VBUPLOAD] / n,
                 s.z[PZ_TRANSIENT] / n, s.z[PZ_CONST] / n, s.z[PZ_TEXTURE] / n, s.z[PZ_STATE] / n, s.z[PZ_LOOKUP] / n,
                 s.z[PZ_SUBMIT] / n, s.z[PZ_FIXED] / n, s.z[PZ_NETWAIT] / n, s.z[PZ_PACE] / n,
                 (s.c[PC_DRAW_INDEXED] + s.c[PC_DRAW_UP] + s.c[PC_DRAW_INLINE] + s.c[PC_DRAW_FIXED]) / n,
                 s.c[PC_INDICES] / n / 1000.0, s.c[PC_VB_HASHED] / n / 1048576.0, s.c[PC_TEX_UPLOADS]);
        char pb[256];
        int pl = 0;
        for (int p = 0; p < GP_COUNT; p++)
            pl += _snprintf(pb + pl, sizeof pb - pl, " %s %.2f", PASS_NAMES[p], s.pass[p] / n);
        port_log("prof: gpu passes (ms):%s | scene %ux%u\n", pb, g_d3d.scene_width, g_d3d.scene_height);
        memset(&s, 0, sizeof s);
        g_sum_tick = now;
        fflush(g_csv);
    }
    memset(g_prof_ticks, 0, sizeof g_prof_ticks);
    memset(g_prof_count, 0, sizeof g_prof_count);
}
