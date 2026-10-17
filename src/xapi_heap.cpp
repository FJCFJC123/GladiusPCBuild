// XAPI heap -> Windows heap. Xbox XAPI carries its own copy of the NT Rtl heap; the CRT calls both the
// Win32-style wrappers and the Rtl functions directly. Same signatures/semantics as Windows.
//
// [debug] PageHeap=1: game blocks from the process heap instead get their own pages in one reserved region
// (block at the end of its pages, an uncommitted guard page after it). A free decommits the pages and the
// region is never reused, so any later touch of freed game memory faults at the culprit; the crash log then
// names the block (size, allocating and freeing game stacks) via pageheap_describe(). Double frees are logged
// and ignored. When the region is used up, allocations fall back to the process heap.
#include "port.h"
#include "xbe_addrs.h"
#include "memstats.h"
#include "container.h"
#include <unordered_map>
#include <intrin.h>

static bool g_ph_on;
static uintptr_t g_ph_base, g_ph_end, g_ph_next;
static SRWLOCK g_ph_lock = SRWLOCK_INIT;
struct PhBlock {
    uint32_t size;                                              // current size (in-place shrinks lower it)
    uint32_t cap;                                               // 16-rounded size at allocation: fixes the pages
    bool live;
    uint32_t alloc_stack[8], free_stack[8];
};
static std::unordered_map<uintptr_t, PhBlock> *g_ph_blocks;     // key = user pointer
static int g_ph_logged;

static bool ph_owns(const void *p) { return (uintptr_t)p >= g_ph_base && (uintptr_t)p < g_ph_end; }

void pageheap_init(const char *ini)
{
    if (!GetPrivateProfileIntA("debug", "PageHeap", 0, ini))
        return;
    if (container_on()) {
        port_log("pageheap: not used - the memory container owns the game heap\n");
        return;
    }
    for (SIZE_T mb : {1024, 768, 512, 256}) {
        void *r = VirtualAlloc(nullptr, mb << 20, MEM_RESERVE, PAGE_NOACCESS);
        if (r) {
            g_ph_base = g_ph_next = (uintptr_t)r;
            g_ph_end = g_ph_base + (mb << 20);
            break;
        }
    }
    if (!g_ph_base) {
        port_log("pageheap: could not reserve a region - off\n");
        return;
    }
    g_ph_blocks = new std::unordered_map<uintptr_t, PhBlock>();
    g_ph_on = true;
    port_log("pageheap: game heap blocks in %08X-%08X (freed pages decommitted, never reused)\n", (unsigned)g_ph_base,
             (unsigned)g_ph_end);
}

static void *ph_alloc(SIZE_T size, const void *sp)
{
    SIZE_T rounded = (size + 15) & ~(SIZE_T)15;
    SIZE_T pages = (rounded + 0xFFF) >> 12;
    if (!pages)
        pages = 1;
    AcquireSRWLockExclusive(&g_ph_lock);
    uintptr_t at = g_ph_next;
    if (at + ((pages + 1) << 12) > g_ph_end) {
        ReleaseSRWLockExclusive(&g_ph_lock);
        static bool warned;
        if (!warned)
            warned = true, port_log("pageheap: region full - falling back to the process heap\n");
        return nullptr;
    }
    g_ph_next = at + ((pages + 1) << 12);                       // + 1 guard page, never committed
    void *c = VirtualAlloc((void *)at, pages << 12, MEM_COMMIT, PAGE_READWRITE);
    uintptr_t user = at + (pages << 12) - rounded;              // ends at the guard page (16-byte aligned)
    if (c) {
        PhBlock &b = (*g_ph_blocks)[user];
        b.size = (uint32_t)size;
        b.cap = (uint32_t)rounded;
        b.live = true;
        memstats_game_stack(b.alloc_stack, 8, sp);
        memset(b.free_stack, 0, sizeof b.free_stack);
    }
    ReleaseSRWLockExclusive(&g_ph_lock);
    return c ? (void *)user : nullptr;
}

static SIZE_T ph_size(const void *p)
{
    AcquireSRWLockShared(&g_ph_lock);
    auto it = g_ph_blocks->find((uintptr_t)p);
    SIZE_T s = it != g_ph_blocks->end() && it->second.live ? it->second.size : (SIZE_T)-1;
    ReleaseSRWLockShared(&g_ph_lock);
    return s;
}

