// [debug] MemStats=1: who allocates game memory, how much, on which thread.
// Counts every allocation that goes through the XAPI wrappers (process heap, private heaps, "physical",
// VirtualAlloc) per thread role; "mem:" lines every 60 s with live / peak bytes and counts since the last report.
#include "port.h"
#include "memstats.h"
#include <intrin.h>
#include <unordered_map>
#include <unordered_set>

static bool g_on;
static bool g_hc_on;
static DWORD g_role_tls = TLS_OUT_OF_INDEXES;

struct Cell {
    volatile LONG allocs, frees;
    volatile LONGLONG live, peak, total;
    LONG last_allocs, last_frees;                       // at the previous report
};
static Cell g_cells[MS_KINDS][MS_ROLES];
static uintptr_t g_lo[MS_KINDS], g_hi[MS_KINDS];        // address range seen per kind
static SRWLOCK g_range_lock = SRWLOCK_INIT;

static const char *const KIND_NAMES[MS_KINDS] = {"heap", "privheap", "phys", "virtual"};
static const char *const ROLE_NAMES[MS_ROLES] = {"other", "main", "cri", "io"};

void memstats_init(const char *ini)
{
    g_hc_on = GetPrivateProfileIntA("debug", "HeapCheck", 0, ini) != 0;
    if (g_hc_on)
        port_log("debug: HeapCheck on (stale frees of game heap blocks are logged and refused)\n");
    g_on = GetPrivateProfileIntA("debug", "MemStats", 0, ini) != 0;
    if (!g_on)
        return;
    g_role_tls = TlsAlloc();
    port_log("debug: MemStats on (mem: lines every 60 s)\n");
}

void memstats_set_role(int role)
{
    if (g_on && g_role_tls != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_role_tls, (void *)(uintptr_t)(role + 1));
}

// game threads by entry point (xapi_CreateThread)
int memstats_role_for_thread(uint32_t start)
{
    switch (start) {
    case 0x1FDA30:                                      // stream reader
    case 0x2105B0:                                      // z:\gladius.cba cache writer
        return MS_IO;
    case 0x211560:                                      // CRI idle meter
    case 0x2115A0:
    case 0x211610:
    case 0x211A50:                                      // CRI servers
        return MS_CRI;
    default:
        return MS_OTHER;
    }
}

static int current_role()
{
    if (g_role_tls == TLS_OUT_OF_INDEXES)
        return MS_OTHER;
    uintptr_t v = (uintptr_t)TlsGetValue(g_role_tls);
    return v ? (int)(v - 1) : MS_OTHER;
}

void memstats_alloc(int kind, const void *p, size_t size)
{
    if (!g_on || !p)
        return;
    Cell &c = g_cells[kind][current_role()];
    InterlockedIncrement(&c.allocs);
    InterlockedExchangeAdd64(&c.total, (LONGLONG)size);
    LONGLONG now = InterlockedExchangeAdd64(&c.live, (LONGLONG)size) + (LONGLONG)size;
    if (now > c.peak)
        c.peak = now;                                   // approximate under races; fine for sizing
    uintptr_t a = (uintptr_t)p;
    AcquireSRWLockExclusive(&g_range_lock);
    if (!g_lo[kind] || a < g_lo[kind])
        g_lo[kind] = a;
    if (a + size > g_hi[kind])
        g_hi[kind] = a + size;
    ReleaseSRWLockExclusive(&g_range_lock);
}

// frees are charged to the freeing thread's role; live bytes per role can go negative when one role frees what
// another allocated - the per-kind total is what matters for sizing
void memstats_free(int kind, size_t size)
{
    if (!g_on)
        return;
    Cell &c = g_cells[kind][current_role()];
    InterlockedIncrement(&c.frees);
    InterlockedExchangeAdd64(&c.live, -(LONGLONG)size);
}

bool memstats_enabled() { return g_on; }

