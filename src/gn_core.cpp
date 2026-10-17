// Deterministic-mode core (the online versus itself is gladiusnet.dll). Each frame gn_hooks.cpp samples the controllers
// before the engine's pad poll; gladiusnet.dll or the record / replay harness may replace them.
// Harness: Ctrl+F9 start / stop recording -> gladiusnet_rec.bin, Ctrl+F10 replay it.
// In deterministic mode: fixed time deltas, fixed AI iterations, no sub-frame real time, every stream read finished
// before the frame goes on, frames paced to 60 Hz. The gameplay RNG seed (0x4CA344) is the per-frame sync checksum.

#include "gn.h"
#include "gladiusnet_api.h"
#include <mmsystem.h>
#include <stdarg.h>
#include <string.h>
#include "d3d_prof.h"

static const uint32_t REC_MAGIC = 0x31585247;   // "GRX1" (Xbox pad layout)
static const uint32_t START_SEED = 0x1234567;

#pragma pack(push, 1)
struct RecHeader {
    uint32_t magic, padBytes, frames;
    float fixedDt;
    uint32_t randseed;
    float timeClock;
    int32_t stateId;
};
struct RecFrameTail {               // follows padBytes of pad data
    uint32_t randseed;
    int32_t stateId;
};
#pragma pack(pop)

enum Mode { MODE_OFF, MODE_RECORD, MODE_REPLAY };

char g_dir[MAX_PATH];
uint32_t g_frame = 0;

static Mode g_mode = MODE_OFF;
static FILE *g_rec = NULL;
static RecHeader g_hdr;
static uint32_t g_mismatches = 0;
static int32_t g_saveOverride = 0;
static float g_saveDelta = 0.0f;
static LARGE_INTEGER g_qpf, g_lastFrame;
static DWORD g_mainThread = 0;
static bool g_keyF9 = false, g_keyF10 = false;

void path_in_dir(char *out, const char *name)
{
    _snprintf(out, MAX_PATH, "%s%s", g_dir, name);
    out[MAX_PATH - 1] = 0;
}

void logf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    char p[MAX_PATH];
    path_in_dir(p, "gladiusnet.log");
    FILE *f = fopen(p, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] %5u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentProcessId(), line);
        fclose(f);
    }
    if (const GnNetApi *n = gn_net()) n->remote_log(line);  // a client forwards diagnostics to the host
}

// Session trace gladiusnet_trace_{host,client}.txt: one line per frame (seed, state) plus I/O events; diff the two
// files to find where a desync started.
static FILE *g_dtrace;
static CRITICAL_SECTION g_dtraceCs;
volatile uint32_t g_dtraceFrame;
// repeated lines collapse into "(xN)", and the file stops growing at TRACE_MAX bytes
static const long TRACE_MAX = 512L << 20;
static char g_dtraceLast[256];
static uint32_t g_dtraceRepeat;
static bool g_dtraceFull;

void trace_open(GnCtx *, const char *name)
{
    static bool init;
    if (!init) { InitializeCriticalSection(&g_dtraceCs); init = true; }
    char p[MAX_PATH];
    path_in_dir(p, name);
    EnterCriticalSection(&g_dtraceCs);
    if (g_dtrace) fclose(g_dtrace);
    g_dtrace = fopen(p, "w");
    if (g_dtrace) setvbuf(g_dtrace, NULL, _IOFBF, 1 << 16);
    g_dtraceFrame = 0;
    g_dtraceFull = false;
    g_dtraceLast[0] = 0;
    g_dtraceRepeat = 0;
    LeaveCriticalSection(&g_dtraceCs);
}

void trace_close(GnCtx *)
{
    if (!g_dtrace) return;
    EnterCriticalSection(&g_dtraceCs);
    fclose(g_dtrace);
    g_dtrace = NULL;
    LeaveCriticalSection(&g_dtraceCs);
}

