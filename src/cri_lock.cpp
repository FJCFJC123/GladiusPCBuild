// CRI middleware (Sofdec/ADX) lock. On Xbox CRI "locks" by raising the current thread to TIME_CRITICAL priority
// (0x22C7E0 -> callback [0x567AA0]) and "unlocks" by restoring a single global saved priority (0x22C800 ->
// [0x567AA8]). On the single-core Xbox that means: while the holder *runs*, nothing else does - but when the
// holder sleeps or waits, other threads run. With several cores the priority trick excludes nothing, CRI
// threads race and the shared saved priority gets clobbered ("E02082301 : force unlocked" halt).
//
// Replacement with the same semantics: a recursive mutex that its owner gives up whenever it blocks (every
// wait/sleep entry point calls port_lock_suspend/port_lock_resume). Thread priorities are left alone.
#include "port.h"

static CRITICAL_SECTION g_cri_lock;
static INIT_ONCE g_cri_once = INIT_ONCE_STATIC_INIT;
static __declspec(thread) int t_cri_depth;
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

void __cdecl cri_Unlock()
{
    if (t_cri_depth <= 0)                                // unbalanced unlock (never locked here): ignore
        return;
    if (!--t_cri_depth)
        g_cri_owner = 0;
    LeaveCriticalSection(&g_cri_lock);
}

}

// Around a blocking call: release the CRI lock completely, return the depth to restore afterwards.
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

void port_lock_resume(int depth)
{
    for (int i = 0; i < depth; i++)
        EnterCriticalSection(&g_cri_lock);
    t_cri_depth = depth;
    if (depth)
        g_cri_owner = GetCurrentThreadId();
}

// Priority exclusion. On the single core a thread above normal priority (CRI servers at 1-2, code that boosts
// itself to TIME_CRITICAL) runs without interruption by normal-priority threads until it blocks, and the CRI
// libraries rely on it (server loops read state the main thread changes inside its lock). Such a thread holds
// one extra level of the CRI lock while it runs; blocking calls give it up and take it back like any level.
static __declspec(thread) bool t_elevated;

void port_set_elevated(bool on)
{
    if (on == t_elevated)
        return;
    t_elevated = on;
    if (on)
        cri_Lock();
    else
        cri_Unlock();
}
