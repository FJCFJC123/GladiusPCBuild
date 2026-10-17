// Virtual handles: the game gets VH_BASE + slot * 4 instead of Windows handles, so handle values in game memory are
// identical on every netplay machine. The slot table lives in the memory container's shared page and is synced with
// game memory; the real handle per slot stays local. After a sync, events / semaphores / mutexes are rebuilt from
// the host's table and files reopened; every wait on a virtual handle also wakes on a "rebind" event so blocked
// threads re-read their handle.
#include "port.h"
#include "handles.h"
#include "container.h"

static const int VH_SLOTS = 512;

struct VhShared {
    uint32_t kind;
    uint32_t a, b, c;               // event: manual, initial | semaphore: initial, maximum | mutex: owner
                                    // file: access, share, flags
    uint32_t state;                 // published before a sync: event signaled / semaphore count
    uint32_t pos_lo, pos_hi;        // file position (published before a sync)
    char path[100];                 // file: the game's Xbox path (mapped per machine on reopen)
};
static_assert(sizeof(VhShared) == 128, "slot layout");

static VhShared g_local_table[VH_SLOTS];
static HANDLE g_real[VH_SLOTS];
static uint32_t g_real_kind[VH_SLOTS];
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_rebind;                                 // manual-reset: set while waiters are being kicked
static volatile LONG g_waiters;                         // threads inside a rebind-aware wait
// Sync epoch, +1 per table rebuild. A wait that read its handle before a rebuild returns at once as if signaled,
// so the caller re-reads it. t_epoch is set where a thread leaves game code (xapi_thread.cpp, gn_hooks.cpp).
static volatile LONG g_epoch;
static __declspec(thread) LONG t_epoch = -1;

LONG vh_epoch() { return g_epoch; }
void vh_mark_epoch() { t_epoch = g_epoch; }
// True (once) when this thread's handle predates the last rebuild.
static bool stale_wait()
{
    LONG e = t_epoch;
    t_epoch = -1;
    return e != -1 && e != g_epoch;
}

static HANDLE rebind_event()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        g_rebind = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        return TRUE;
    }, nullptr, nullptr);
    return g_rebind;
}

// Wait on a virtual handle; also returns (as signaled) when the table is rebuilt.
DWORD vh_wait(HANDLE h, DWORD ms, BOOL alertable)
{
    if (stale_wait() && vh_is(h))
        return WAIT_OBJECT_0;
    if (!vh_is(h))
        return WaitForSingleObjectEx(h, ms, alertable);
    HANDLE w[2] = {vh_real(h), rebind_event()};
    InterlockedIncrement(&g_waiters);
    DWORD r = WaitForMultipleObjectsEx(2, w, FALSE, ms, alertable);
    InterlockedDecrement(&g_waiters);
    return r == WAIT_OBJECT_0 + 1 ? WAIT_OBJECT_0 : r;
}

DWORD vh_wait_multi(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable)
{
    HANDLE w[MAXIMUM_WAIT_OBJECTS];
    bool any_virtual = false, stale = stale_wait();
    if (n > MAXIMUM_WAIT_OBJECTS) n = MAXIMUM_WAIT_OBJECTS;
    for (DWORD i = 0; i < n; i++) {
        any_virtual |= vh_is(h[i]);
        w[i] = vh_real(h[i]);
    }
    if (stale && any_virtual)
        return WAIT_OBJECT_0;
    if (!any_virtual || all || n == MAXIMUM_WAIT_OBJECTS)
        return WaitForMultipleObjectsEx(n, w, all, ms, alertable);
    w[n] = rebind_event();
    InterlockedIncrement(&g_waiters);
    DWORD r = WaitForMultipleObjectsEx(n + 1, w, FALSE, ms, alertable);
    InterlockedDecrement(&g_waiters);
    return r == WAIT_OBJECT_0 + n ? WAIT_OBJECT_0 : r;
}