void dtrace(const char *fmt, ...)
{
    if (!g_dtrace || g_dtraceFull) return;
    EnterCriticalSection(&g_dtraceCs);
    if (g_dtrace && !g_dtraceFull) {
        char line[256];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf(line, sizeof line - 1, fmt, ap);
        va_end(ap);
        line[sizeof line - 1] = 0;
        static DWORD lastFlush;                         // also on a timer: a stuck loop repeating one line
        if (GetTickCount() - lastFlush > 1000) {
            lastFlush = GetTickCount();
            if (g_dtraceRepeat) fprintf(g_dtrace, "  (previous line x%u more so far)\n", g_dtraceRepeat), g_dtraceRepeat = 0;
            fflush(g_dtrace);
        }
        if (!strcmp(line, g_dtraceLast)) {
            g_dtraceRepeat++;
        } else {
            if (g_dtraceRepeat) fprintf(g_dtrace, "  (previous line x%u more)\n", g_dtraceRepeat);
            g_dtraceRepeat = 0;
            strcpy(g_dtraceLast, line);
            if (ftell(g_dtrace) < TRACE_MAX) {
                fputs(line, g_dtrace);
                fputc('\n', g_dtrace);
                static uint32_t frames;                 // a killed / hung instance must not lose its last seconds
                if (line[0] == 'F' && ++frames % 60 == 0) fflush(g_dtrace);
            } else {
                fputs("  TRACE STOPPED: size limit reached\n", g_dtrace);
                fflush(g_dtrace);
                g_dtraceFull = true;
            }
        }
    }
    LeaveCriticalSection(&g_dtraceCs);
}

uint16_t g_sessionCw = 0x027F;

void fpu_enforce(uint32_t frame)
{
    static uint16_t lastBad = 0;
    uint16_t cw = fpu_get();
    if (cw == g_sessionCw) return;
    if (cw != lastBad) { logf("fpu: control word %04X at frame %u -> forced %04X", cw, frame, g_sessionCw); lastBad = cw; }
    fpu_set(g_sessionCw);
}

