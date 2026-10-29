// Renderer profiler ([debug] Profile=1 or -gnprof): per-frame CPU time per shim stage, counters, and GPU time per
// pass from timestamp queries. One row per frame in build\perf.csv; port.log gets a "prof:" summary each second.
#pragma once
#include <windows.h>
#include <intrin.h>

enum ProfZone {
    PZ_SHIM,        // inside any D3D entry point (top-level D3D lock hold) - includes all zones below but PRESENT
    PZ_DRAIN,       // push-buffer decode (engine-written NV2A methods)
    PZ_INDEX,       // primitive expansion / index remap (CPU)
    PZ_VBHASH,      // vertex buffer chunk verification (hashing game memory)
    PZ_VBUPLOAD,    // vertex buffer chunk uploads (UpdateSubresource)
    PZ_TRANSIENT,   // UP / inline vertices + expanded indices into the ring buffers
    PZ_CONST,       // shader constant assembly + constant buffer uploads
    PZ_TEXTURE,     // texture lookup + content check (includes PZ_TEXUPLOAD)
    PZ_TEXUPLOAD,   // texture (re)uploads: unswizzle / decode / CreateTexture
    PZ_STATE,       // targets, blend / depth / raster state, samplers
    PZ_LOOKUP,      // shader / input layout cache lookups (includes PZ_COMPILE)
    PZ_COMPILE,     // HLSL translation + D3DCompile + Create*Shader / CreateInputLayout
    PZ_SUBMIT,      // IA / VS / PS binds + the Draw call itself
    PZ_FIXED,       // fixed-function (FVF) draws, whole path
    PZ_PRESENT,     // swap chain Present (vsync / GPU backpressure)
    PZ_NETWAIT,     // netplay: waiting for other players' inputs
    PZ_PACE,        // netplay: holding the session at 60 Hz
    PZ_PROGRAM,     // draw_program, whole (program draws: includes most zones above)
    PZ_LIGHT,       // per-draw lighting / shadow-lookup matrix math (pixel lighting)
    PZ_SHREC,       // shadow caster recording + texture name lookup
    PZ_SHAPPLY,     // shadow map render + composite (CPU side)
    PZ_POST,        // SSAO / TAA / bloom / final pass (CPU side)
    PZ_COUNT
};
enum ProfCounter {
    PC_DRAW_INDEXED, PC_DRAW_UP, PC_DRAW_INLINE, PC_DRAW_FIXED,
    PC_INDICES,         // indices produced by expansion
    PC_VB_HASHED,       // bytes of vertex buffer memory hashed
    PC_VB_UPLOADED,     // bytes of vertex buffer chunks uploaded
    PC_TRANSIENT,       // bytes into the ring buffers
    PC_CB_UPLOADS,      // constant buffer Maps
    PC_TEX_CHECKS,      // texture content samples
    PC_TEX_UPLOADS, PC_TEX_BYTES,
    PC_STATE_CHANGES,   // blend / depth / raster / scissor objects (re)bound
    PC_COMPILES,
    PC_VB_LEN_UPLOADS,  // vertex chunk uploads whose bytes were unchanged (only the verified length differed)
    PC_COUNT
};

// GPU passes: game draws (opaque / blended / 3D effects) and each port pass get their own GPU segment.
enum GpuPass { GP_OPAQUE, GP_BLEND, GP_FX, GP_SHADOW, GP_SSAO, GP_TAA, GP_BLOOM, GP_FXAA, GP_FINAL, GP_OVERLAY, GP_COUNT };
extern GP_DATA bool g_prof_on;
extern GP_DATA unsigned long long g_prof_ticks[PZ_COUNT];
extern unsigned long long g_prof_count[PC_COUNT];

struct ProfScope {
    int z;
    unsigned long long t;
    explicit ProfScope(int zone) : z(zone), t(g_prof_on ? __rdtsc() : 0) {}
    ~ProfScope() { stop(); }
    void stop() { if (t) g_prof_ticks[z] += __rdtsc() - t, t = 0; }   // end the zone before the scope does
};
inline void prof_count(int c, unsigned long long n = 1) { if (g_prof_on) g_prof_count[c] += n; }
inline void prof_add(int z, unsigned long long ticks) { if (g_prof_on) g_prof_ticks[z] += ticks; }

void prof_init();                       // reads the switch (call once, early)
// Live profiling for the devtools graphs: on without [debug] Profile (no perf.csv / port.log lines), applied at the
// next frame end on the game thread.
extern bool g_prof_report;              // [debug] Profile / -gnprof: perf.csv + port.log summaries + thread stats
void prof_set_live(bool on);
// The last finished frame (game thread writes at Present; readers copy it)
struct ProfLast {
    DWORD frame;
    float wall_ms, cpu_ms, present_ms;  // always: Present to Present, game + submission, blocked in Present
    DWORD draws;
    bool prof;                          // the profiler fields below are valid
    float game_ms, zone_ms[PZ_COUNT], gpu_ms, gpu_pass_ms[GP_COUNT];
    DWORD gpu_frame;
    unsigned counters[PC_COUNT];
};
extern ProfLast g_prof_last;
const char *prof_zone_name(int z);
const char *prof_pass_name(int p);
const char *prof_counter_name(int c);
void prof_frame_begin_gpu();            // after Present: open this frame's GPU timing
void prof_gpu_segment(bool blended);    // before each draw: start a new GPU segment when the class changes
void prof_gpu_pass(int pass);            // a port pass starts (the next game draw ends it)
void prof_frame_close_gpu();            // just before Present: the frame's closing GPU timestamp
void prof_frame_end(double wall_ms);    // at Present: close GPU timing, write the row, roll the counters
void prof_lock_enter();                 // D3D lock depth 0 -> 1 (PZ_SHIM)
void prof_lock_leave();
