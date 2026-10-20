// Xbox default.xbe hooks for the frame and deterministic mode.
// Frame hook: the engine's pad poll (call 0x183360 at 0x1CD5BF) comes here first; controllers are sampled, then
// gladiusnet.dll exchanges them or the harness records / replays them. The save screen pages, the God Camera and the
// editor run from here too.
// Deterministic mode (det_enter while a session or harness run is active):
//   time_Update 0x18EC20       fixed 1/60 delta (engine override 0x49C2B4 / 0x49C2B0); RealDelta 0x49C214 too
//   SinceFrameStart 0x18EDC0   real time since the frame began -> 0
//   UnitAIScope::Update 0xE3160  AI think loop: fixed iterations instead of 4.5 ms of real time
//   stream reader 0x4FCA00     reads drained in the I/O gate (gn_core.cpp)
//   decompressor 0x526EF0      completions handed to worker 0x2105B0: also drained
#include "gn.h"
#include "gladiusnet_api.h"
#include "handles.h"
#include <string.h>
#include <intrin.h>

void port_log(const char *fmt, ...);
void port_pads_sample(BYTE *slots);         // xapi_input.cpp
void port_pads_override(const BYTE *slots);
int gn_init(GnCtx *c);                      // gn_core.cpp
void gn_harness_frame(GnCtx *c);

volatile LONG g_streamBlocked, g_streamApc;  // stream reader thread state (io_idle in gn_core.cpp)
volatile LONG g_decompBlocked;               // decompression worker blocked in its wait

static uint32_t g_det;
static uint8_t g_pads[PAD_BYTES];
static uint32_t g_dummySeedMix;
static void *g_dummyTraceFn;
static uint8_t g_dummyInputSys[0x20];
static uint32_t g_padMap[NSLOTS] = {0, 1, 2, 3};
static uint32_t g_curTeam = 0xFFFFFFFF, g_joinPad[NSLOTS] = {0, 1, 2, 3}, g_curMemCard;
static void *g_dummyPcManager;

static int __cdecl xbox_state_id()
{
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);   // state manager -> current state
    return state ? *(const int32_t *)(state + 8) : -1;
}

static GnCtx g_ctx = {
    8,
    g_pads, PAD_BYTES,
    (uint32_t *)0x4CA344,                   // gameplay RNG seed (Rand inlined: imul 0x343FD / add 0x26A247)
    (int32_t *)0x49C2B4,                    // time_OverrideEnabled
    (float *)0x49C2B0,                      // time_DeltaOverride
    (float *)0x49C210,                      // time_clock
    &g_det,
    &g_dummySeedMix,                        // no Srand in the Xbox build (seed only advanced by Rand)
    xbox_state_id,
    &g_dummyTraceFn,
    g_dummyInputSys,                        // keyboard folds into this machine's pad sample instead
    g_padMap,
    &g_curTeam,
    g_joinPad,
    &g_dummyPcManager,
    &g_curMemCard,
};

// ── determinism patches ─────────────────────────────────────────────────
#define FIXED_DT_BITS 0x3C888889             // 1/60 (macros: inline asm immediates)
#define AI_ITERATIONS 64                     // per frame in deterministic mode (same everywhere)
static int g_aiLeft;
static int g_aiLogged;

static void __cdecl ai_log_real(uint32_t iterations)    // calibration: real-time loop counts
{
    if (g_aiLogged < 40) { g_aiLogged++; logf("ai: think slice ran %u iterations (real time)", iterations); }
}

static uint32_t g_aiStartCount;