static bool we_have_focus()               // two instances on one PC: only the focused one reacts
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool hotkey(int vk, bool &latch, bool shift)
{
    bool down = we_have_focus() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(vk) & 0x8000) &&
                (!shift || (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    bool edge = down && !latch;
    latch = down;
    return edge;
}

// Xbox "random pick" RNG (0x2F0B0, re-seeded from the tick counter unless the byte at 0x3945A0 is set): pinned to
// the session seed while deterministic.
static uint8_t *const PICK_FIXED = (uint8_t *)0x3945A0;
static uint32_t *const PICK_SEED = (uint32_t *)0x3945A4;
static uint32_t *const PICK_INIT = (uint32_t *)0x3945A8;
static uint8_t g_savePickFixed;

static bool g_detOn;                // deterministic mode (det_enter .. det_leave)

void det_enter(GnCtx *c)
{
    g_saveOverride = *c->timeOverrideEnabled;
    g_saveDelta = *c->timeDeltaOverride;
    *c->timeDeltaOverride = FIXED_DT;
    *c->timeOverrideEnabled = 1;
    g_savePickFixed = *PICK_FIXED;
    *PICK_FIXED = 1;
    *PICK_SEED = *c->randseed ^ 0x9E3779B9;
    *PICK_INIT |= 1;
    *c->detFlag = 1;
    g_detOn = true;
    QueryPerformanceCounter(&g_lastFrame);
}

void det_leave(GnCtx *c)
{
    *c->detFlag = 0;
    g_detOn = false;
    *PICK_FIXED = g_savePickFixed;
    *c->timeOverrideEnabled = g_saveOverride;
    *c->timeDeltaOverride = g_saveDelta;
}

// Stream reader idle wait: the engine reads files on its stream thread (manager 0x4FCA00, thread 0x1FDA30). In
// deterministic mode every read is finished before each lockstep frame. Manager: +0 request in flight, +0x18 Xbox
// CRITICAL_SECTION, +0x34 pending queue head.
extern volatile LONG g_streamBlocked, g_streamApc;
extern "C" void __stdcall krnl_RtlEnterCriticalSection(void *cs);
extern "C" void __stdcall krnl_RtlLeaveCriticalSection(void *cs);
static const uintptr_t STREAM_MGR = 0x4FCA00;

// Read completions go on to the decompressor (0x526EF0, same layout; worker 0x2105B0): idle needs both.
extern volatile LONG g_decompBlocked;
static const uintptr_t DECOMP_MGR = 0x526EF0;

static bool io_idle()
{
    uint8_t *m = (uint8_t *)STREAM_MGR;
    if (m[0xC]) {                                               // reader running
        void *cs = m + 0x18;
        krnl_RtlEnterCriticalSection(cs);
        bool idle = g_streamBlocked && !g_streamApc && !m[0] && !*(uint32_t *)(m + 0x34);
        krnl_RtlLeaveCriticalSection(cs);
        if (!idle) return false;
    }
    uint8_t *d = (uint8_t *)DECOMP_MGR;
    if (!d[0xC]) return true;
    void *cs = d + 0x18;
    krnl_RtlEnterCriticalSection(cs);
    bool idle = g_decompBlocked && !*(uint32_t *)(d + 0x34);
    krnl_RtlLeaveCriticalSection(cs);
    return idle;
}

// I/O gate (deterministic mode): the stream reader and cache writer only work while the main thread is in io_wait -
// at the start of every frame and whenever it is about to Sleep. Each point drains everything outstanding, so what has
// completed is identical on every machine, and a read never completes in the middle of the code that issued it.
volatile LONG g_ioWindow;

bool gn_is_main_thread() { return GetCurrentThreadId() == g_mainThread; }

static void __cdecl main_sleep_notify(int enter)
{
    if (enter && g_detOn && GetCurrentThreadId() == g_mainThread) { dtrace("  sleep-drain"); io_wait(); }
}

void io_wait()
{
    if (GetCurrentThreadId() != g_mainThread) return;          // never from the stream thread
    InterlockedExchange(&g_ioWindow, 1);
    DWORD t0 = GetTickCount();
    int spins = 0;
    while (!io_idle()) {
        if (GetTickCount() - t0 > 10000) { logf("io_wait: stream queue still busy after 10 s - continuing"); break; }
        if (++spins < 200) SwitchToThread(); else Sleep(1);
    }
    InterlockedExchange(&g_ioWindow, 0);
}

// Hold the deterministic session at 60 Hz (game speed = frame rate under a fixed delta).
void pace()
{
    ProfScope pz(PZ_PACE);                              // renderer profiler: 60 Hz session hold
    LONGLONG period = g_qpf.QuadPart / 60;
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = g_lastFrame.QuadPart + period - now.QuadPart;
        if (left <= 0) break;
        if (left > g_qpf.QuadPart / 500) Sleep(1);      // > 2 ms left
    }
    // no catching up after a long frame (loading / network wait): restart the cadence
    if (now.QuadPart - g_lastFrame.QuadPart > period * 4) g_lastFrame = now;
    else g_lastFrame.QuadPart += period;
}

static void record_start(GnCtx *c)
{
    char p[MAX_PATH];
    path_in_dir(p, "gladiusnet_rec.bin");
    g_rec = fopen(p, "wb");
    if (!g_rec) { logf("record: cannot create %s", p); return; }
    *c->randseed = START_SEED;
    memset(&g_hdr, 0, sizeof g_hdr);
    g_hdr.magic = REC_MAGIC;
    g_hdr.padBytes = c->padBytes;
    g_hdr.fixedDt = FIXED_DT;
    g_hdr.randseed = *c->randseed;
    g_hdr.timeClock = *c->timeClock;
    g_hdr.stateId = c->getStateId();
    fwrite(&g_hdr, sizeof g_hdr, 1, g_rec);
    g_frame = 0;
    g_sessionCw = fpu_get();
    det_enter(c);
    g_mode = MODE_RECORD;
    logf("record: START state=%d clock=%.3f", g_hdr.stateId, g_hdr.timeClock);
    MessageBeep(MB_OK);
}

static void record_stop(GnCtx *c)
{
    g_hdr.frames = g_frame;
    fseek(g_rec, 0, SEEK_SET);
    fwrite(&g_hdr, sizeof g_hdr, 1, g_rec);
    fclose(g_rec);
    g_rec = NULL;
    det_leave(c);
    g_mode = MODE_OFF;
    logf("record: STOP %u frames (%.1f s), final state=%d seed=%08X",
         g_frame, g_frame * FIXED_DT, c->getStateId(), *c->randseed);
    MessageBeep(MB_OK);
}

