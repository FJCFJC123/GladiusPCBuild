// CRI middleware (Sofdec/ADX) lock. On the single-core Xbox, CRI locks by raising thread priority, which excludes
// nothing on a multi-core PC. Replaced by a recursive mutex that its owner releases whenever it blocks
// (port_lock_suspend / port_lock_resume around every wait).
#include "port.h"
#include <intrin.h>

static CRITICAL_SECTION g_cri_lock;
static INIT_ONCE g_cri_once = INIT_ONCE_STATIC_INIT;
static __declspec(thread) int t_cri_depth;
static __declspec(thread) bool t_elevated;                          // holds one level while above normal priority
extern "C" volatile DWORD g_cri_owner = 0;                          // last thread that took the lock (watchdog)

static BOOL CALLBACK init_cri(PINIT_ONCE, PVOID, PVOID *)
{
    InitializeCriticalSectionAndSpinCount(&g_cri_lock, 4000);
    return TRUE;
}

extern "C" {

void __cdecl cri_Lock()
{
    InitOnceExecuteOnce(&g_cri_once, init_cri, nullptr, nullptr);
    EnterCriticalSection(&g_cri_lock);
    t_cri_depth++;
    g_cri_owner = GetCurrentThreadId();
}

static void unlock_one()
{
    if (!--t_cri_depth)
        g_cri_owner = 0;
    LeaveCriticalSection(&g_cri_lock);
}

void __cdecl cri_Unlock()
{
    // unbalanced unlock (never locked here): ignore. It must not take the level an elevated thread holds, or that
    // thread would run CRI servers unlocked from then on.
    if (t_cri_depth <= (t_elevated ? 1 : 0)) {
        static volatile LONG n;
        if (t_cri_depth > 0 && InterlockedIncrement(&n) <= 5)
            port_log("cri: unbalanced unlock on elevated thread %u from %p ignored\n", GetCurrentThreadId(),
                     _ReturnAddress());
        return;
    }
    unlock_one();
}

}

// Before a blocking call: release the CRI lock completely and return the depth to restore.
int port_lock_suspend()
{
    int depth = t_cri_depth;
    if (depth)
        g_cri_owner = 0;
    for (int i = 0; i < depth; i++)
        LeaveCriticalSection(&g_cri_lock);
    t_cri_depth = 0;
    return depth;
}

// After a blocking call: take the lock back to `depth`.
void port_lock_resume(int depth)
{
    for (int i = 0; i < depth; i++)
        EnterCriticalSection(&g_cri_lock);
    t_cri_depth = depth;
    if (depth)
        g_cri_owner = GetCurrentThreadId();
}

// Threads above normal priority ran uninterrupted on the Xbox until they blocked, and CRI relies on it. Such a
// thread holds one extra level of the CRI lock while it runs.
void port_set_elevated(bool on)
{
    if (on == t_elevated)
        return;
    t_elevated = on;
    if (on)
        cri_Lock();
    else if (t_cri_depth > 0)
        unlock_one();
}

// Movie audio output server (mwRna, 0x22B640): walks the 16 output slots (0x5AD940, 0xA8 bytes each) and services the
// live ones. It reads a slot's live flag before the service takes the CRI lock, so a slot that a movie stop or skip
// destroyed in between was serviced zeroed: divide by zero in mwlRnaAddWrPos (0x22A883). Replaced by the same walk
// with each slot checked and serviced under the lock.
static void __cdecl rna_server()
{
    volatile uint32_t &busy = *(volatile uint32_t *)0x567A88;
    if (busy == 1)
        return;
    if (!t_cri_depth) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 5)
            port_log("cri: movie audio server on thread %u without the CRI lock (elevated %d)\n", GetCurrentThreadId(),
                     t_elevated);
    }
    busy = 1;
    for (int i = 0; i < 16; i++) {
        uint8_t *slot = (uint8_t *)0x5AD940 + i * 0xA8;
        cri_Lock();
        if (*slot == 1)
            ((void(__cdecl *)(uint8_t *))0x22B4C0)(slot);
        cri_Unlock();
    }
    busy = 0;
}

void port_cri_fixes_init()
{
    static const uint8_t head[8] = {0x83, 0x3D, 0x88, 0x7A, 0x56, 0x00, 0x01, 0x74};   // cmp dword [567A88h],1; je
    uint8_t *p = (uint8_t *)0x22B640;
    if (memcmp(p, head, sizeof head)) {
        port_log("cri: unexpected bytes at 0022B640 - movie audio server fix not installed\n");
        return;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uint32_t)rna_server - (0x22B640 + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    port_log("cri: movie audio server checks each output under the CRI lock\n");
}