static VhShared *table()
{
    static VhShared *t;
    if (!t) {
        // shared page: input state at 0x20001000, this table from 0x20002000 to 0x2000F000 (416 slots)
        char *page = (char *)container_shared_page();
        t = page ? (VhShared *)(page + 0x1000) : g_local_table;
    }
    return t;
}
static int slots() { return container_shared_page() ? 0xD000 / (int)sizeof(VhShared) : VH_SLOTS; }

bool vh_is(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    return v >= VH_BASE && v < VH_BASE + VH_SLOTS * 4 && !(v & 3);
}

static int slot_of(HANDLE h) { return vh_is(h) ? (int)(((uintptr_t)h - VH_BASE) >> 2) : -1; }

// Wrap a real handle in a free slot (returns the real handle if the table is full).
HANDLE vh_new(uint32_t kind, HANDLE real, uint32_t a, uint32_t b, uint32_t c, const char *path)
{
    if (!real || real == INVALID_HANDLE_VALUE)
        return real;
    VhShared *t = table();
    AcquireSRWLockExclusive(&g_lock);
    int n = slots(), s = 0;
    while (s < n && t[s].kind != VH_FREE)
        s++;
    if (s == n) {
        ReleaseSRWLockExclusive(&g_lock);
        static bool warned;
        if (!warned) warned = true, port_log("handles: virtual handle table full - giving the game a real handle\n");
        return real;
    }
    VhShared &e = t[s];
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.a = a, e.b = b, e.c = c;
    if (path) strncpy(e.path, path, sizeof e.path - 1);
    g_real[s] = real;
    g_real_kind[s] = kind;
    ReleaseSRWLockExclusive(&g_lock);
    return (HANDLE)(VH_BASE + (uintptr_t)s * 4);
}

HANDLE vh_real(HANDLE h)
{
    int s = slot_of(h);
    if (s < 0)
        return h;                                       // pseudo handles (-1, -2), handles the port made itself
    HANDLE r = g_real[s];
    return r ? r : INVALID_HANDLE_VALUE;
}

uint32_t vh_kind(HANDLE h)
{
    int s = slot_of(h);
    return s < 0 ? VH_FREE : table()[s].kind;
}

BOOL vh_close(HANDLE h)
{
    int s = slot_of(h);
    if (s < 0)
        return CloseHandle(h);
    AcquireSRWLockExclusive(&g_lock);
    HANDLE r = g_real[s];
    g_real[s] = nullptr;
    g_real_kind[s] = VH_FREE;
    table()[s].kind = VH_FREE;
    ReleaseSRWLockExclusive(&g_lock);
    return r ? CloseHandle(r) : FALSE;
}

