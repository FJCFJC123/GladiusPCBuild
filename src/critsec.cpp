// Xbox kernel critical sections, implemented on the Xbox RTL_CRITICAL_SECTION layout (28 bytes) because game code
// initializes them statically in that format. LockCount counts owners + waiters - 1; contention waits on an event.
#include "port.h"
#include <unordered_map>

struct XboxCritSec {
    uint8_t type, absolute, size, inserted;
    volatile LONG event;        // SignalState slot: unused (wait events: side table below)
    uint32_t wait_list[2];
    volatile LONG lock_count;
    LONG recursion;
    volatile LONG owner;        // virtual thread id (port_vtid: same on every netplay machine)
};
static_assert(sizeof(XboxCritSec) == 28, "Xbox RTL_CRITICAL_SECTION is 28 bytes");

// Wait events live in a side table keyed by address, not in the structure: a netplay state sync copies game
// memory between machines, and a Windows handle stored there would be invalid on the other PC.
static SRWLOCK g_cs_lock = SRWLOCK_INIT;
static std::unordered_map<const void *, HANDLE> *g_cs_events;

// The wait event for cs (created on first use).
static HANDLE cs_event(XboxCritSec *cs)
{
    AcquireSRWLockShared(&g_cs_lock);
    HANDLE e = nullptr;
    if (g_cs_events) {
        auto it = g_cs_events->find(cs);
        if (it != g_cs_events->end())
            e = it->second;
    }
    ReleaseSRWLockShared(&g_cs_lock);
    if (e)
        return e;
    AcquireSRWLockExclusive(&g_cs_lock);
    if (!g_cs_events)
        g_cs_events = new std::unordered_map<const void *, HANDLE>();
    HANDLE &slot = (*g_cs_events)[cs];
    if (!slot)
        slot = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    e = slot;
    ReleaseSRWLockExclusive(&g_cs_lock);
    return e;
}

extern "C" {

void __stdcall krnl_RtlInitializeCriticalSection(XboxCritSec *cs)
{
    cs->type = 1;               // SynchronizationEvent, as the Xbox kernel sets it
    cs->absolute = 0;
    cs->size = sizeof(XboxCritSec) / 4;
    cs->inserted = 0;
    cs->event = 0;
    cs->wait_list[0] = cs->wait_list[1] = (uint32_t)&cs->wait_list[0];
    cs->lock_count = -1;
    cs->recursion = 0;
    cs->owner = 0;
}

void __stdcall krnl_RtlEnterCriticalSection(XboxCritSec *cs)
{
    LONG me = (LONG)port_vtid();
    if (InterlockedIncrement(&cs->lock_count) == 0) {
        cs->owner = me;
        cs->recursion = 1;
        return;
    }
    if (cs->owner == me) {
        cs->recursion++;
        return;
    }
    int depth = port_lock_suspend();                   // blocked: other threads would run on Xbox
    WaitForSingleObject(cs_event(cs), INFINITE);
    port_lock_resume(depth);
    cs->owner = me;
    cs->recursion = 1;
}

void __stdcall krnl_RtlLeaveCriticalSection(XboxCritSec *cs)
{
    if (--cs->recursion) {
        InterlockedDecrement(&cs->lock_count);
        return;
    }
    cs->owner = 0;
    if (InterlockedDecrement(&cs->lock_count) >= 0)
        SetEvent(cs_event(cs));     // a waiter exists: hand the lock over
}

BOOLEAN __stdcall krnl_RtlTryEnterCriticalSection(XboxCritSec *cs)
{
    LONG me = (LONG)port_vtid();
    if (InterlockedCompareExchange(&cs->lock_count, 0, -1) == -1) {
        cs->owner = me;
        cs->recursion = 1;
        return 1;
    }
    if (cs->owner == me) {
        InterlockedIncrement(&cs->lock_count);
        cs->recursion++;
        return 1;
    }
    return 0;
}

}