// Replaces 0xE31A1..0xE31CB (loop + elapsed): ebp = slice budget, esi = ticks per 100 us, edi = start ticks,
// bl = NotDone; [esp+0x38] = ticks per ms, [esp+0x2C/0x30] = elapsed.
static __declspec(naked) void ai_loop()
{
    __asm {
        cmp g_det, 0
        jne det
        mov eax, ds:[0x4162E0]
        mov g_aiStartCount, eax
    real_loop:
        mov eax, 0xE2C80                // UnitAI::Update -> NotDone
        call eax
        mov bl, al
        inc dword ptr ds:[0x4162E0]
        mov eax, 0x18E8D0               // ticks
        call eax
        sub eax, edi
        xor edx, edx
        div esi
        cmp eax, ebp
        jae real_out
        test bl, bl
        jne real_loop
    real_out:
        mov eax, ds:[0x4162E0]
        sub eax, g_aiStartCount
        push eax
        call ai_log_real
        add esp, 4
        push 0xE31C1                    // original: elapsed = ticks - start
        ret
    det:
        mov eax, AI_ITERATIONS
        mov g_aiLeft, eax
    det_loop:
        mov eax, 0xE2C80
        call eax
        mov bl, al
        inc dword ptr ds:[0x4162E0]
        dec g_aiLeft
        jz det_out
        test bl, bl
        jne det_loop
    det_out:
        mov eax, [esp + 0x38]           // fixed 4 ms of "thinking" per frame
        shl eax, 2
        mov [esp + 0x30], eax
        mov dword ptr [esp + 0x2C], 0
        xor edx, edx                    // 0xE31D3 stores it as the elapsed high dword
        push 0xE31CC
        ret
    }
}

// call at 0x18EC9B (was fstp [0x49C214]); flags from `test edx,edx` at 0x18EC83 are still live
static __declspec(naked) void real_delta_store()
{
    __asm {
        fstp dword ptr ds:[0x49C214]
        pushfd
        cmp g_det, 0
        je done
        mov dword ptr ds:[0x49C214], FIXED_DT_BITS
    done:
        popfd
        ret
    }
}

// jmp at 0x18EDC0 (SinceFrameStart, returns st0)
static __declspec(naked) void since_frame_start()
{
    __asm {
        cmp g_det, 0
        jne zero
        mov al, byte ptr ds:[0x49C258]
        push 0x18EDC5
        ret
    zero:
        fldz
        ret
    }
}

// jmp at 0x1FD850 (StreamManager enqueue). Must not wait for the read here: callers set their own "loading" state
// after the enqueue returns (see the I/O gate in gn_core.cpp).
// Game return addresses on the stack (the read's issuers), for the trace.
static void stack_callers(char *out, int n)
{
    const uint32_t *sp = (const uint32_t *)_AddressOfReturnAddress();
    int o = 0, found = 0;
    const uint32_t *top = (const uint32_t *)__readfsdword(4);           // stack base (TIB)
    for (int i = 0; i < 800 && sp + i < top && found < 20 && o < n - 8; i++) {
        uint32_t v = sp[i];
        if (v < 0x12000 || v > 0x2F0000) continue;
        const uint8_t *c = (const uint8_t *)v;
        if (c[-5] != 0xE8 && c[-6] != 0xFF && c[-2] != 0xFF && c[-3] != 0xFF) continue;
        o += _snprintf(out + o, n - o, " %06X", v);
        found++;
    }
    out[o < n ? o : n - 1] = 0;
}

namespace pkgdiag { void read_queued(uint32_t buf, uint32_t size, const char *stack); }

static void __cdecl after_enqueue(uint32_t caller, int front, const uint8_t *cmd)
{
    char st[260];
    stack_callers(st, sizeof st);
    if (cmd) pkgdiag::read_queued(*(const uint32_t *)(cmd + 0x10), *(const uint32_t *)(cmd + 0x14), st);
    if (!g_det) return;
    dtrace("  read-issued%s from %06X tid %u <-%s", front ? " (front)" : "", caller, GetCurrentThreadId(), st);
    const uint8_t *m = (const uint8_t *)0x4FCA00;
    dtrace("  queue after enqueue: inflight %u running %u head %08X cs lock %d owner %u", m[0], m[0xC],
           *(const uint32_t *)(m + 0x34), *(const int32_t *)(m + 0x18 + 0x10), *(const uint32_t *)(m + 0x18 + 0x18));
}

// I/O threads: no work outside the gate while deterministic
extern volatile LONG g_ioWindow;
int port_lock_suspend();
void port_lock_resume(int depth);
static void io_gate()
{
    if (!g_det || g_ioWindow) return;
    int depth = port_lock_suspend();
    while (g_det && !g_ioWindow) Sleep(1);
    port_lock_resume(depth);
}

static __declspec(naked) void enqueue_original()
{
    __asm {
        push esi
        mov esi, ecx
        mov al, byte ptr [esi + 0xC]
        push 0x1FD856
        ret
    }
}

