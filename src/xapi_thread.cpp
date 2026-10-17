// XAPI threads / synchronization / last error / time -> Windows. The Xbox functions are a Win32 subset
// with identical signatures; object handles are Windows handles throughout (whole family replaced).
#include "port.h"
#include "memstats.h"
#include "container.h"
#include "handles.h"
#include <intrin.h>

void dtrace(const char *fmt, ...);                      // gn_core.cpp: netplay session trace (no-op outside)

typedef ULONG (WINAPI *RtlNtStatusToDosErrorFn)(LONG);

// ---- deferred thread start ----------------------------------------------------------------------------
// On the single-core Xbox a new thread does not run until its creator blocks or yields; engine code relies
// on that (e.g. thread procs 0x1FDA30 / 0x2105B0 return -1 unless the owner set its ready flag right after
// CreateThread). Game threads are therefore created suspended and released when the creating thread next
// waits, sleeps, yields or presents - or after 50 ms. After that they run fully parallel on all cores.
struct PendingThread {
    HANDLE handle;
    DWORD creator;
    DWORD created_ms;
};
static PendingThread g_pending[32];
static int g_pending_count;
static SRWLOCK g_pending_lock = SRWLOCK_INIT;

static void resume_pending(bool all)
{
    if (!g_pending_count)
        return;
    DWORD me = GetCurrentThreadId(), now = GetTickCount();
    AcquireSRWLockExclusive(&g_pending_lock);
    for (int i = 0; i < g_pending_count;) {
        PendingThread &p = g_pending[i];
        if (all ? now - p.created_ms >= 50 : p.creator == me) {
            ResumeThread(p.handle);
            CloseHandle(p.handle);
            p = g_pending[--g_pending_count];
        } else {
            i++;
        }
    }
    ReleaseSRWLockExclusive(&g_pending_lock);
}

void port_thread_yield_point() { resume_pending(false); }

// Every blocking call is a point where the single-core Xbox would run other threads: release deferred
// thread starts and give up the CRI priority-lock for the duration (see cri_lock.cpp).
// Game-visible thread priorities (by thread id). A priority set on another thread takes effect for the
// exclusion (port_set_elevated) when that thread next runs: thread start or return from a blocking call.
struct ThreadPrio {
    DWORD tid;
    int prio;
};
static ThreadPrio g_prio[64];
static int g_prio_count;
static SRWLOCK g_prio_lock = SRWLOCK_INIT;

static void prio_store(DWORD tid, int p)
{
    AcquireSRWLockExclusive(&g_prio_lock);
    int i = 0;
    while (i < g_prio_count && g_prio[i].tid != tid)
        i++;
    if (i < 64) {
        g_prio[i] = {tid, p};
        if (i == g_prio_count)
            g_prio_count++;
    }
    ReleaseSRWLockExclusive(&g_prio_lock);
}

static int prio_load(DWORD tid)
{
    int p = 0;
    AcquireSRWLockShared(&g_prio_lock);
    for (int i = 0; i < g_prio_count; i++)
        if (g_prio[i].tid == tid)
            p = g_prio[i].prio;
    ReleaseSRWLockShared(&g_prio_lock);
    return p;
}

static void sync_elevation() { port_set_elevated(prio_load(GetCurrentThreadId()) > 0); }

// deterministic mode (gn_core.cpp): told when a game thread enters / leaves Sleep or SleepEx
void (*port_sleep_notify)(int enter);

struct Blocking {
    int depth;
    Blocking() { vh_mark_epoch(); resume_pending(false); depth = port_lock_suspend(); }
    ~Blocking() { port_lock_resume(depth); sync_elevation(); }
};
void port_thread_timer_tick() { resume_pending(true); }

