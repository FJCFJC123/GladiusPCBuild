// Kernel time functions the Xbox CRT/XAPI call directly. Pure conversions with exact Windows equivalents
// (same LARGE_INTEGER 100 ns FILETIME scale and TIME_FIELDS layout).
#include "port.h"

typedef void (WINAPI *RtlTimeToTimeFieldsFn)(const LARGE_INTEGER *, void *);
typedef BOOLEAN (WINAPI *RtlTimeFieldsToTimeFn)(const void *, LARGE_INTEGER *);

static FARPROC ntdll_proc(const char *name)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
}

extern "C" {

void __stdcall krnl_KeQuerySystemTime(LARGE_INTEGER *t)
{
    GetSystemTimeAsFileTime((FILETIME *)t);
}

void __stdcall krnl_RtlTimeToTimeFields(const LARGE_INTEGER *t, void *fields)
{
    static RtlTimeToTimeFieldsFn fn = (RtlTimeToTimeFieldsFn)ntdll_proc("RtlTimeToTimeFields");
    fn(t, fields);
}

BOOLEAN __stdcall krnl_RtlTimeFieldsToTime(const void *fields, LARGE_INTEGER *t)
{
    static RtlTimeFieldsToTimeFn fn = (RtlTimeFieldsToTimeFn)ntdll_proc("RtlTimeFieldsToTime");
    return fn(fields, t);
}

// pure status-code translation (XAPI save-game error paths)
ULONG __stdcall krnl_RtlNtStatusToDosError(LONG status)
{
    typedef ULONG(WINAPI * Fn)(LONG);
    static Fn fn = (Fn)ntdll_proc("RtlNtStatusToDosError");
    return fn(status);
}

}
