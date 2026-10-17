// Memory container: the game's heap and "physical" memory live in fixed reserved ranges managed by dlmalloc mspaces.
// dlmalloc is deterministic, so every PC in a lockstep session gets identical game memory layouts. The ranges are
// reserved in DllMain; [mem] Container=1 commits them and routes the game's allocations here.
#include "port.h"
#include "container.h"
#include "memstats.h"
#include <intrin.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

extern "C" {
typedef void *mspace;
mspace create_mspace_with_base(void *base, size_t capacity, int locked);
void *mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void *mem);
void *mspace_realloc(mspace msp, void *mem, size_t newsize);
void *mspace_realloc_in_place(mspace msp, void *oldmem, size_t bytes);
void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_footprint(mspace msp);
size_t mspace_max_footprint(mspace msp);
size_t mspace_usable_size(const void *mem);
int mspace_mallopt(int, int);
size_t mspace_set_footprint_limit(mspace msp, size_t bytes);
struct mallinfo {                                       // dlmalloc's layout (MALLINFO_FIELD_TYPE size_t)
    size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
struct mallinfo mspace_mallinfo(mspace msp);
}

struct Arena {
    const char *name;
    uintptr_t base;
    size_t size;
    bool reserved;
    mspace ms;
    volatile LONG failed;                               // allocations that did not fit (logged once)
    volatile LONG hi;                                   // high-water mark: end of the highest block ever handed out
    size_t peak;                                        // most in use at any report (container_report)
    bool warned;                                        // past 75% (logged once)
};
// Arena layout (statesync.cpp reads it through container_arena). Below 2 GB everything stays in 0x20000000-0x30000000,
// free at DllMain time; gladiusport.dll sits at 0x30000000.
static Arena g_arenas[CTR_ARENAS] = {
    {"main", 0x20000000, 192 << 20},
    {"io", 0x2C000000, 16 << 20},
    {"phys", 0xA0000000, 512 << 20},
    {"local", 0x2D000000, 32 << 20},                    // machine-local heap blocks (sound / CRI / I-O code)
    {"lphys", 0xC0000000, 64 << 20},                    // machine-local "physical" blocks (CRI audio buffers)
};
static bool g_on;
static DWORD g_io_tls = TLS_OUT_OF_INDEXES;

// Live block tags: size + allocating game stack, kept outside game memory.
struct Tag {
    uint32_t size;
    bool local;                                         // machine-local: never overwritten by a state sync
    uint32_t stack[CTR_TAG_STACK];
};
static bool g_tags_on;
static std::unordered_map<uintptr_t, Tag> *g_tags;
static SRWLOCK g_tags_lock = SRWLOCK_INIT;

// Call sites whose allocations go to the local arenas. None any more: everything is synced; only explicit
// game_alloc_local blocks are machine-local.
static bool local_code(uint32_t ra)
{
    (void)ra;
    return false;
}

// True when a game caller on the stack is machine-local code.
static bool local_caller()
{
    uint32_t st[CTR_TAG_STACK];
    memstats_game_stack(st, CTR_TAG_STACK, _AddressOfReturnAddress());
    for (int i = 0; i < CTR_TAG_STACK; i++)
        if (st[i] && local_code(st[i]))
            return true;
    return false;
}

// Record a block's size and allocating stack.
static void tag_add(const void *p, size_t size, bool force_local = false)
{
    if (!g_tags_on)
        return;
    Tag t;
    t.size = (uint32_t)size;
    memstats_game_stack(t.stack, CTR_TAG_STACK, _AddressOfReturnAddress());
    t.local = force_local;
    for (int i = 0; i < CTR_TAG_STACK && !t.local; i++)
        t.local = t.stack[i] && local_code(t.stack[i]);
    AcquireSRWLockExclusive(&g_tags_lock);
    (*g_tags)[(uintptr_t)p] = t;
    ReleaseSRWLockExclusive(&g_tags_lock);
}