static __declspec(naked) void enqueue_hook()
{
    __asm {
        push dword ptr [esp + 4]
        call enqueue_original           // ecx = manager, pops the argument
        push eax
        push dword ptr [esp + 8]        // the request (still on the caller's stack)
        push 0
        push dword ptr [esp + 0xC]      // caller
        call after_enqueue
        add esp, 12
        pop eax
        ret 4
    }
}

// jmp at 0x1FD8C0 (enqueue at the front of the queue: async package reads)
static __declspec(naked) void enqueue_front_original()
{
    __asm {
        push esi
        mov esi, ecx
        mov al, byte ptr [esi + 0xC]
        push 0x1FD8C6
        ret
    }
}

static __declspec(naked) void enqueue_front_hook()
{
    __asm {
        push dword ptr [esp + 4]
        call enqueue_front_original
        push eax
        push dword ptr [esp + 8]        // the request (still on the caller's stack)
        push 1
        push dword ptr [esp + 0xC]      // caller
        call after_enqueue
        add esp, 12
        pop eax
        ret 4
    }
}

// The reader thread's wait (call 0x1FDAA1) and completion APC (0x1FD5C7). If a state sync rebuilt the handle table
// while they were parked, their event handle is stale: go round the loop instead.
LONG vh_epoch();
static DWORD __stdcall stream_wait(HANDLE h, DWORD ms, BOOL alertable)
{
    typedef DWORD(__stdcall * WaitFn)(HANDLE, DWORD, BOOL);
    LONG epoch = vh_epoch();
    io_gate();
    if (vh_epoch() != epoch)
        return WAIT_OBJECT_0;
    InterlockedExchange(&g_streamBlocked, 1);
    if (g_det) {
        const uint8_t *m = (const uint8_t *)0x4FCA00;
        dtrace("  stream-wait %p -> %p | inflight %u running %u head %08X cs lock %d owner %u", h, vh_real(h), m[0],
               m[0xC], *(const uint32_t *)(m + 0x34), *(const int32_t *)(m + 0x18 + 0x10), *(const uint32_t *)(m + 0x18 + 0x18));
    }
    DWORD r = ((WaitFn)0x168C07)(h, ms, alertable);
    if (g_det) dtrace("  stream-woke %X", r);
    InterlockedExchange(&g_streamBlocked, 0);
    io_gate();
    return r;
}

// The decompression worker's wait (call 0x210618).
static DWORD __stdcall decomp_wait(HANDLE h, DWORD ms, BOOL alertable)
{
    typedef DWORD(__stdcall * WaitFn)(HANDLE, DWORD, BOOL);
    LONG epoch = vh_epoch();
    io_gate();
    if (vh_epoch() != epoch)
        return WAIT_OBJECT_0;
    InterlockedExchange(&g_decompBlocked, 1);
    DWORD r = ((WaitFn)0x168C07)(h, ms, alertable);
    InterlockedExchange(&g_decompBlocked, 0);
    io_gate();
    return r;
}

static void __stdcall stream_apc(DWORD err, DWORD bytes, void *ov)
{
    typedef void(__stdcall * ApcFn)(DWORD, DWORD, void *);
    InterlockedIncrement(&g_streamApc);
    if (g_det) dtrace("  read-done err %u bytes %u", err, bytes);
    ((ApcFn)0x1FD4E0)(err, bytes, ov);
    InterlockedDecrement(&g_streamApc);
}

// Synthetic clock: the engine's tick readers (0x18E8D0 / 0x18E900) feed real-time decisions in game logic. In
// deterministic mode the main thread reads start + frame * (frequency / 60), advancing slightly per read and never
// backwards. Other threads (VBlank, audio, movies) keep real time.
bool gn_is_main_thread();
static uint64_t g_synthNow, g_synthStart;
static uint32_t g_synthFrame0;
static bool g_synthOn;

static uint64_t real_ticks()
{
    if (*(const uint8_t *)0x49C258) {
        LARGE_INTEGER v;
        ((BOOL(__stdcall *)(LARGE_INTEGER *))0x284653)(&v);
        return (uint64_t)v.QuadPart;
    }
    return ((DWORD(__stdcall *)())0x2840A3)();
}