// Before a state sync: publish object states (event signaled, semaphore count, file position) into the table.
void vh_publish()
{
    VhShared *t = table();
    AcquireSRWLockShared(&g_lock);
    for (int s = 0, n = slots(); s < n; s++) {
        HANDLE r = g_real[s];
        if (!r) continue;
        switch (t[s].kind) {
        case VH_FILE: {
            LONG hi = 0;
            DWORD lo = SetFilePointer(r, 0, &hi, FILE_CURRENT);
            t[s].pos_lo = lo, t[s].pos_hi = (uint32_t)hi;
            break;
        }
        case VH_EVENT:                                  // a zero-timeout wait consumes an auto-reset signal: put it back
            t[s].state = WaitForSingleObject(r, 0) == WAIT_OBJECT_0;
            if (t[s].state && !t[s].a) SetEvent(r);
            break;
        case VH_SEMAPHORE: {                            // count = how many zero-timeout waits succeed; then restore
            uint32_t count = 0;
            while (count < 0x10000 && WaitForSingleObject(r, 0) == WAIT_OBJECT_0) count++;
            if (count) ReleaseSemaphore(r, (LONG)count, nullptr);
            t[s].state = count;
            break;
        }
        default: break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
}

// After a state sync replaced the table: recreate or reopen every slot on this machine, then wake all waiters.
void vh_reconcile()
{
    VhShared *t = table();
    int created = 0, reopened = 0, closed = 0, failed = 0;
    HANDLE retired[VH_SLOTS];
    int nretired = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (int s = 0, n = slots(); s < n; s++) {
        const VhShared &e = t[s];
        HANDLE &r = g_real[s];
        if (e.kind == VH_FREE) {
            if (r) CloseHandle(r), r = nullptr, closed++;
            g_real_kind[s] = VH_FREE;
            continue;
        }
        char local_path[MAX_PATH] = "";
        // the file this machine has open for that Xbox path (z:\gladius.bec is served from the disc file)
        if (e.kind == VH_FILE &&
            !port_map_path(_stricmp(e.path, "z:\\gladius.bec") ? e.path : "d:\\gladius.bec", local_path))
            local_path[0] = 0;
        bool keep = r && g_real_kind[s] == e.kind && (e.kind == VH_FILE || e.kind == VH_THREAD);
        if (keep && e.kind == VH_FILE) {                // same file? compare without the drive ("\\?\C:..." prefix)
            char mine[MAX_PATH] = "";
            DWORD len = GetFinalPathNameByHandleA(r, mine, MAX_PATH, 0);
            const char *want = local_path[0] && local_path[1] == ':' ? local_path + 2 : local_path;
            size_t lm = strlen(mine), lw = strlen(want);
            keep = len && want[0] && lm >= lw && !_stricmp(mine + lm - lw, want);
        }
        if (keep) {
            if (e.kind == VH_FILE) {
                LONG hi = (LONG)e.pos_hi;
                SetFilePointer(r, (LONG)e.pos_lo, &hi, FILE_BEGIN);
            }
            continue;
        }
        if (r) {
            if (e.kind == VH_FILE || g_real_kind[s] == VH_FILE) CloseHandle(r);
            else retired[nretired++] = r;               // a thread may still be waiting on it: close after the kick
            r = nullptr;
        }
        switch (e.kind) {
        case VH_EVENT: r = CreateEventA(nullptr, e.a, e.state ? TRUE : FALSE, nullptr); break;
        case VH_SEMAPHORE:
            r = CreateSemaphoreA(nullptr, (LONG)(e.state <= e.b ? e.state : e.b), (LONG)(e.b ? e.b : 0x7FFFFFFF), nullptr);
            break;
        case VH_MUTEX: r = CreateMutexA(nullptr, FALSE, nullptr); break;
        case VH_FILE:
            // same open path as the game's CreateFile (drive mapping, disc-mirror redirect, overlapped bookkeeping)
            r = port_open_file(e.path, e.a, e.b, OPEN_EXISTING, e.c, nullptr);
            if (r == INVALID_HANDLE_VALUE) r = nullptr;
            if (r) {
                LONG hi = (LONG)e.pos_hi;
                SetFilePointer(r, (LONG)e.pos_lo, &hi, FILE_BEGIN);
                reopened++;
            }
            break;
        default: break;                                 // threads: created at boot in the same order everywhere
        }
        if (r) {
            g_real_kind[s] = e.kind;
            if (e.kind != VH_FILE) created++;
        } else {
            failed++;
            port_log("handles: slot %d (kind %u%s%s) has no equivalent on this machine\n", s, e.kind,
                     e.path[0] ? ", " : "", e.path);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    // kick every thread waiting on a virtual handle: it returns to its loop and waits on the new object
    InterlockedIncrement(&g_epoch);
    LONG before = g_waiters;
    SetEvent(rebind_event());
    for (int i = 0; i < 200 && g_waiters > 0; i++)
        Sleep(1);
    ResetEvent(g_rebind);
    for (int i = 0; i < nretired; i++)
        CloseHandle(retired[i]);
    port_log("handles: rebuilt after state sync - %d created, %d files reopened, %d closed, %d unavailable, %ld "
             "waiters kicked (%ld still waiting)\n", created, reopened, closed, failed, before, g_waiters);
}