static void tag_remove(const void *p)
{
    if (!g_tags_on)
        return;
    AcquireSRWLockExclusive(&g_tags_lock);
    g_tags->erase((uintptr_t)p);
    ReleaseSRWLockExclusive(&g_tags_lock);
}

static void tag_resize(const void *p, size_t size)
{
    if (!g_tags_on)
        return;
    AcquireSRWLockExclusive(&g_tags_lock);
    auto it = g_tags->find((uintptr_t)p);
    if (it != g_tags->end())
        it->second.size = (uint32_t)size;
    ReleaseSRWLockExclusive(&g_tags_lock);
}

static int arena_of(const void *p);

// Machine-local blocks inside the synced arenas (masked by state syncs): (addr, size) pairs into `pairs` (room for
// `max`), sorted; returns the total (max 0 = just count).
int container_local_blocks_n(uint32_t *pairs, int max)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (!g_tags_on)
        return 0;
    AcquireSRWLockShared(&g_tags_lock);
    for (auto &kv : *g_tags) {
        int a = arena_of((const void *)kv.first);
        if (kv.second.local && (a == CTR_MAIN || a == CTR_PHYS))
            out.push_back({(uint32_t)kv.first, kv.second.size});
    }
    ReleaseSRWLockShared(&g_tags_lock);
    std::sort(out.begin(), out.end());
    for (int i = 0; i < (int)out.size() && i < max; i++)
        pairs[2 * i] = out[i].first, pairs[2 * i + 1] = out[i].second;
    return (int)out.size();
}

// Visit every live tagged block.
void container_tags(void (*cb)(uintptr_t addr, uint32_t size, const uint32_t *stack, void *ctx), void *ctx)
{
    if (!g_tags_on)
        return;
    AcquireSRWLockShared(&g_tags_lock);
    for (auto &kv : *g_tags)
        cb(kv.first, kv.second.size, kv.second.stack, ctx);
    ReleaseSRWLockShared(&g_tags_lock);
}

// Game-visible "process heap" handle, identical on every PC (the CRT and XAPI keep it in game memory). Never
// dereferenced.
static void *const FAKE_HEAP = (void *)0x2000F000;

// Reserve every arena (with write-watch, so state syncs re-hash only written pages).
static void reserve_all()
{
    for (Arena &a : g_arenas)
        a.reserved = VirtualAlloc((void *)a.base, a.size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS) == (void *)a.base;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        reserve_all();                                  // earliest point our code runs: before the exe's entry
    return TRUE;
}

bool container_on() { return g_on; }
void *container_fake_heap() { return FAKE_HEAP; }

// Shim state that is game state (input, handle table): 0x20001000, below the main mspace, synced with game memory.
void *container_shared_page() { return g_on ? (void *)0x20001000 : nullptr; }