void memstats_tick()                                    // watchdog thread, every 50 ms
{
    static DWORD last;
    if (!g_on)
        return;
    DWORD now = GetTickCount();
    if (last && now - last < 60000)
        return;
    last = now;
    for (int k = 0; k < MS_KINDS; k++) {
        LONGLONG live = 0, peak = 0;
        for (int r = 0; r < MS_ROLES; r++)
            live += g_cells[k][r].live, peak += g_cells[k][r].peak;
        port_log("mem: %-8s live %7.2f MB (sum of role peaks %7.2f MB) range %08X-%08X\n", KIND_NAMES[k],
                 live / 1048576.0, peak / 1048576.0, (unsigned)g_lo[k], (unsigned)g_hi[k]);
        for (int r = 0; r < MS_ROLES; r++) {
            Cell &c = g_cells[k][r];
            if (!c.allocs && !c.frees)
                continue;
            LONG a = c.allocs, f = c.frees;
            port_log("mem:   %-5s allocs %8ld (+%ld) frees %8ld (+%ld) live %8.2f MB peak %8.2f MB total %9.2f MB\n",
                     ROLE_NAMES[r], a, a - c.last_allocs, f, f - c.last_frees, c.live / 1048576.0, c.peak / 1048576.0,
                     c.total / 1048576.0);
            c.last_allocs = a, c.last_frees = f;
        }
    }
}

// ── HeapCheck ───────────────────────────────────────────────────────────
// The engine's tracked heap (alloc header 0x20 bytes in front of the user pointer, free 0x176A30) sits on CRT
// malloc -> xapi_RtlAllocateHeap -> the Windows process heap, which D3D11 / the driver also use. A block the game
// frees twice may already belong to a D3D11 object by the second free (2026-10-06 dumps: d3d11 CDeviceChild vtable
// in the "header") -> the unlink in 0x176A30 writes through it. Here every game heap block is tracked; a tracked
// free of a block that is no longer live is logged with both freeing stacks and skipped.
struct FreeRec {
    uint32_t stack[10];
    uint32_t seq;
};
static SRWLOCK g_hc_lock = SRWLOCK_INIT;
static std::unordered_set<uintptr_t> *g_live;
static std::unordered_map<uintptr_t, FreeRec> *g_freed;
static const uint32_t RING = 1 << 16;
static uintptr_t g_ring[RING];
static uint32_t g_ring_seq;
static int g_hc_logged, g_hc_unknown;
extern volatile DWORD g_port_probe_tid;                   // port_main.cpp: thread doing a guarded read

bool heapcheck_enabled() { return g_hc_on; }

// game return addresses on the current stack (image .text, preceded by a call)
void memstats_game_stack(uint32_t *out, int n, const void *from)
{
    const uint32_t *sp = (const uint32_t *)from;
    const uint32_t *top = (const uint32_t *)__readfsdword(4);
    int k = 0;
    for (int i = 0; i < 1024 && sp + i < top && k < n; i++) {
        uint32_t v = sp[i];
        if (v < 0x12000 || v >= 0x2F0000)
            continue;
        const uint8_t *c = (const uint8_t *)v;
        if (c[-5] != 0xE8 && c[-6] != 0xFF && c[-2] != 0xFF && c[-3] != 0xFF)
            continue;
        out[k++] = v;
    }
    while (k < n)
        out[k++] = 0;
}

void memstats_fmt_stack(char *o, int n, const uint32_t *s, int count)
{
    int w = 0;
    for (int i = 0; i < count && s[i] && w < n - 8; i++)
        w += _snprintf(o + w, n - w, " %06X", s[i]);
    o[w < n ? w : n - 1] = 0;
}

void heapcheck_alloc(const void *p)
{
    if (!g_hc_on || !p)
        return;
    AcquireSRWLockExclusive(&g_hc_lock);
    if (!g_live)
        g_live = new std::unordered_set<uintptr_t>(), g_freed = new std::unordered_map<uintptr_t, FreeRec>();
    g_live->insert((uintptr_t)p);
    g_freed->erase((uintptr_t)p);
    ReleaseSRWLockExclusive(&g_hc_lock);
}