static BOOL ph_free(const void *p, const void *sp)
{
    uint32_t stack[8];
    memstats_game_stack(stack, 8, sp);
    AcquireSRWLockExclusive(&g_ph_lock);
    auto it = g_ph_blocks->find((uintptr_t)p);
    if (it == g_ph_blocks->end() || !it->second.live) {
        bool known = it != g_ph_blocks->end();
        PhBlock b = known ? it->second : PhBlock{};
        ReleaseSRWLockExclusive(&g_ph_lock);
        if (g_ph_logged++ < 50) {
            char now[96], first[96];
            memstats_fmt_stack(now, sizeof now, stack, 8);
            memstats_fmt_stack(first, sizeof first, b.free_stack, 8);
            port_log("pageheap: %s free of %p (size %u) - ignored\n  now   <-%s\n  first <-%s\n",
                     known ? "DOUBLE" : "UNKNOWN", p, b.size, now, first);
        }
        return FALSE;
    }
    PhBlock &b = it->second;
    b.live = false;
    memcpy(b.free_stack, stack, sizeof stack);
    uintptr_t rounded = b.cap;
    uintptr_t pages = ((rounded + 0xFFF) >> 12) ? (rounded + 0xFFF) >> 12 : 1;
    uintptr_t first_page = (uintptr_t)p + rounded - (pages << 12);
    VirtualFree((void *)first_page, pages << 12, MEM_DECOMMIT);
    ReleaseSRWLockExclusive(&g_ph_lock);
    return TRUE;
}

// Shrinks stay in place, like the Xbox / NT heap: the engine's tracked heap (0x1762A0..0x176308) shrinks a block
// with CRT realloc 0x27526E and IGNORES the returned pointer (its list keeps the old address). Growing in place
// fails (only asked for with HEAP_REALLOC_IN_PLACE_ONLY; a plain grow moves).
static bool ph_resize_in_place(const void *p, SIZE_T size)
{
    AcquireSRWLockExclusive(&g_ph_lock);
    auto it = g_ph_blocks->find((uintptr_t)p);
    bool ok = it != g_ph_blocks->end() && it->second.live && size <= it->second.size;
    if (ok)
        it->second.size = (uint32_t)size;
    ReleaseSRWLockExclusive(&g_ph_lock);
    return ok;
}

// crash log: which game block a faulting address belongs to (block = the last one starting at or before it)
void pageheap_describe(uintptr_t addr)
{
    if (!g_ph_on || addr < g_ph_base || addr >= g_ph_end)
        return;
    AcquireSRWLockShared(&g_ph_lock);
    uintptr_t best = 0;
    const PhBlock *bb = nullptr;
    for (auto &kv : *g_ph_blocks) {
        uintptr_t lo = kv.first & ~(uintptr_t)0xFFF;            // pages of a block start at its first page
        uintptr_t hi = kv.first + kv.second.cap + 0x1000;       // + guard page
        if (addr >= lo && addr < hi && kv.first >= best)
            best = kv.first, bb = &kv.second;
    }
    if (bb) {
        char a[96], f[96];
        memstats_fmt_stack(a, sizeof a, bb->alloc_stack, 8);
        memstats_fmt_stack(f, sizeof f, bb->free_stack, 8);
        port_log("pageheap: fault address %08X is %s game block %08X (+0x%X of %u bytes)\n  allocated <-%s\n  freed     <-%s\n",
                 (unsigned)addr, bb->live ? "past the end of LIVE" : "inside FREED", (unsigned)best,
                 (unsigned)(addr - best), bb->size, a, bb->live ? " (still live)" : f);
    } else {
        port_log("pageheap: fault address %08X is in the page heap region but no block covers it\n", (unsigned)addr);
    }
    ReleaseSRWLockShared(&g_ph_lock);
}

// The game sees container_fake_heap() as its process heap when the container is on (same value on every PC);
// Windows calls get the real one.
static HANDLE real_heap(HANDLE heap) { return heap == container_fake_heap() ? GetProcessHeap() : heap; }
static bool use_ctr(HANDLE heap) { return container_on() && heap == GetProcessHeap(); }
static bool use_ph(HANDLE heap) { return g_ph_on && heap == GetProcessHeap(); }

static int heap_kind(HANDLE heap) { return heap == GetProcessHeap() ? MS_HEAP : MS_PRIVHEAP; }

static SIZE_T block_size(HANDLE heap, DWORD flags, const void *p)
{
    if (ctr_owns(p))
        return ctr_size(p);
    return ph_owns(p) ? ph_size(p) : HeapSize(heap, flags & HEAP_NO_SERIALIZE, p);
}

static void stat_free(HANDLE heap, DWORD flags, const void *p)
{
    if (p && memstats_enabled()) {
        SIZE_T s = block_size(heap, flags, p);
        if (s != (SIZE_T)-1)
            memstats_free(heap_kind(heap), s);
    }
    if (heap == GetProcessHeap())
        heapcheck_free(p);
}

static void stat_alloc(HANDLE heap, const void *p, SIZE_T size)
{
    memstats_alloc(heap_kind(heap), p, size);
    if (heap == GetProcessHeap())
        heapcheck_alloc(p);
}

static void *raw_alloc(HANDLE heap, DWORD flags, SIZE_T size, const void *sp)
{
    if (use_ctr(heap)) {
        if (void *c = ctr_alloc(size, (flags & HEAP_ZERO_MEMORY) != 0))
            return c;
    }
    void *p = use_ph(heap) ? ph_alloc(size, sp) : nullptr;     // committed pages are already zero
    return p ? p : HeapAlloc(heap, flags, size);
}

