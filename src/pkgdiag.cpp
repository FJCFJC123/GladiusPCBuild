// Package lifetime diagnostics for the skinned-draw crash at 0x1EF5BA: the skinned mesh draw (0x1EF3E0) read a
// garbage mesh pointer from geometry in a data package that had been freed or overwritten. Completed packages record
// their buffer range and the destructor marks it freed; a draw that would crash is logged once and skipped.
#include "gn.h"
#include <string.h>
#include <intrin.h>

void port_log(const char *fmt, ...);
extern void (*port_readex_notify)(void *dest, DWORD n);   // xapi_file.cpp
void port_memwatch_set(int slot, uint32_t addr);          // port_main.cpp: DR2/DR3 write watches
uint32_t port_memwatch_get(int slot);
void port_write_dump(const char *why);                    // port_main.cpp: full-memory minidump, once per reason
extern volatile DWORD g_port_probe_tid;                   // port_main.cpp: thread doing a guarded read

namespace pkgdiag {

struct Pkg {
    uint32_t loader, buf, size;
    DWORD loadedAt, freedAt;
    bool freed;
    char name[28];
};
static Pkg g_pkgs[128];
static int g_next;
static CRITICAL_SECTION g_cs;
static int g_badLogged;

static const char *loader_name(const uint8_t *loader)
{
    const uint8_t *nm = loader ? *(const uint8_t *const *)(loader + 8) : NULL;
    return nm ? (const char *)(nm + 0xC) : "?";
}

void on_loaded(const uint8_t *loader, uint32_t size)     // package completion (gn_hooks.cpp pkg_cb_log)
{
    if (!loader) return;
    uint32_t buf = *(const uint32_t *)(loader + 0xC);
    if (!buf) return;
    EnterCriticalSection(&g_cs);
    Pkg &p = g_pkgs[g_next++ % 128];
    p.loader = (uint32_t)loader;
    p.buf = buf;
    p.size = size;
    p.loadedAt = GetTickCount();
    p.freed = false;
    p.freedAt = 0;
    strncpy(p.name, loader_name(loader), sizeof p.name - 1);
    p.name[sizeof p.name - 1] = 0;
    LeaveCriticalSection(&g_cs);
}

static void __cdecl on_destroy(const uint8_t *loader)
{
    uint32_t buf = *(const uint32_t *)(loader + 0xC);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 128; i++)
        if (g_pkgs[i].buf == buf && buf && !g_pkgs[i].freed) {
            g_pkgs[i].freed = true;
            g_pkgs[i].freedAt = GetTickCount();
        }
    LeaveCriticalSection(&g_cs);
    port_log("pkg: free '%s' buffer %08X (state %u, refs %u)\n", loader_name(loader), buf,
             *(const uint16_t *)(loader + 0x12), *(const uint16_t *)(loader + 0x10));
}

static const Pkg *find(uint32_t addr)
{
    const Pkg *best = NULL;
    for (int i = 0; i < 128; i++) {
        const Pkg &p = g_pkgs[i];
        if (p.buf && addr >= p.buf && addr < p.buf + p.size && (!best || p.loadedAt > best->loadedAt)) best = &p;
    }
    return best;
}