extern "C" {

// Every object the game creates gets a virtual handle (handles.cpp): the numbers live in game memory and must be
// the same on every machine of a netplay session; each call below resolves them to the real handle.
HANDLE __stdcall xapi_CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, const char *name)
{
    return vh_new(VH_EVENT, CreateEventA(sa, manual, initial, name), manual, initial);
}
HANDLE __stdcall xapi_OpenEventA(DWORD access, BOOL inherit, const char *name)
{
    return vh_new(VH_EVENT, OpenEventA(access, inherit, name));
}
BOOL __stdcall xapi_SetEvent(HANDLE h) { return SetEvent(vh_real(h)); }
BOOL __stdcall xapi_ResetEvent(HANDLE h) { return ResetEvent(vh_real(h)); }
BOOL __stdcall xapi_PulseEvent(HANDLE h) { return PulseEvent(vh_real(h)); }
HANDLE __stdcall xapi_CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG maximum, const char *name)
{
    return vh_new(VH_SEMAPHORE, CreateSemaphoreA(sa, initial, maximum, name), (uint32_t)initial, (uint32_t)maximum);
}
BOOL __stdcall xapi_ReleaseSemaphore(HANDLE h, LONG count, LONG *prev)
{
    LONG before = 0;
    BOOL ok = ReleaseSemaphore(vh_real(h), count, &before);
    dtrace("  sem-release %p -> %p +%d (was %d) ok %d tid %u", h, vh_real(h), count, before, ok, GetCurrentThreadId());
    if (prev) *prev = before;
    return ok;
}
HANDLE __stdcall xapi_CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owner, const char *name)
{
    return vh_new(VH_MUTEX, CreateMutexA(sa, owner, name), owner);
}
BOOL __stdcall xapi_ReleaseMutex(HANDLE h) { return ReleaseMutex(vh_real(h)); }
DWORD __stdcall xapi_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    Blocking b;
    return vh_wait(h, ms, alertable);
}
DWORD __stdcall xapi_SignalObjectAndWait(HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable)
{
    Blocking b;
    return SignalObjectAndWait(vh_real(sig), vh_real(wait), ms, alertable);
}
DWORD __stdcall xapi_WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable)
{
    Blocking b;
    return vh_wait_multi(n, h, all, ms, alertable);
}
DWORD __stdcall xapi_SleepEx(DWORD ms, BOOL alertable)
{
    Blocking b;
    if (port_sleep_notify) port_sleep_notify(1);
    DWORD r = SleepEx(ms, alertable);
    if (port_sleep_notify) port_sleep_notify(0);
    return r;
}
DWORD __stdcall xapi_WaitForSingleObject(HANDLE h, DWORD ms)
{
    Blocking b;
    return vh_wait(h, ms, FALSE);
}
void __stdcall xapi_Sleep(DWORD ms)
{
    Blocking b;
    if (port_sleep_notify) port_sleep_notify(1);
    Sleep(ms);
    if (port_sleep_notify) port_sleep_notify(0);
}

DWORD __stdcall xapi_GetLastError() { return GetLastError(); }
void __stdcall xapi_SetLastError(DWORD e) { SetLastError(e); }
DWORD __stdcall xapi_XapiSetLastNTError(LONG status)
{
    static RtlNtStatusToDosErrorFn conv =
        (RtlNtStatusToDosErrorFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlNtStatusToDosError");
    DWORD e = conv ? conv(status) : ERROR_GEN_FAILURE;
    SetLastError(e);
    return e;
}
BOOL __stdcall xapi_GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *n, BOOL wait)
{
    int depth = wait ? port_lock_suspend() : 0;
    HANDLE ev = ov->hEvent;
    ov->hEvent = vh_real(ev);                           // Windows reads the event from the OVERLAPPED
    BOOL ok = GetOverlappedResult(vh_real(h), ov, n, wait);
    ov->hEvent = ev;
    if (wait)
        port_lock_resume(depth);
    static int traced;
    if (traced++ < 40)
        port_log("GetOverlappedResult(h %p, wait %d) -> %d, %u bytes, err %u\n", h, wait, ok, n ? *n : 0,
                 ok ? 0 : GetLastError());
    return ok;
}
BOOL __stdcall xapi_CloseHandle(HANDLE h)
{
    if (port_close_find_handle(h))
        return TRUE;
    port_forget_file_handle(vh_real(h));
    return vh_close(h);
}

BOOL __stdcall xapi_SetThreadPriority(HANDLE h, int p)
{
    h = vh_real(h);
    static int traced;
    if (traced++ < 60)
        port_log("SetThreadPriority(thread %u, %d) from thread %u\n", GetThreadId(h), p, GetCurrentThreadId());
    DWORD tid = GetThreadId(h);
    prio_store(tid, p);
    if (tid == GetCurrentThreadId())
        port_set_elevated(p > 0);
    return SetThreadPriority(h, p);
}
int __stdcall xapi_GetThreadPriority(HANDLE h) { return GetThreadPriority(vh_real(h)); }
BOOL __stdcall xapi_SetThreadPriorityBoost(HANDLE h, BOOL disable) { return SetThreadPriorityBoost(vh_real(h), disable); }
// Suspend/Resume are hand-off points between CRI threads (its server loop resumes the server thread and spins;
// the server suspends itself when done): on Xbox the other thread then gets the CPU, so the CRI lock is given up.
DWORD __stdcall xapi_SuspendThread(HANDLE h)
{
    Blocking b;
    return SuspendThread(vh_real(h));
}
DWORD __stdcall xapi_ResumeThread(HANDLE h)
{
    h = vh_real(h);
    Blocking b;
    DWORD r = ResumeThread(h);
    static int traced;
    if (traced++ < 20)
        port_log("ResumeThread(thread %u) -> prev count %u, from thread %u ret %p\n", GetThreadId(h), r,
                 GetCurrentThreadId(), _ReturnAddress());
    SwitchToThread();                                   // let the resumed thread take the lock first
    return r;
}
void __stdcall xapi_RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) { RaiseException(code, flags, n, args); }
DWORD __stdcall xapi_QueueUserAPC(PAPCFUNC fn, HANDLE h, ULONG_PTR data) { return QueueUserAPC(fn, vh_real(h), data); }
BOOL __stdcall xapi_SwitchToThread()
{
    Blocking b;
    return SwitchToThread();
}
void __stdcall xapi_ExitThread(DWORD code)
{
    port_lock_suspend();                               // Xbox: a priority "lock" ends with its thread
    port_restore_game_tls();                           // ntdll frees only its own TLS block
    ExitThread(code);
}
BOOL __stdcall xapi_GetExitCodeThread(HANDLE h, DWORD *code) { return GetExitCodeThread(vh_real(h), code); }
// Virtual thread ids. The engine keeps thread ids in game memory and compares them (the stream reader only reads
// if GetCurrentThreadId() == [0x4FCA14], its own id stored at startup): Windows ids are random per process, so after
// a netplay state sync copied the host's memory the client's reader saw a "foreign" id and skipped every request.
// Game threads get ids by creation order (main 0x1000, then 0x1004, 0x1008, ... - boot creates them in the same
// order everywhere); port threads that run game code (VBlank callback) get fixed ones.
static __declspec(thread) DWORD t_vtid;
static volatile LONG g_next_vtid = 0x1000;              // + 4 per game thread; 0x1000 = main (port_main.cpp)