static BOOL raw_free(HANDLE heap, DWORD flags, void *p, const void *sp)
{
    if (ctr_owns(p)) {
        ctr_free(p);
        return TRUE;
    }
    if (ph_owns(p))
        return ph_free(p, sp);
    return HeapFree(heap, flags, p);
}

extern "C" {

HANDLE __stdcall xapi_HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum)
{
    return HeapCreate(options & (HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS), initial, maximum);
}

BOOL __stdcall xapi_HeapDestroy(HANDLE heap) { return HeapDestroy(heap); }

BOOL __stdcall xapi_HeapFree(HANDLE heap, DWORD flags, void *p)
{
    heap = real_heap(heap);
    stat_free(heap, flags, p);
    return raw_free(heap, flags, p, _AddressOfReturnAddress());
}

HANDLE __stdcall xapi_GetProcessHeap() { return container_on() ? container_fake_heap() : GetProcessHeap(); }

void *__stdcall xapi_RtlAllocateHeap(HANDLE heap, DWORD flags, SIZE_T size)
{
    heap = real_heap(heap);
    void *p = raw_alloc(heap, flags, size, _AddressOfReturnAddress());
    stat_alloc(heap, p, size);
    return p;
}

BOOLEAN __stdcall xapi_RtlFreeHeap(HANDLE heap, DWORD flags, void *p)
{
    heap = real_heap(heap);
    stat_free(heap, flags, p);
    return raw_free(heap, flags, p, _AddressOfReturnAddress()) ? 1 : 0;
}

SIZE_T __stdcall xapi_RtlSizeHeap(HANDLE heap, DWORD flags, const void *p) { return block_size(real_heap(heap), flags, p); }

void *__stdcall xapi_RtlReAllocateHeap(HANDLE heap, DWORD flags, void *p, SIZE_T size)
{
    const void *sp = _AddressOfReturnAddress();
    heap = real_heap(heap);
    if (!p) {
        void *n = raw_alloc(heap, flags, size, sp);
        stat_alloc(heap, n, size);
        return n;
    }
    SIZE_T old = block_size(heap, flags, p);
    void *n;
    if (ctr_owns(p)) {                                  // container: shrinks stay in place (container.cpp)
        n = ctr_realloc(p, size, (flags & HEAP_ZERO_MEMORY) != 0, (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0);
        if (n) {
            memstats_free(heap_kind(heap), old);
            heapcheck_free(p);
            stat_alloc(heap, n, size);
        }
        return n;
    }
    if (ph_owns(p) && ((flags & HEAP_REALLOC_IN_PLACE_ONLY) || (old != (SIZE_T)-1 && size <= old))) {
        if (!ph_resize_in_place(p, size))
            return nullptr;
        if (memstats_enabled() && old != (SIZE_T)-1) {
            memstats_free(heap_kind(heap), old);
            memstats_alloc(heap_kind(heap), p, size);
        }
        return p;
    }
    if (ph_owns(p) || (use_ph(heap) && !(flags & HEAP_REALLOC_IN_PLACE_ONLY))) {   // page heap grow: move
        if (old == (SIZE_T)-1)
            return nullptr;
        n = raw_alloc(heap, flags & ~HEAP_ZERO_MEMORY, size, sp);
        if (!n)
            return nullptr;
        memcpy(n, p, old < size ? old : size);
        if ((flags & HEAP_ZERO_MEMORY) && size > old)
            memset((char *)n + old, 0, size - old);
        raw_free(heap, flags, p, sp);
    } else if (old != (SIZE_T)-1 && size <= old) {
        // Shrink in place, as on the Xbox. The engine's tracked heap (0x1762A0..0x176308) shrinks blocks with CRT
        // realloc and ignores the returned pointer; the Windows low-fragmentation heap MOVES small blocks to a
        // smaller size class on shrink and frees the old one -> the engine's list kept pointing at memory that
        // D3D11 then reused (crash at 0x176A5E, 2026-10-05/06 dumps). If the heap cannot shrink in place, the
        // block simply stays as it is (a larger block is harmless).
        n = HeapReAlloc(heap, flags | HEAP_REALLOC_IN_PLACE_ONLY, p, size);
        if (!n)
            n = p;
    } else {
        n = HeapReAlloc(heap, flags, p, size);
    }
    if (n) {
        memstats_free(heap_kind(heap), old);
        if (heap == GetProcessHeap())
            heapcheck_free(p);
        stat_alloc(heap, n, size);
    }
    return n;
}

}

void port_init_heap()
{
    *(HANDLE *)XBE_XAPI_PROCESS_HEAP = xapi_GetProcessHeap();  // XAPI code that reads XapiProcessHeap directly
}