void heapcheck_free(const void *p)
{
    if (!g_hc_on || !p)
        return;
    FreeRec r;
    memstats_game_stack(r.stack, 10, _AddressOfReturnAddress());
    AcquireSRWLockExclusive(&g_hc_lock);
    if (g_live && g_live->erase((uintptr_t)p)) {
        r.seq = ++g_ring_seq;
        uintptr_t &slot = g_ring[r.seq & (RING - 1)];
        if (slot) {                                     // forget the oldest record in this ring slot
            auto it = g_freed->find(slot);
            if (it != g_freed->end() && it->second.seq == r.seq - RING)
                g_freed->erase(it);
        }
        slot = (uintptr_t)p;
        (*g_freed)[(uintptr_t)p] = r;
    }
    ReleaseSRWLockExclusive(&g_hc_lock);
}

// 0x176A30 entry (thiscall tracker, user pointer): 1 = go ahead, 0 = refuse
static int __cdecl tracked_free_ok(const uint8_t *user, const void *sp)
{
    if (!user)
        return 1;
    uintptr_t hdr = (uintptr_t)user - 0x20, alt = 0;
    g_port_probe_tid = GetCurrentThreadId();            // the crash handler ignores faults of this probe
    __try {
        alt = hdr - *(const uint32_t *)(hdr - 4);       // aligned blocks: malloc block = header - [header-4]
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_port_probe_tid = 0;
    AcquireSRWLockShared(&g_hc_lock);
    bool live = g_live && (g_live->count(hdr) || (alt && g_live->count(alt)));
    FreeRec prev = {};
    bool stale = false;
    if (!live && g_freed) {
        auto it = g_freed->find(hdr);
        if (it == g_freed->end() && alt)
            it = g_freed->find(alt);
        if (it != g_freed->end())
            prev = it->second, stale = true;
    }
    ReleaseSRWLockShared(&g_hc_lock);
    if (live)
        return 1;
    uint32_t now[10];
    memstats_game_stack(now, 10, sp);
    char a[128], b[128];
    memstats_fmt_stack(a, sizeof a, now, 10);
    if (!stale) {
        if (g_hc_unknown++ < 20)
            port_log("heapcheck: tracked free of unknown block %p (header %08X) <-%s\n", user, (unsigned)hdr, a);
        return 1;                                       // not ours to judge: let it through
    }
    memstats_fmt_stack(b, sizeof b, prev.stack, 10);
    if (g_hc_logged++ < 50) {
        port_log("heapcheck: STALE FREE of %p (header %08X, freed %u frees ago) - refused\n", user, (unsigned)hdr,
                 g_ring_seq - prev.seq);
        port_log("heapcheck:   now   <-%s\n", a);
        port_log("heapcheck:   first <-%s\n", b);
    }
    return 0;
}

static __declspec(naked) void tracked_free_hook()
{
    __asm {
        push ecx
        lea eax, [esp + 8]              // the caller's stack: [esp+4] = return address, [esp+8] = user pointer
        push eax
        push dword ptr [esp + 0xC]
        call tracked_free_ok
        add esp, 8
        pop ecx
        test eax, eax
        jz refuse
        mov eax, [esp + 4]              // displaced: mov eax,[esp+4]; test eax,eax
        test eax, eax
        push 0x176A36
        ret
    refuse:
        ret 4
    }
}

void heapcheck_install()
{
    if (!g_hc_on)
        return;
    uint8_t *p = (uint8_t *)0x176A30;
    static const uint8_t expect[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0};
    if (memcmp(p, expect, sizeof expect)) {
        port_log("heapcheck: unexpected bytes at 0x176A30 - tracked free hook not installed\n");
        return;
    }
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)tracked_free_hook - (0x176A30 + 5));
    p[5] = 0x90;
    port_log("heapcheck: tracked free 0x176A30 hooked\n");
}