static void replay_start(GnCtx *c)
{
    char p[MAX_PATH];
    path_in_dir(p, "gladiusnet_rec.bin");
    g_rec = fopen(p, "rb");
    if (!g_rec) { logf("replay: no recording (%s)", p); return; }
    if (fread(&g_hdr, sizeof g_hdr, 1, g_rec) != 1 || g_hdr.magic != REC_MAGIC ||
        g_hdr.padBytes != c->padBytes || g_hdr.frames == 0) {
        logf("replay: bad or empty recording");
        fclose(g_rec); g_rec = NULL;
        return;
    }
    int state = c->getStateId();
    if (state != g_hdr.stateId)
        logf("replay: WARNING start state %d != recorded %d", state, g_hdr.stateId);
    *c->randseed = g_hdr.randseed;
    *c->timeClock = g_hdr.timeClock;
    g_frame = 0;
    g_mismatches = 0;
    g_sessionCw = fpu_get();
    det_enter(c);
    g_mode = MODE_REPLAY;
    logf("replay: START %u frames", g_hdr.frames);
    MessageBeep(MB_OK);
}

static void replay_stop(GnCtx *c, const char *why)
{
    fclose(g_rec);
    g_rec = NULL;
    det_leave(c);
    g_mode = MODE_OFF;
    if (g_mismatches == 0)
        logf("replay: %s after %u frames - NO DIVERGENCE (final state=%d seed=%08X)",
             why, g_frame, c->getStateId(), *c->randseed);
    else
        logf("replay: %s after %u frames - %u divergent frames", why, g_frame, g_mismatches);
    MessageBeep(g_mismatches ? MB_ICONHAND : MB_OK);
}

void gn_harness_frame(GnCtx *c)
{
    if (PORT_RELEASE) return;                          // record / replay is a developer tool
    bool f9 = hotkey(VK_F9, g_keyF9);
    bool f10 = hotkey(VK_F10, g_keyF10);

    switch (g_mode) {
    case MODE_OFF:
        if (f9) record_start(c);
        else if (f10) replay_start(c);
        if (g_mode == MODE_OFF) return;
        break;
    case MODE_RECORD:
        if (f9) { record_stop(c); return; }
        break;
    case MODE_REPLAY:
        if (f10) { replay_stop(c, "stopped"); return; }
        break;
    }

    io_wait();                                         // loads land on the same frame on replay
    fpu_enforce(g_frame);
    RecFrameTail tail;
    tail.randseed = *c->randseed;
    tail.stateId = c->getStateId();

    if (g_mode == MODE_RECORD) {
        fwrite(c->padState, c->padBytes, 1, g_rec);
        fwrite(&tail, sizeof tail, 1, g_rec);
    } else {
        uint8_t pads[PAD_BYTES];
        RecFrameTail want;
        if (fread(pads, g_hdr.padBytes, 1, g_rec) != 1 || fread(&want, sizeof want, 1, g_rec) != 1) {
            replay_stop(c, "finished");
            return;
        }
        memcpy(c->padState, pads, c->padBytes);
        if (want.randseed != tail.randseed || want.stateId != tail.stateId) {
            if (g_mismatches < 10)
                logf("replay: DIVERGENCE frame %u: seed %08X (rec %08X) state %d (rec %d)",
                     g_frame, tail.randseed, want.randseed, tail.stateId, want.stateId);
            if (g_mismatches == 0) MessageBeep(MB_ICONHAND);
            g_mismatches++;
        }
    }
    g_frame++;
    pace();
}

bool gn_harness_active()
{
    return g_mode != MODE_OFF;
}

int gn_init(GnCtx *c)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(g_dir, exe);
    char *slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0; else g_dir[0] = 0;
    QueryPerformanceFrequency(&g_qpf);
    timeBeginPeriod(1);
    g_mainThread = GetCurrentThreadId();                       // gn_init runs on the game thread
    extern void (*port_log_hook)(const char *line);
    if (const GnNetApi *n = gn_net()) port_log_hook = n->remote_log;
    extern void (*port_sleep_notify)(int enter);
    port_sleep_notify = main_sleep_notify;
    logf(PORT_RELEASE ? "init: deterministic core ok" : "init: deterministic core ok - Ctrl+F9 record, Ctrl+F10 replay");
    return 1;
}