// [mem] Container=1: commit the arenas and create the mspaces.
void container_init(const char *ini)
{
    HMODULE self = GetModuleHandleA("gladiusport.dll");
    port_log("container: gladiusport.dll at %p%s\n", self,
             (uintptr_t)self == 0x30000000 ? "" : " - NOT at its fixed base 0x30000000 (pointers into it will differ "
                                                  "between PCs)");
    if (!GetPrivateProfileIntA("mem", "Container", 0, ini)) {
        port_log("container: off ([mem] Container=0) - game memory comes from the Windows heap\n");
        return;
    }
    for (Arena &a : g_arenas) {
        if (!a.reserved) {
            port_log("container: could NOT reserve %s range %08X (+%u MB) - container off\n", a.name, (unsigned)a.base,
                     (unsigned)(a.size >> 20));
            return;
        }
    }
    // commit charge only: a page takes RAM once it is written
    for (Arena &a : g_arenas) {
        if (!VirtualAlloc((void *)a.base, a.size, MEM_COMMIT, PAGE_READWRITE)) {
            port_log("container: could NOT commit %s range %08X (+%u MB, error %u) - container off (pagefile too "
                     "small?)\n", a.name, (unsigned)a.base, (unsigned)(a.size >> 20), (unsigned)GetLastError());
            for (Arena &b : g_arenas)
                VirtualFree((void *)b.base, b.size, MEM_DECOMMIT);
            return;
        }
    }
    mspace_mallopt(-3 /* M_MMAP_THRESHOLD */, -1);      // never mmap large blocks outside the ranges
    for (Arena &a : g_arenas) {
        // the main range's first page stays out of the mspace: FAKE_HEAP points there
        uintptr_t start = a.base + (&a == &g_arenas[CTR_MAIN] ? 0x10000 : 0);
        a.ms = create_mspace_with_base((void *)start, a.size - (start - a.base), 1);
        mspace_set_footprint_limit(a.ms, mspace_footprint(a.ms));   // no growth: a full arena fails, never moves out
    }
    g_io_tls = TlsAlloc();
    g_tags = new std::unordered_map<uintptr_t, Tag>();     // always: state syncs need the machine-local blocks
    g_tags_on = true;
    g_on = true;
    port_log("container: ON - main %08X io %08X phys %08X, machine-local %08X / %08X (dlmalloc mspaces)\n",
             (unsigned)g_arenas[CTR_MAIN].base, (unsigned)g_arenas[CTR_IO].base, (unsigned)g_arenas[CTR_PHYS].base,
             (unsigned)g_arenas[CTR_LOCAL].base, (unsigned)g_arenas[CTR_LOCAL_PHYS].base);
}

// Mark this thread as an I/O thread: its blocks go to the io arena.
void container_set_io_thread()
{
    if (g_io_tls != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_io_tls, (void *)1);
}

static int arena_of(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < CTR_ARENAS; i++)
        if (a >= g_arenas[i].base && a < g_arenas[i].base + g_arenas[i].size)
            return i;
    return -1;
}

bool ctr_owns(const void *p) { return g_on && arena_of(p) >= 0; }

// Log an arena overflow (once).
static void full(Arena &a, size_t size)
{
    if (InterlockedIncrement(&a.failed) == 1)
        port_log("container: %s arena FULL (request %u bytes) - falling back outside the container; sessions will "
                 "desync\n", a.name, (unsigned)size);
}

// Tag a new block and raise the arena's high-water mark.
static void note(Arena &a, const void *p, size_t size)
{
    tag_add(p, size);
    LONG end = (LONG)((uintptr_t)p + size);
    for (LONG cur = a.hi; (ULONG)end > (ULONG)cur; cur = a.hi)
        if (InterlockedCompareExchange(&a.hi, end, cur) == cur)
            break;
}

// An arena's whole reserved range.
bool container_arena(int arena, uintptr_t *base, size_t *size)
{
    if (!g_on || arena < 0 || arena >= CTR_ARENAS)
        return false;
    *base = g_arenas[arena].base;
    *size = g_arenas[arena].size;
    return true;
}

// The used part of an arena: [base, high-water mark).
bool container_range(int arena, uintptr_t *base, uintptr_t *end)
{
    if (!g_on || arena < 0 || arena >= CTR_ARENAS)
        return false;
    *base = g_arenas[arena].base;
    *end = g_arenas[arena].hi ? (uintptr_t)(ULONG)g_arenas[arena].hi : *base;
    return true;
}

// Game heap allocation: io arena on I/O threads, else main.
void *ctr_alloc(size_t size, bool zero)
{
    Arena &a = g_arenas[g_io_tls != TLS_OUT_OF_INDEXES && TlsGetValue(g_io_tls) ? CTR_IO
                        : local_caller()                                           ? CTR_LOCAL
                                                                                   : CTR_MAIN];
    void *p = mspace_malloc(a.ms, size ? size : 1);
    if (!p) {
        full(a, size);
        return nullptr;
    }
    // Always zeroed: the game reads some fields it never initializes, and recycled blocks would differ per machine.
    (void)zero;
    memset(p, 0, size);
    note(a, p, size);
    return p;
}