static uint64_t __cdecl ticks_value()
{
    if (!g_det || !gn_is_main_thread()) { g_synthOn = false; return real_ticks(); }
    uint64_t freq = *(const uint8_t *)0x49C258 ? *(const uint64_t *)0x49C250 : 1000;
    uint64_t perFrame = freq / 60, step = perFrame / 256 ? perFrame / 256 : 1;
    if (!g_synthOn) {                                   // first read in this deterministic stretch
        g_synthOn = true;
        g_synthStart = g_synthNow = real_ticks();
        g_synthFrame0 = g_dtraceFrame;
    }
    uint64_t frameBase = g_synthStart + (uint64_t)(g_dtraceFrame - g_synthFrame0) * perFrame;
    g_synthNow += step;
    if (g_synthNow < frameBase) g_synthNow = frameBase;
    return g_synthNow;
}

static __declspec(naked) void ticks_hook()
{
    __asm {
        call ticks_value                // edx:eax
        ret
    }
}

// Package load diagnostics: a package load (0x1AE630, callback 0x1E5B20) only finishes when the request's running
// total (req[2]) equals the last read's size (req[1]); one that arrives in two reads would hang its waiters.
static int g_pkgCalls, g_pkgOdd;
namespace pkgdiag { void on_loaded(const uint8_t *loader, uint32_t size); void install(); }

static void __cdecl pkg_cb_log(const uint32_t *req, const uint8_t *loader)
{
    g_pkgCalls++;
    if (req && req[2] == req[1]) pkgdiag::on_loaded(loader, req[1]);
    if (!req || req[2] == req[1] || g_pkgOdd >= 40) return;
    g_pkgOdd++;
    const char *name = "?";
    if (loader && *(const uint8_t *const *)(loader + 8)) name = (const char *)(*(const uint8_t *const *)(loader + 8) + 0xC);
    logf("pkg: '%.24s' read %u bytes but total %u - load will not complete (req %p, call %d)", name, req[1], req[2],
         req, g_pkgCalls);
}

static __declspec(naked) void pkg_cb_hook()
{
    __asm {
        push dword ptr [esp + 0xC]      // loader
        push dword ptr [esp + 0xC]      // req (2nd argument)
        call pkg_cb_log
        add esp, 8
        push esi                        // original: push esi; mov esi,[esp+0x10]
        mov esi, dword ptr [esp + 0x10]
        push 0x1E5B25
        ret
    }
}

// ── frame hook ──────────────────────────────────────────────────────────
bool gn_harness_active();                   // gn_core.cpp
static bool g_inited;
static const GnNetApi *g_net;               // gladiusnet.dll, NULL = offline

const GnNetApi *gn_net() { return g_net; }

void port_save_frame();                     // save_pages.cpp

static void __cdecl pad_poll_hook()
{
    if (!g_inited) {
        g_inited = true;
        gn_init(&g_ctx);
        if (g_net) g_net->init(&g_ctx);
    }
    port_pads_sample(g_pads);
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    int stateId = state ? *(const int32_t *)(state + 8) : -1;
    int sub = stateId == 21 ? *(const int32_t *)(state + 0x2C) : -1;      // main menu sub-state
    bool busy = g_net && g_net->frame(&g_ctx, stateId, sub, gn_harness_active());
    if (!busy) gn_harness_frame(&g_ctx);
    port_save_frame();
    if (g_net) g_net->after(&g_ctx, stateId, sub);
    port_pads_override(g_pads);
    void port_codes_frame();                            // codes.cpp: per-frame gameplay codes (money, JP)
    port_codes_frame();
    ((void(__cdecl *)())0x183360)();
    void port_godcam_frame();                           // godcam.cpp: fly the God Camera with this frame's pads
    port_godcam_frame();
    void port_editor_frame();                           // editor.cpp: file loads for the editor window
    port_editor_frame();
}

// Load gladiusnet.dll from next to gladius.exe and check its ABI; any mismatch means the game runs offline.
static const GnNetApi *load_netplay()
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "gladiusnet.dll");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        port_log("net: gladiusnet.dll not found - online versus off\n");
        return NULL;
    }
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        port_log("net: gladiusnet.dll did not load (error %u: built for another gladiusport.dll?) - online versus off\n",
                 (unsigned)GetLastError());
        return NULL;
    }
    GladiusNetEntry entry = (GladiusNetEntry)GetProcAddress(m, "gladiusnet_api");
    const GnNetApi *api = entry ? entry(GLADIUSNET_ABI) : NULL;
    if (!api || api->abi != GLADIUSNET_ABI) {
        port_log("net: gladiusnet.dll interface %u, this gladiusport.dll needs %u - online versus off\n",
                 api ? api->abi : 0, GLADIUSNET_ABI);
        FreeLibrary(m);
        return NULL;
    }
    return api;
}