DWORD port_vtid() { return t_vtid ? t_vtid : GetCurrentThreadId(); }
void port_set_vtid(DWORD v) { t_vtid = v; }

struct ThreadStart {
    LPTHREAD_START_ROUTINE fn;
    void *param;
    DWORD vtid;
};
static DWORD WINAPI thread_start(void *p)
{
    ThreadStart ts = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    port_fix_game_tls();                               // before any game code: a full-size game TLS block
    memstats_set_role(memstats_role_for_thread((uint32_t)ts.fn));
    if (memstats_role_for_thread((uint32_t)ts.fn) == MS_IO)
        container_set_io_thread();                     // its timing-dependent allocations stay out of the main arena
    sync_elevation();
    t_vtid = ts.vtid;
    DWORD r = ts.fn(ts.param);
    port_lock_suspend();                               // returning ends the thread like ExitThread
    port_restore_game_tls();
    return r;
}
HANDLE __stdcall xapi_CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *param,
                                   DWORD flags, DWORD *id)
{
    // Xbox: 0 = XBE default stack (header page, no longer mapped here). Windows threads get >= 256 KB.
    if (stack < 0x40000)
        stack = 0x40000;
    ThreadStart *ts = new ThreadStart{fn, param, (DWORD)InterlockedAdd(&g_next_vtid, 4)};
    DWORD vtid = ts->vtid;
    HANDLE t = CreateThread(sa, stack, thread_start, ts, CREATE_SUSPENDED, nullptr);
    if (id)
        *id = vtid;                                     // the game sees virtual ids only
    if (t)
        port_pin_game_thread(t);
    if (t)
        port_apply_watch(t);
    if (t && !(flags & CREATE_SUSPENDED)) {                 // start deferred until the creator yields
        HANDLE own;
        DuplicateHandle(GetCurrentProcess(), t, GetCurrentProcess(), &own, 0, FALSE, DUPLICATE_SAME_ACCESS);
        AcquireSRWLockExclusive(&g_pending_lock);
        if (g_pending_count < 32)
            g_pending[g_pending_count++] = {own, GetCurrentThreadId(), GetTickCount()};
        else
            ResumeThread(own), CloseHandle(own);
        ReleaseSRWLockExclusive(&g_pending_lock);
    }
    if (t && g_game_thread_count < 32) {
        HANDLE own = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                GetThreadId(t));
        g_game_threads[g_game_thread_count++] = {own, (uint32_t)fn};
    }
    port_log("CreateThread(start %p, param %p, stack %u) -> thread %u\n", fn, param, (DWORD)stack, t ? GetThreadId(t) : 0);
    return vh_new(VH_THREAD, t);
}
DWORD __stdcall xapi_GetCurrentThreadId() { return port_vtid(); }

DWORD __stdcall xapi_GetTickCount() { return GetTickCount(); }

// The engine reads the CPU timestamp counter directly (43 inline `cpuid; rdtsc` timers) and converts with
// QueryPerformanceFrequency, which on Xbox is a hard-coded 733 MHz. Here QPC *is* the timestamp counter and
// QPF is this machine's measured TSC rate, so inline rdtsc and QPC stay on one clock on any CPU (modern
// x86 TSCs are invariant: constant rate, synchronized across cores).
static LONGLONG measure_tsc_hz()
{
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    unsigned long long t0 = __rdtsc();
    do {
        QueryPerformanceCounter(&q1);
    } while (q1.QuadPart - q0.QuadPart < f.QuadPart / 20);          // 50 ms
    unsigned long long t1 = __rdtsc();
    LONGLONG hz = (LONGLONG)((double)(t1 - t0) * f.QuadPart / (double)(q1.QuadPart - q0.QuadPart));
    port_log("timestamp counter: %.3f MHz\n", hz / 1e6);
    return hz;
}

BOOL __stdcall xapi_QueryPerformanceCounter(LARGE_INTEGER *c)
{
    c->QuadPart = (LONGLONG)__rdtsc();
    return TRUE;
}
BOOL __stdcall xapi_QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    static LONGLONG hz = measure_tsc_hz();
    f->QuadPart = hz;
    return TRUE;
}
void __stdcall xapi_OutputDebugStringA(const char *s)
{
    OutputDebugStringA(s);
    port_log("[dbg] %s", s);
}

}