static bool readable(uint32_t addr, uint32_t len)
{
    if (addr < 0x10000) return false;
    g_port_probe_tid = GetCurrentThreadId();            // the crash handler ignores faults of this probe
    bool ok = true;
    __try {
        volatile uint8_t a = *(const uint8_t *)addr, b = *(const uint8_t *)(addr + len - 1);
        (void)a; (void)b;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_port_probe_tid = 0;
    return ok;
}

// Hardware write watches on geometry headers shared by 2+ models (mesh count +0x1C) while a watch slot is free.
struct HdrSeen { uint32_t hdr, models[3]; int n; };
static HdrSeen g_seen[64];
static int g_seenNext;

static void note_shared(uint32_t hdr, uint32_t model)
{
    uint32_t w = hdr + 0x1C;
    if (port_memwatch_get(0) == w || port_memwatch_get(1) == w) return;
    int slot = !port_memwatch_get(0) ? 0 : !port_memwatch_get(1) ? 1 : -1;
    if (slot < 0) return;
    HdrSeen *e = NULL;
    for (int i = 0; i < 64 && !e; i++) if (g_seen[i].hdr == hdr) e = &g_seen[i];
    if (!e) { e = &g_seen[g_seenNext++ % 64]; e->hdr = hdr; e->n = 0; }
    for (int i = 0; i < e->n; i++) if (e->models[i] == model) return;
    if (e->n < 3) e->models[e->n++] = model;
    if (e->n < 2) return;
    port_memwatch_set(slot, w);
    port_log("memwatch: slot %d armed on mesh count %08X (header %08X, value %08X, shared by %d+ models)\n", slot, w,
             hdr, *(const uint32_t *)w, e->n);
}

static void watch_freed(uint32_t block, uint32_t size)
{
    for (int s = 0; s < 2; s++) {
        uint32_t w = port_memwatch_get(s);
        if (w && w >= block && w < block + size) {
            port_memwatch_set(s, 0);
            port_log("memwatch: slot %d released - its header block %08X+%X was freed normally\n", s, block, size);
        }
    }
}

// 1 = draw it, 0 = skip (it would crash)
static int __cdecl draw_ok(const uint8_t *model, uint32_t flags)
{
    uint32_t hdr = readable((uint32_t)model, 0x20) ? *(const uint32_t *)(model + 0x10) : 0;
    const char *why = NULL;
    uint32_t bad = 0;
    int idx = -1;
    if (!readable(hdr, 0x30)) { why = "geometry header unreadable"; bad = hdr; }
    else {
        int count = *(const int *)(hdr + (flags & 1 ? 0x28 : 0x1C));
        uint32_t list = *(const uint32_t *)(hdr + (flags & 1 ? 0x2C : 0x20));
        if (count < 0 || count > 4096) { why = "mesh count out of range"; bad = (uint32_t)count; }
        else if (count && !readable(list, count * 4)) { why = "mesh list unreadable"; bad = list; }
        else {
            // per-model array parallel to the mesh list (model+0x18, flags & 1: +0x1C): entry -> +8 -> +0x14 is read per mesh
            uint32_t arr = *(const uint32_t *)(model + (flags & 1 ? 0x1C : 0x18));
            if (count && !readable(arr, count * 4)) { why = "model mesh-state array bad"; bad = arr; }
            for (int i = 0; i < count && !why; i++) {
                uint32_t m = ((const uint32_t *)list)[i];
                if (!readable(m, 0x34)) { why = "mesh pointer bad"; bad = m; idx = i; break; }
                uint32_t m8 = *(const uint32_t *)(m + 8);       // material: [m8+8] added to the draw stats
                if (!readable(m8, 0xC)) { why = "mesh material pointer bad"; bad = m8; idx = i; break; }
                int n = 0;                             // material/texture chain: node +0, +0x10, next +0xC
                for (uint32_t node = *(const uint32_t *)(m + 4); node && !why; node = *(const uint32_t *)(node + 0xC))
                    if (!readable(node, 0x14) || ++n > 64) { why = "mesh material chain bad"; bad = node; idx = i; }
                uint32_t st = ((const uint32_t *)arr)[i], st8;
                if (!why && (!readable(st, 0xC) || !readable(st8 = *(const uint32_t *)(st + 8), 0x18))) {
                    why = "mesh state entry bad"; bad = st; idx = i;
                }
            }
        }
    }
    if (!why) {
        if (!(flags & 1)) note_shared(hdr, (uint32_t)model);
        return 1;
    }
    if (port_memwatch_get(0) == hdr + 0x1C || port_memwatch_get(1) == hdr + 0x1C)
        port_log("memwatch: WATCHED header %08X is corrupt - see the WRITE lines above (none = written by the OS: "
                 "file I/O into a stale buffer)\n", hdr);
    if (g_badLogged < 30) {
        g_badLogged++;
        EnterCriticalSection(&g_cs);
        const Pkg *p = find(hdr);
        char where[160];
        if (p) _snprintf(where, sizeof where, "header is in package '%s' (%08X+%X) %s %u ms ago", p->name, p->buf,
                         p->size, p->freed ? "FREED" : "loaded", GetTickCount() - (p->freed ? p->freedAt : p->loadedAt));
        else _snprintf(where, sizeof where, "header not in any tracked package");
        where[sizeof where - 1] = 0;
        LeaveCriticalSection(&g_cs);
        port_log("draw: SKIPPED model %p (header %08X): %s %08X%s%d - %s\n", model, hdr, why, bad,
                 idx >= 0 ? " at mesh " : "", idx, where);
        if (g_badLogged == 1 && readable((uint32_t)model, 0x40)) {
            const uint32_t *w = (const uint32_t *)model;
            port_log("draw: model %p words: %08X %08X %08X %08X %08X %08X %08X %08X  %08X %08X %08X %08X %08X %08X %08X %08X\n",
                     model, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
            port_write_dump("baddraw");                 // full memory while the bad model is still live
        }
    }
    return 0;
}

// Frees while a stream read into the block is still queued: the late ReadFile would write into memory that now
// belongs to someone else (invisible to write watches, since the OS writes it).
struct PendingRead { uint32_t buf, size, freedBlock, freedSize; DWORD queuedAt; bool freed; char stack[200]; };
static PendingRead g_reads[64];
static volatile LONG g_nreads;
static int g_rwLogged;

void read_queued(uint32_t buf, uint32_t size, const char *stack)
{
    if (!buf || !size) return;
    EnterCriticalSection(&g_cs);
    int slot = -1;
    for (int i = 0; i < 64 && slot < 0; i++) if (!g_reads[i].buf) slot = i;
    if (slot >= 0) {
        PendingRead &r = g_reads[slot];
        r.buf = buf; r.size = size; r.freed = false; r.freedBlock = r.freedSize = 0;
        r.queuedAt = GetTickCount();
        strncpy(r.stack, stack, sizeof r.stack - 1);
        r.stack[sizeof r.stack - 1] = 0;
        InterlockedIncrement(&g_nreads);
    }
    LeaveCriticalSection(&g_cs);
}

static void __cdecl read_executing(void *dest, DWORD n)          // xapi_ReadFileEx (port_readex_notify)
{
    if (!g_nreads) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; i++) {
        PendingRead &r = g_reads[i];
        if (!r.buf || r.buf != (uint32_t)dest) continue;
        if (r.freed && g_rwLogged < 40) {
            g_rwLogged++;
            port_log("memdiag: READ INTO FREED MEMORY: %u bytes -> %08X (block %08X+%X freed after the read was "
                     "queued %u ms ago; queued from%s)\n", n, r.buf, r.freedBlock, r.freedSize,
                     GetTickCount() - r.queuedAt, r.stack);
        }
        r.buf = 0;
        InterlockedDecrement(&g_nreads);
        break;
    }
    LeaveCriticalSection(&g_cs);
}

static void check_free(uint32_t block, uint32_t size, const char *kind)
{
    if (!g_nreads || !block) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < 64; i++) {
        PendingRead &r = g_reads[i];
        if (!r.buf || r.freed) continue;
        if (r.buf < block + size && block < r.buf + r.size) {
            r.freed = true; r.freedBlock = block; r.freedSize = size;
            if (g_rwLogged < 40) {
                g_rwLogged++;
                const uint32_t *sp = (const uint32_t *)&block;
                char st[200]; int o = 0, found = 0;
                const uint32_t *top = (const uint32_t *)__readfsdword(4);    // stack base (TIB)
                for (int k = 0; k < 300 && sp + k < top && found < 10 && o < 180; k++) {
                    uint32_t v = sp[k];
                    if (v < 0x12000 || v > 0x2F0000) continue;
                    const uint8_t *c = (const uint8_t *)v;
                    if (c[-5] != 0xE8 && c[-6] != 0xFF && c[-2] != 0xFF && c[-3] != 0xFF) continue;
                    o += _snprintf(st + o, sizeof st - o, " %06X", v); found++;
                }
                st[o < (int)sizeof st ? o : (int)sizeof st - 1] = 0;
                port_log("memdiag: %s free of %08X+%X while a stream read into %08X+%X is still queued "
                         "(queued from%s; freed from%s)\n", kind, block, size, r.buf, r.size, r.stack, st);
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void __cdecl on_heap_free(uint32_t p)     // 0x176A30: tracked heap, header at p-0x20, size at p-0x14
{
    if (!p) return;
    uint32_t size = *(const uint32_t *)(p - 0x14);
    if (port_memwatch_get(0) || port_memwatch_get(1)) watch_freed(p, size);
    if (g_nreads) check_free(p, size, "heap");
}

static void __cdecl on_pool_free(uint32_t p)     // 0x176360: small-block pools
{
    if (!p) return;
    if (port_memwatch_get(0) || port_memwatch_get(1)) watch_freed(p, 0x400);
    if (g_nreads) check_free(p, 0x1000, "pool");
}

// ── hooks ───────────────────────────────────────────────────────────────
static __declspec(naked) void draw_hook()
{
    __asm {
        push ecx
        push dword ptr [esp + 0x10]     // flags
        push dword ptr [esp + 0x0C]     // model
        call draw_ok
        add esp, 8
        pop ecx
        test eax, eax
        jz skip
        push ebp                        // original 0x1EF3E0: push ebp; mov ebp,esp; and esp,-16
        mov ebp, esp
        and esp, 0xFFFFFFF0
        push 0x1EF3E6
        ret
    skip:
        ret 0xC
    }
}

static __declspec(naked) void heap_free_hook()
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call on_heap_free
        add esp, 4
        pop ecx
        mov eax, dword ptr [esp + 4]    // original 0x176A30: mov eax,[esp+4]; test eax,eax
        test eax, eax
        push 0x176A36
        ret
    }
}

static __declspec(naked) void pool_free_hook()
{
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call on_pool_free
        add esp, 4
        pop ecx
        mov edx, dword ptr [esp + 4]    // original 0x176360: mov edx,[esp+4]; push ebx
        push ebx
        push 0x176365
        ret
    }
}

static __declspec(naked) void destroy_hook()
{
    __asm {
        push ecx
        push ecx
        call on_destroy
        add esp, 4
        pop ecx
        push -1                         // original 0x1E6410: push -1; push 0x298DFB
        push 0x298DFB
        push 0x1E6417
        ret
    }
}

static bool patch(uint32_t va, const char *hex, void *target)
{
    int len = (int)strlen(hex) / 2;
    for (int i = 0; i < len; i++) {
        unsigned b;
        sscanf(hex + i * 2, "%2x", &b);
        if (((const uint8_t *)va)[i] != b) {
            port_log("pkgdiag: unexpected bytes at %08X - not installed\n", va);
            return false;
        }
    }
    uint8_t *w = (uint8_t *)va;
    w[0] = 0xE9;
    *(int32_t *)(w + 1) = (int32_t)((uint32_t)target - (va + 5));
    for (int i = 5; i < len; i++) w[i] = 0x90;
    return true;
}

void install()
{
    InitializeCriticalSection(&g_cs);
    patch(0x1EF3E0, "558bec83e4f0", draw_hook);
    patch(0x1E6410, "6aff68fb8d2900", destroy_hook);
    patch(0x176A30, "8b44240485c0", heap_free_hook);
    patch(0x176360, "8b54240453", pool_free_hook);
    port_readex_notify = read_executing;
}

}  // namespace pkgdiag