// "Physical" allocation (page aligned).
void *ctr_phys_alloc(size_t size, size_t align)
{
    Arena &a = g_arenas[local_caller() ? CTR_LOCAL_PHYS : CTR_PHYS];
    void *p = mspace_memalign(a.ms, align < 4096 ? 4096 : align, size ? size : 1);
    if (!p)
        full(a, size);
    else
        note(a, p, size);
    return p;
}

size_t ctr_size(const void *p) { return mspace_usable_size(p); }

void ctr_free(void *p)
{
    int i = arena_of(p);
    if (i >= 0) {
        tag_remove(p);
        mspace_free(g_arenas[i].ms, p);
    }
}

void *ctr_realloc(void *p, size_t size, bool zero, bool in_place_only)
{
    int i = arena_of(p);
    if (i < 0)
        return nullptr;
    size_t old = mspace_usable_size(p);
    if (size <= old) {                                  // shrink in place, like the Xbox heap (see xapi_heap.cpp)
        mspace_realloc_in_place(g_arenas[i].ms, p, size ? size : 1);
        tag_resize(p, size);
        return p;                                       // if it could not split, the block just stays larger
    }
    void *n = in_place_only ? mspace_realloc_in_place(g_arenas[i].ms, p, size)
                            : mspace_realloc(g_arenas[i].ms, p, size);
    if (!n) {
        if (!in_place_only)
            full(g_arenas[i], size);
        return nullptr;
    }
    (void)zero;                                         // grown part zeroed, same reason as ctr_alloc
    memset((char *)n + old, 0, size - old);
    if (n != p)
        tag_remove(p);
    note(g_arenas[i], n, size);
    return n;
}

void container_report()
{
    if (!g_on)
        return;
    // Called every 60 s: per-arena usage and peak, with a warning past 75%.
    for (Arena &a : g_arenas) {
        struct mallinfo mi = mspace_mallinfo(a.ms);
        if ((size_t)mi.uordblks > a.peak)
            a.peak = mi.uordblks;
        port_log("container: %-4s in use %6.2f MB of %3u MB, peak %6.2f MB (%u free chunks)%s\n", a.name,
                 mi.uordblks / 1048576.0, (unsigned)(a.size >> 20), a.peak / 1048576.0, (unsigned)mi.ordblks,
                 a.failed ? " - OVERFLOWED" : "");
        if (!a.warned && a.peak > a.size / 4 * 3) {
            a.warned = true;
            port_log("container: WARNING %s arena past 75%% (peak %.2f of %u MB) - raise its size in container.cpp\n",
                     a.name, a.peak / 1048576.0, (unsigned)(a.size >> 20));
        }
    }
    // whole 32-bit process (game + renderer + driver + textures)
    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms))
        port_log("container: process address space %.0f MB used of %.0f MB\n",
                 (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0, ms.ullTotalVirtual / 1048576.0);
}

// Shim objects the game keeps pointers to (D3D resources, sound buffers, ...): main arena, in game order.
void *game_alloc(size_t size, bool zero)
{
    if (g_on) {
        if (void *p = mspace_malloc(g_arenas[CTR_MAIN].ms, size ? size : 1)) {
            if (zero)
                memset(p, 0, size);
            note(g_arenas[CTR_MAIN], p, size);
            return p;
        }
        full(g_arenas[CTR_MAIN], size);
    }
    return HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, size ? size : 1);
}

// Machine-local shim object: local arena, excluded from state syncs.
void *game_alloc_local(size_t size)
{
    if (g_on) {
        if (void *p = mspace_malloc(g_arenas[CTR_LOCAL].ms, size ? size : 1)) {
            memset(p, 0, size);
            note(g_arenas[CTR_LOCAL], p, size);
            tag_add(p, size, true);                     // overrides the tag note() just wrote
            return p;
        }
        full(g_arenas[CTR_LOCAL], size);
    }
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

void game_free(void *p)
{
    if (!p)
        return;
    if (ctr_owns(p))
        ctr_free(p);
    else
        HeapFree(GetProcessHeap(), 0, p);
}