// ── install ─────────────────────────────────────────────────────────────
// True when the bytes at va match the hex string.
static bool expect(uint32_t va, const char *hex)
{
    const uint8_t *p = (const uint8_t *)va;
    for (int i = 0; hex[i * 2]; i++) {
        unsigned b;
        sscanf(hex + i * 2, "%2x", &b);
        if (p[i] != b) {
            port_log("net: unexpected bytes at %08X - netplay hooks not installed\n", va);
            return false;
        }
    }
    return true;
}

static void put_rel(uint32_t va, uint8_t op, const void *target, int len)
{
    uint8_t *p = (uint8_t *)va;
    p[0] = op;
    *(int32_t *)(p + 1) = (int32_t)((uint32_t)target - (va + 5));
    for (int i = 5; i < len; i++) p[i] = 0x90;
}

// Exact reciprocals: game logic computes 1/x with rcpps + a Newton step, and rcpps' low bits differ between CPU
// families, so results drift between netplay machines. Each site's rcpps + mulps becomes a call to a stub doing an
// exact divps plus the displaced mulps. Flags are untouched.
static __declspec(align(16)) const float k_ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};

static __declspec(naked) void rcp_t1_a0()               // rcpps xmm1,xmm0 ; mulps xmm0,xmm1
{
    __asm {
        movaps xmm1, xmmword ptr k_ones
        divps xmm1, xmm0
        mulps xmm0, xmm1
        ret
    }
}
static __declspec(naked) void rcp_t3_a2()               // rcpps xmm3,xmm2 ; mulps xmm2,xmm3
{
    __asm {
        movaps xmm3, xmmword ptr k_ones
        divps xmm3, xmm2
        mulps xmm2, xmm3
        ret
    }
}
static __declspec(naked) void rcp_t4_a2()               // rcpps xmm4,xmm2 ; mulps xmm2,xmm4
{
    __asm {
        movaps xmm4, xmmword ptr k_ones
        divps xmm4, xmm2
        mulps xmm2, xmm4
        ret
    }
}

// Trace: every screen-fade start (0x184000) with its game callers, so a trace diff shows who started a fade.
static void __cdecl fade_trace(const uint32_t *sp)
{
    char st[96];
    int o = 0, n = 0;
    for (int i = 0; i < 64 && n < 6 && o < 80; i++)
        if (sp[i] >= 0x12000 && sp[i] < 0x2F0000) o += _snprintf(st + o, sizeof st - o, " %06X", sp[i]), n++;
    st[o < (int)sizeof st ? o : (int)sizeof st - 1] = 0;
    dtrace("  fade-start %08X from%s", sp[1], st);
}
static __declspec(naked) void fade_hook()
{
    __asm {
        pushad
        lea eax, [esp + 32]             // the caller's esp: [0] return address, [4] seconds
        push eax
        call fade_trace
        add esp, 4
        popad
        movss xmm2, dword ptr [esp + 4]
        push 0x184006
        ret
    }
}

static void exact_reciprocals()
{
    static const struct { uint32_t va; const char *hex; void (*stub)(); } sites[] = {
        {0xA1736, "0f53c80f59c1", rcp_t1_a0}, {0xA176A, "0f53c80f59c1", rcp_t1_a0},
        {0xA18B0, "0f53c80f59c1", rcp_t1_a0}, {0xA606B, "0f53c80f59c1", rcp_t1_a0},
        {0xA61C4, "0f53c80f59c1", rcp_t1_a0}, {0xA635F, "0f53c80f59c1", rcp_t1_a0},
        {0xD0465, "0f53c80f59c1", rcp_t1_a0}, {0xA6744, "0f53da0f59d3", rcp_t3_a2},
        {0x1BED8C, "0f53e20f59d4", rcp_t4_a2},
    };
    int n = 0;
    for (auto &s : sites)
        if (expect(s.va, s.hex)) {
            put_rel(s.va, 0xE8, (const void *)s.stub, 6);
            n++;
        }
    port_log("net: %d/9 rcpps reciprocals made exact (cross-CPU determinism)\n", n);
}

