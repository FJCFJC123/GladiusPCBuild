// CRI middleware (Sofdec/ADX) lock. On the single-core Xbox, CRI locks by raising thread priority, which excludes
// nothing on a multi-core PC. Replaced by a recursive mutex that its owner releases whenever it blocks
// (port_lock_suspend / port_lock_resume around every wait).
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