// Versus join order: the join panel (main menu +0xC4) lists the joined pads in the order Start was pressed (+0xC4..
// +0xD0, count +0xB8), and the match start (0xFD8BC) maps pad -> player from that list (0x183070). Pads that joined
// out of order (pad 2 before pad 1) got swapped players, and battle input followed the wrong controller. Sort the
// list into pad order first - the same as everyone joining in order.
static void __cdecl join_sort(uint8_t *join)
{
    int n = *(int *)(join + 0xB8);
    int *pad = (int *)(join + 0xC4);
    if (n < 2 || n > 4) return;
    bool moved = false;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && pad[j - 1] > pad[j]; j--) {
            int t = pad[j]; pad[j] = pad[j - 1]; pad[j - 1] = t;
            moved = true;
        }
    if (moved) port_log("versus: players joined out of order - pads sorted to %d %d %d %d\n", pad[0], n > 1 ? pad[1] : -1,
                        n > 2 ? pad[2] : -1, n > 3 ? pad[3] : -1);
}
static __declspec(naked) void join_sort_hook()
{
    __asm {
        pushad
        push dword ptr [ebp + 0xC4]
        call join_sort
        add esp, 4
        popad
        mov edx, dword ptr [ebp + 0xC4]                 // displaced instruction
        ret
    }
}

void gn_install(bool netplay)
{
    if (expect(0xFD8AB, "8b95c4000000"))                // match start: mov edx, [ebp+0xC4] (the join panel)
        put_rel(0xFD8AB, 0xE8, join_sort_hook, 6);
    if (!expect(0x1CD5BF, "e89c5dfbff") ||          // call joypad poll 0x183360
        !expect(0xE31A1, "e8dafaffff") ||           // call UnitAI::Update 0xE2C80
        !expect(0xE31C1, "e80ab70a00") ||           // call ticks 0x18E8D0
        !expect(0x18EC9B, "d91d14c24900") ||        // fstp [time_RealDelta]
        !expect(0x18EDC0, "a058c24900") ||          // mov al,[QPC flag]
        !expect(0x1FD850, "568bf18a460c") ||        // StreamManager enqueue prologue
        !expect(0x1FD8C0, "568bf18a460c") ||        // StreamManager enqueue-at-front prologue
        !expect(0x1FD5C7, "68e0d41f00") ||          // push completion routine 0x1FD4E0
        !expect(0x1FDAA1, "e861b1f6ff") ||          // call WaitForSingleObjectEx 0x168C07
        !expect(0x210618, "e8ea85f5ff") ||          // decompressor wait, same call
        !expect(0x101B95, "83fd02") ||              // main menu: VersuS item needs >= 2 local pads
        !expect(0x1E5B20, "568b742410") ||          // package load callback prologue
        !expect(0x18E8D0, "a058c2490083ec08") ||    // tick reader (QPC / GetTickCount)
        !expect(0x18E900, "a058c2490083ec08"))      // second tick reader, same code
        return;
    if (netplay) g_net = load_netplay();
    if (g_net) *(uint8_t *)0x101B97 = 0;            // online players bring their own controllers
    put_rel(0x1CD5BF, 0xE8, pad_poll_hook, 5);
    put_rel(0xE31A1, 0xE9, ai_loop, 5);
    put_rel(0x18EC9B, 0xE8, real_delta_store, 6);
    put_rel(0x18EDC0, 0xE9, since_frame_start, 5);
    put_rel(0x1FD850, 0xE9, enqueue_hook, 6);
    put_rel(0x1FD8C0, 0xE9, enqueue_front_hook, 6);
    *(uint32_t *)(0x1FD5C7 + 1) = (uint32_t)stream_apc;
    put_rel(0x1FDAA1, 0xE8, stream_wait, 5);
    put_rel(0x210618, 0xE8, decomp_wait, 5);
    put_rel(0x1E5B20, 0xE9, pkg_cb_hook, 5);
    put_rel(0x18E8D0, 0xE9, ticks_hook, 5);
    put_rel(0x18E900, 0xE9, ticks_hook, 5);
    exact_reciprocals();
    if (expect(0x184000, "f30f10542404"))           // screen fade start: movss xmm2,[esp+4]
        put_rel(0x184000, 0xE9, fade_hook, 6);
    if (g_net) g_net->install();
    pkgdiag::install();
    port_log("net: frame hook + deterministic time/AI/IO hooks installed%s\n", g_net ? ", gladiusnet.dll loaded" : "");
}
