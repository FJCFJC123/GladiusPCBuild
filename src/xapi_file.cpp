// XAPI file I/O -> Windows (same Win32 signatures). Xbox drives map to PC folders (gladiusport.ini):
//   D: game disc -> GameDir (default: exe folder)   T: title data -> SaveDir\T
//   U: saves     -> SaveDir\U                       Z: utility cache -> SaveDir\Z
#include "port.h"
#include "handles.h"
#include <set>

static char g_game_dir[MAX_PATH], g_save_dir[MAX_PATH];
static std::set<HANDLE> g_find_handles;
static CRITICAL_SECTION g_find_lock;

static void exe_dir(char *out)
{
    GetModuleFileNameA(nullptr, out, MAX_PATH);
    char *s = strrchr(out, 0x5C);
    if (s)
        *s = 0;
}

void port_init_files()
{
    InitializeCriticalSection(&g_find_lock);
    char ini[MAX_PATH], dir[MAX_PATH];
    exe_dir(dir);
    _snprintf(ini, MAX_PATH, "%s\\gladiusport.ini", dir);
    GetPrivateProfileStringA("paths", "GameDir", dir, g_game_dir, MAX_PATH, ini);
    char def_save[MAX_PATH];
    _snprintf(def_save, MAX_PATH, "%s\\save", dir);
    GetPrivateProfileStringA("paths", "SaveDir", def_save, g_save_dir, MAX_PATH, ini);
    for (const char *sub : {"", "\\T", "\\U", "\\Z", "\\Z1", "\\Z2", "\\Z3"}) {   // Z<N>: -gnpad=N instance
        char p[MAX_PATH];
        _snprintf(p, MAX_PATH, "%s%s", g_save_dir, sub);
        CreateDirectoryA(p, nullptr);
    }
    port_log("paths: D: -> %s   T:/U:/Z: -> %s\\{T,U,Z}\n", g_game_dir, g_save_dir);
}

// Netplay save exchange (gladiusnet.dll): U: temporarily shows another player's saves.
static char g_u_override[MAX_PATH];
static void (*g_u_resolver)();                          // netplay: re-picks the override before any U: access

void port_set_u_resolver(void (*fn)()) { g_u_resolver = fn; }

const char *port_game_dir() { return g_game_dir; }     // D: (mods.cpp scans its data\ folder)

const char *port_u_dir()
{
    static char dir[MAX_PATH];
    _snprintf(dir, MAX_PATH, "%s\\U", g_save_dir);
    return dir;
}

void port_set_u_override(const char *dir)
{
    if (dir)
        strncpy(g_u_override, dir, MAX_PATH - 1);
    else
        g_u_override[0] = 0;
    port_log("paths: U: -> %s\n", dir ? dir : port_u_dir());
}

// Xbox path -> PC path. Returns false for paths we cannot map.
static bool map_path(const char *in, char *out)
{
    if (!in)
        return false;
    if (!strncmp(in, "\\??\\", 4))                      // NT DOS-device prefix ("\??\U:\...", XAPI save code)
        in += 4;
    const char *rest = in;
    const char *root = nullptr;
    char sub[4] = {};
    if (in[0] && in[1] == ':') {
        char d = (char)toupper((unsigned char)in[0]);
        rest = in + 2;
        if (d == 'D')
            root = g_game_dir;
        else if (d == 'U' && (g_u_resolver && (g_u_resolver(), 0), g_u_override[0]))
            root = g_u_override;
        else if (d == 'T' || d == 'U' || d == 'Z') {
            root = g_save_dir;
            sub[0] = 0x5C, sub[1] = d;
            // a second instance on this PC (-gnpad=N) keeps its own cache: Z<N>
            static int inst = -1;
            if (inst < 0) { const char *p = strstr(GetCommandLineA(), "-gnpad="); inst = p ? atoi(p + 7) : 0; }
            if (d == 'Z' && inst > 0 && inst < 10) sub[2] = (char)('0' + inst);
        } else {
            port_log("file: unmapped drive in '%s'\n", in);
            return false;
        }
    } else {
        root = g_game_dir;                              // relative: disc root
    }
    while (*rest == 0x5C || *rest == '/')
        rest++;
    _snprintf(out, MAX_PATH, "%s%s\\%s", root, sub, rest);
    out[MAX_PATH - 1] = 0;
    for (char *c = out; *c; c++)
        if (*c == '/')
            *c = 0x5C;
    return true;
}

// handles.cpp: map an Xbox path through this machine's drive mapping
bool port_map_path(const char *xbox, char *out) { return map_path(xbox, out); }

static int g_open_log = 0;

// Handles opened with FILE_FLAG_OVERLAPPED. Xbox ReadFileEx / WriteFileEx work on any handle; for the others the
// transfer runs synchronously and the completion routine is queued as an APC.
static std::set<HANDLE> g_overlapped_handles;
static std::set<HANDLE> g_mirror_handles;               // real handles: z:\gladius.bec served from the disc file
static SRWLOCK g_mirror_lock = SRWLOCK_INIT;
static SRWLOCK g_overlapped_lock = SRWLOCK_INIT;

static bool is_overlapped(HANDLE h)
{
    AcquireSRWLockShared(&g_overlapped_lock);
    bool r = g_overlapped_handles.count(h) != 0;
    ReleaseSRWLockShared(&g_overlapped_lock);
    return r;
}

void port_note_overlapped_handle(HANDLE h)
{
    AcquireSRWLockExclusive(&g_overlapped_lock);
    g_overlapped_handles.insert(h);
    ReleaseSRWLockExclusive(&g_overlapped_lock);
}

void port_forget_file_handle(HANDLE h)
{
    AcquireSRWLockExclusive(&g_mirror_lock);
    g_mirror_handles.erase(h);
    ReleaseSRWLockExclusive(&g_mirror_lock);
    AcquireSRWLockExclusive(&g_overlapped_lock);
    g_overlapped_handles.erase(h);
    ReleaseSRWLockExclusive(&g_overlapped_lock);
}

void dtrace(const char *fmt, ...);                      // gn_core.cpp: netplay session trace (no-op outside)

// memory diagnostics (pkgdiag.cpp): told right before a ReadFileEx writes into its buffer
void (*port_readex_notify)(void *dest, DWORD n);

struct QueuedCompletion {
    LPOVERLAPPED_COMPLETION_ROUTINE cb;
    DWORD err, bytes;
    OVERLAPPED *ov;
};

static void CALLBACK run_completion(ULONG_PTR p)
{
    QueuedCompletion *q = (QueuedCompletion *)p;
    q->cb(q->err, q->bytes, q->ov);
    HeapFree(GetProcessHeap(), 0, q);
}

// Every transfer completes before the call returns: Windows must never write into a game buffer after the game saw
// the request complete. The game still gets its normal completion signals, just immediately.
static BOOL complete_transfer(bool write, HANDLE h, void *buf, DWORD n, const OVERLAPPED *at, DWORD *done)
{
    OVERLAPPED o = {};
    o.Offset = at->Offset;
    o.OffsetHigh = at->OffsetHigh;
    *done = 0;
    if (!is_overlapped(h))
        return write ? WriteFile(h, buf, n, done, &o) : ReadFile(h, buf, n, done, &o);
    static __declspec(thread) HANDLE t_event;           // per-thread completion event for our own OVERLAPPED
    if (!t_event)
        t_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    o.hEvent = t_event;
    BOOL ok = write ? WriteFile(h, buf, n, nullptr, &o) : ReadFile(h, buf, n, nullptr, &o);
    if (ok)
        return GetOverlappedResult(h, &o, done, FALSE);
    if (GetLastError() != ERROR_IO_PENDING)
        return FALSE;
    int depth = port_lock_suspend();                    // blocking: give up the CRI priority lock meanwhile
    ok = GetOverlappedResult(h, &o, done, TRUE);
    DWORD err = ok ? 0 : GetLastError();
    port_lock_resume(depth);
    SetLastError(err);
    return ok;
}

// Complete the transfer, then queue the completion routine as an APC.
static BOOL sync_transfer_with_apc(bool write, HANDLE h, void *buf, DWORD n, OVERLAPPED *ov,
                                   LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    DWORD done = 0;
    BOOL ok = complete_transfer(write, h, buf, n, ov, &done);
    DWORD err = ok ? 0 : GetLastError();
    if (!ok && err != ERROR_HANDLE_EOF)
        return FALSE;                                   // failed to start: no completion, like Windows
    ov->Internal = ok ? 0 : 0xC0000011;                 // STATUS_END_OF_FILE
    ov->InternalHigh = done;
    static int short_logged;
    if ((err || done != n) && short_logged < 60) {
        short_logged++;
        port_log("%sFileEx(h %p, %u bytes at %08X) -> %u bytes, err %u%s\n", write ? "Write" : "Read", h, n, ov->Offset,
                 done, err, cb ? "" : " (no completion routine)");
    }
    if (cb) {
        QueuedCompletion *q = (QueuedCompletion *)HeapAlloc(GetProcessHeap(), 0, sizeof *q);
        *q = {cb, err, done, ov};
        QueueUserAPC(run_completion, GetCurrentThread(), (ULONG_PTR)q);
    }
    SetLastError(0);
    return TRUE;
}

// The engine's hard-disk cache z:\gladius.bec is a same-offset mirror of d:\gladius.bec. Here it is served straight
// from the disc file (reads always correct, writes dropped), so a netplay sync of the cache's block map can't break it.

static bool is_mirror(HANDLE h)
{
    AcquireSRWLockShared(&g_mirror_lock);
    bool r = g_mirror_handles.count(h) != 0;
    ReleaseSRWLockShared(&g_mirror_lock);
    return r;
}

extern "C" {

// Real open of an Xbox path (drive mapping, z:\gladius.bec -> disc file, overlapped bookkeeping), also used by
// handles.cpp to reopen synced files.
HANDLE port_open_file(const char *name, DWORD access, DWORD share, DWORD disp, DWORD flags, char *mapped)
{
    char path[MAX_PATH];
    if (!map_path(name, path)) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    flags &= ~FILE_FLAG_NO_BUFFERING;
    bool mirror = !_stricmp(name, "z:\\gladius.bec");
    if (mirror) {
        map_path("d:\\gladius.bec", path);
        access = GENERIC_READ;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        disp = OPEN_EXISTING;
    }
    if (mapped)
        strcpy(mapped, path);
    HANDLE h = CreateFileA(path, access, share | FILE_SHARE_READ, nullptr, disp, flags, nullptr);
    if (h != INVALID_HANDLE_VALUE && mirror) {
        AcquireSRWLockExclusive(&g_mirror_lock);
        g_mirror_handles.insert(h);
        ReleaseSRWLockExclusive(&g_mirror_lock);
    }
    if (h != INVALID_HANDLE_VALUE && (flags & FILE_FLAG_OVERLAPPED))
        port_note_overlapped_handle(h);
    return h;
}

HANDLE __stdcall xapi_CreateFileA(const char *name, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp,
                                  DWORD flags, HANDLE templ)
{
    char path[MAX_PATH] = "";
    HANDLE h = port_open_file(name, access, share, disp, flags, path);   // no sector alignment needed on PC
    if (!path[0])
        return INVALID_HANDLE_VALUE;                    // unmapped drive (error already set)
    bool save_path = strstr(path, "\\save\\") != nullptr;        // save data: always logged
    if (g_open_log < 400 || h == INVALID_HANDLE_VALUE || save_path) {
        g_open_log++;
        port_log("open %s %s%s-> %s%s  = %p\n", name, (access & GENERIC_WRITE) ? "(w) " : "",
                 (flags & FILE_FLAG_OVERLAPPED) ? "(async) " : "", path,
                 h == INVALID_HANDLE_VALUE ? "  [NOT FOUND]" : "", h);
    }
    // the table keeps the Xbox path: each machine maps it itself
    return vh_new(VH_FILE, h, access, share, flags, name);
}

static int g_io_fail_log = 0;
static void io_fail(const char *what, HANDLE h, DWORD n)
{
    if (g_io_fail_log++ < 50)
        port_log("%s(handle %p, %u bytes) failed: error %u\n", what, h, n, GetLastError());
}

// Overlapped ReadFile / WriteFile: completed immediately, results published like a finished async request.
static BOOL complete_overlapped(bool write, HANDLE h, void *buf, DWORD n, DWORD *out, OVERLAPPED *ov)
{
    DWORD done = 0;
    BOOL ok = complete_transfer(write, h, buf, n, ov, &done);
    DWORD err = ok ? 0 : GetLastError();
    ov->Internal = ok ? 0 : err == ERROR_HANDLE_EOF ? 0xC0000011 : 0xC0000001;   // STATUS_END_OF_FILE / UNSUCCESSFUL
    ov->InternalHigh = done;
    if (out)
        *out = done;
    if (ov->hEvent)
        SetEvent(vh_real(ov->hEvent));
    SetLastError(err);
    return ok;
}

// A game OVERLAPPED's hEvent is virtual: Windows gets a copy with the real one.
static BOOL native_rw(bool write, HANDLE h, void *buf, DWORD n, DWORD *done, OVERLAPPED *ov)
{
    if (!ov)
        return write ? WriteFile(h, buf, n, done, nullptr) : ReadFile(h, buf, n, done, nullptr);
    HANDLE ev = ov->hEvent;
    ov->hEvent = vh_real(ev);
    BOOL ok = write ? WriteFile(h, buf, n, done, ov) : ReadFile(h, buf, n, done, ov);
    DWORD err = GetLastError();
    ov->hEvent = ev;
    SetLastError(err);
    return ok;
}

BOOL __stdcall xapi_ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, OVERLAPPED *ov)
{
    h = vh_real(h);
    if (ov && is_overlapped(h)) {
        BOOL ok = complete_overlapped(false, h, buf, n, read, ov);
        if (!ok && GetLastError() != ERROR_HANDLE_EOF)
            io_fail("ReadFile", h, n);
        return ok;
    }
    BOOL ok = native_rw(false, h, buf, n, read, ov);
    static int traced;
    if (ov && traced++ < 40)
        port_log("ReadFile(h %p, %u bytes, ov offset %08X, event %p) -> %d err %u\n", h, n, ov->Offset, ov->hEvent, ok,
                 ok ? 0 : GetLastError());
    if (!ok && GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_HANDLE_EOF)
        io_fail("ReadFile", h, n);
    return ok;
}
BOOL __stdcall xapi_WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, OVERLAPPED *ov)
{
    h = vh_real(h);
    if (is_mirror(h)) {                                 // disk cache served from the disc: nothing to store
        if (written) *written = n;
        if (ov) {
            ov->Internal = 0;
            ov->InternalHigh = n;
            if (ov->hEvent) SetEvent(vh_real(ov->hEvent));
        }
        return TRUE;
    }
    if (ov && is_overlapped(h)) {
        BOOL ok = complete_overlapped(true, h, (void *)buf, n, written, ov);
        if (!ok)
            io_fail("WriteFile", h, n);
        return ok;
    }
    BOOL ok = native_rw(true, h, (void *)buf, n, written, ov);
    static int traced;
    if (traced++ < 40)
        port_log("WriteFile(h %p, %u bytes, ov %p) -> %d err %u\n", h, n, ov, ok, ok ? 0 : GetLastError());
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        io_fail("WriteFile", h, n);
    return ok;
}
// I/O trace: completion routines are wrapped so each APC delivery is logged (first g_io_trace_max ops).
static int g_io_trace = 0;
static const int g_io_trace_max = 60;
struct IoTrace {
    OVERLAPPED *ov;
    LPOVERLAPPED_COMPLETION_ROUTINE cb;
};
static IoTrace g_io_pending[64];
static SRWLOCK g_io_lock = SRWLOCK_INIT;

static void WINAPI traced_completion(DWORD err, DWORD bytes, OVERLAPPED *ov)
{
    LPOVERLAPPED_COMPLETION_ROUTINE cb = nullptr;
    AcquireSRWLockExclusive(&g_io_lock);
    for (auto &p : g_io_pending)
        if (p.ov == ov) {
            cb = p.cb;
            p.ov = nullptr;
            break;
        }
    ReleaseSRWLockExclusive(&g_io_lock);
    port_log("  io complete: ov %p err %u bytes %u -> cb %p (thread %u)\n", ov, err, bytes, cb, GetCurrentThreadId());
    if (cb)
        cb(err, bytes, ov);
}

static LPOVERLAPPED_COMPLETION_ROUTINE trace_io(const char *what, HANDLE h, DWORD n, OVERLAPPED *ov,
                                                LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    if (g_io_trace >= g_io_trace_max || !cb)
        return cb;
    g_io_trace++;
    AcquireSRWLockExclusive(&g_io_lock);
    for (auto &p : g_io_pending)
        if (!p.ov) {
            p = {ov, cb};
            ReleaseSRWLockExclusive(&g_io_lock);
            port_log("%s(h %p, %u bytes, offset %08X, ov %p, cb %p, thread %u)\n", what, h, n, ov->Offset, ov, cb,
                     GetCurrentThreadId());
            return traced_completion;
        }
    ReleaseSRWLockExclusive(&g_io_lock);
    return cb;
}

BOOL __stdcall xapi_ReadFileEx(HANDLE h, void *buf, DWORD n, OVERLAPPED *ov, LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    HANDLE vh = h;
    h = vh_real(h);
    if (port_readex_notify) port_readex_notify(buf, n);   // pkgdiag.cpp: read into a freed block?
    BOOL ok = sync_transfer_with_apc(false, h, buf, n, ov, trace_io("ReadFileEx", h, n, ov, cb));
    dtrace("  readex %p -> %p %u @%08X ok %d err %u", vh, h, n, ov ? ov->Offset : 0, ok, ok ? 0 : GetLastError());
    if (!ok)
        io_fail("ReadFileEx", h, n);
    return ok;
}
BOOL __stdcall xapi_WriteFileEx(HANDLE h, const void *buf, DWORD n, OVERLAPPED *ov, LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
    h = vh_real(h);
    if (is_mirror(h)) {                                 // completed at once, as if written
        ov->Internal = 0;
        ov->InternalHigh = n;
        if (cb) {
            QueuedCompletion *q = (QueuedCompletion *)HeapAlloc(GetProcessHeap(), 0, sizeof *q);
            *q = {cb, 0, n, ov};
            QueueUserAPC(run_completion, GetCurrentThread(), (ULONG_PTR)q);
        }
        return TRUE;
    }
    BOOL ok = sync_transfer_with_apc(true, h, (void *)buf, n, ov, trace_io("WriteFileEx", h, n, ov, cb));
    if (!ok)
        io_fail("WriteFileEx", h, n);
    return ok;
}
BOOL __stdcall xapi_SetEndOfFile(HANDLE h) { return is_mirror(vh_real(h)) ? TRUE : SetEndOfFile(vh_real(h)); }
DWORD __stdcall xapi_SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method) { return SetFilePointer(vh_real(h), lo, hi, method); }
BOOL __stdcall xapi_SetFilePointerEx(HANDLE h, LONG lo, LONG hi, LARGE_INTEGER *newpos, DWORD method)
{
    LARGE_INTEGER d;
    d.LowPart = (DWORD)lo;
    d.HighPart = hi;
    return SetFilePointerEx(vh_real(h), d, newpos, method);
}
BOOL __stdcall xapi_GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *w) { return GetFileTime(vh_real(h), c, a, w); }
BOOL __stdcall xapi_FlushFileBuffers(HANDLE h) { return FlushFileBuffers(vh_real(h)); }
DWORD __stdcall xapi_GetFileSize(HANDLE h, DWORD *hi) { return GetFileSize(vh_real(h), hi); }
BOOL __stdcall xapi_GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { return GetFileSizeEx(vh_real(h), size); }

DWORD __stdcall xapi_GetFileAttributesA(const char *name)
{
    char path[MAX_PATH];
    if (!map_path(name, path))
        return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesA(path);
}
BOOL __stdcall xapi_SetFileAttributesA(const char *name, DWORD attrs)
{
    char path[MAX_PATH];
    return map_path(name, path) && SetFileAttributesA(path, attrs);
}
BOOL __stdcall xapi_DeleteFileA(const char *name)
{
    char path[MAX_PATH];
    return map_path(name, path) && DeleteFileA(path);
}
BOOL __stdcall xapi_RemoveDirectoryA(const char *name)
{
    char path[MAX_PATH];
    return map_path(name, path) && RemoveDirectoryA(path);
}
BOOL __stdcall xapi_CreateDirectoryA(const char *name, SECURITY_ATTRIBUTES *sa)
{
    char path[MAX_PATH];
    return map_path(name, path) && CreateDirectoryA(path, sa);
}
// XAPI save-game internals (XCreateSaveGame 0x285645): create the save folder (FILE_CREATE: fails if it exists)
BOOL __stdcall xapi_XapiCreateSaveDirectory(const char *name, SECURITY_ATTRIBUTES *sa)
{
    return xapi_CreateDirectoryA(name, sa);
}
// ... and stamp its last-write time with "now" (save listings sort/show by it)
void __stdcall xapi_XapiTouchSaveDirectory(const char *name)
{
    char path[MAX_PATH];
    if (!map_path(name, path))
        return;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

// XCreateSaveGame (XAPI 0x285645): same branches as the original; its folder name hash (0x28520B) and SaveMeta.xbx
// check (0x285569) only use replaced file calls, so they are called in place.
typedef void(__stdcall *SaveDirNameFn)(const wchar_t *name, char *out, int avail);
typedef DWORD(__stdcall *SaveMetaCheckFn)(const char *meta_path, const wchar_t *name);
static const SaveDirNameFn xbe_save_dir_name = (SaveDirNameFn)0x28520B;
static const SaveMetaCheckFn xbe_save_meta_check = (SaveMetaCheckFn)0x285569;

static DWORD write_save_meta(const char *dir, const wchar_t *name, DWORD flags)
{
    char meta[MAX_PATH], path[MAX_PATH];
    _snprintf(meta, MAX_PATH, "%s\\SaveMeta.xbx", dir);
    if (!map_path(meta, path))
        return ERROR_PATH_NOT_FOUND;
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    wchar_t text[0x98];                                  // UTF-16: BOM, "Name=<name>\r\n" [+ "NoCopy=1\r\n"]
    _snwprintf(text, 0x98, (flags & 1) ? L"%lc%ls%lc%ls%ls%ls" : L"%lc%ls%lc%ls%ls", 0xFEFF, L"Name", L'=', name,
               L"\r\n", L"NoCopy=1\r\n");
    text[0x97] = 0;
    DWORD bytes = (DWORD)wcslen(text) * 2, done = 0;
    BOOL ok = WriteFile(h, text, bytes, &done, nullptr) && SetEndOfFile(h);
    DWORD err = ok && done == bytes ? 0 : GetLastError();
    CloseHandle(h);
    return err;
}

DWORD __stdcall xapi_XCreateSaveGame(const char *root, const wchar_t *name, DWORD disposition, DWORD flags,
                                     char *path_out, UINT path_size)
{
    char dir[0x128];
    lstrcpynA(dir, root, sizeof dir);
    size_t root_len = strlen(dir);
    xbe_save_dir_name(name, dir + root_len, (int)(0xF7 - root_len));
    size_t n = strlen(dir);
    char meta[0x128];
    _snprintf(meta, sizeof meta, "%s\\SaveMeta.xbx", dir);
    meta[sizeof meta - 1] = 0;
    if (disposition == OPEN_EXISTING) {
        DWORD err = xbe_save_meta_check(meta, name);
        if (err)
            return err;
    } else if (xapi_XapiCreateSaveDirectory(dir, nullptr)) {
        DWORD err = write_save_meta(dir, name, flags);
        if (err) {
            xapi_RemoveDirectoryA(dir);
            return err;
        }
    } else {
        DWORD last = GetLastError();
        if (last != ERROR_ALREADY_EXISTS)
            return last;
        if (xbe_save_meta_check(meta, name))
            return ERROR_CANNOT_MAKE;                    // a folder with that hash belongs to another save
        xapi_XapiTouchSaveDirectory(dir);
        if (disposition == CREATE_NEW)
            return last;
    }
    if (path_out) {
        lstrcpynA(path_out, dir, path_size);
        if (n < path_size - 1u) {
            path_out[n] = 0x5C;
            path_out[n + 1] = 0;
        }
    }
    port_log("save: create '%ls' (disposition %u) -> %s ok\n", name, disposition, dir);
    return ERROR_SUCCESS;
}

// XAPI internal 0x171A50 (XDeleteSaveGame): delete everything inside a directory, recursively. Returns NTSTATUS.
static bool is_dot_entry(const WIN32_FIND_DATAA *fd);
static bool delete_tree_contents(const char *dir)
{
    char pattern[MAX_PATH];
    _snprintf(pattern, MAX_PATH, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (is_dot_entry(&fd))
            continue;
        char p[MAX_PATH];
        _snprintf(p, MAX_PATH, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= delete_tree_contents(p) && RemoveDirectoryA(p);
        else
            ok &= SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL) && DeleteFileA(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
// (XDeleteSaveGame calls it with "\??\U:\<save>\" and nothing after it: the directory itself goes too)
LONG __stdcall xapi_XapiDeleteDirectoryContents(const char *name, HANDLE root)
{
    char path[MAX_PATH];
    if (!map_path(name, path))
        return (LONG)0xC000003A;                        // STATUS_OBJECT_PATH_NOT_FOUND
    size_t n = strlen(path);
    while (n > 3 && path[n - 1] == 0x5C)                // trailing backslash
        path[--n] = 0;
    bool ok = delete_tree_contents(path) && RemoveDirectoryA(path);
    port_log("save: delete %s -> %s\n", path, ok ? "ok" : "FAILED");
    return ok ? 0 : (LONG)0xC0000022;                   // STATUS_ACCESS_DENIED
}

BOOL __stdcall xapi_MoveFileA(const char *from, const char *to)
{
    char a[MAX_PATH], b[MAX_PATH];
    return map_path(from, a) && map_path(to, b) && MoveFileA(a, b);
}

static bool is_dot_entry(const WIN32_FIND_DATAA *fd)
{
    const char *n = fd->cFileName;
    return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]));
}

HANDLE __stdcall xapi_FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd)
{
    char path[MAX_PATH];
    if (!map_path(pattern, path)) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = FindFirstFileA(path, fd);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    while (is_dot_entry(fd))                            // FATX listings have no "." / ".." entries
        if (!FindNextFileA(h, fd)) {
            FindClose(h);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
    EnterCriticalSection(&g_find_lock);
    g_find_handles.insert(h);
    LeaveCriticalSection(&g_find_lock);
    return h;
}
BOOL __stdcall xapi_FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    BOOL ok;
    while ((ok = FindNextFileA(h, fd)) && is_dot_entry(fd))
        ;
    return ok;
}

// Volume queries on a drive root ("d:\", "u:\" ...): answered for the mapped folder's volume.
static bool volume_root(const char *in, char *root)
{
    char path[MAX_PATH];
    if (!map_path(in ? in : "d:\\", path))
        return false;
    return GetVolumePathNameA(path, root, MAX_PATH) != 0;
}
BOOL __stdcall xapi_GetDiskFreeSpaceExA(const char *dir, ULARGE_INTEGER *avail, ULARGE_INTEGER *total,
                                        ULARGE_INTEGER *free)
{
    char root[MAX_PATH];
    return volume_root(dir, root) && GetDiskFreeSpaceExA(root, avail, total, free);
}
BOOL __stdcall xapi_GetVolumeInformationA(const char *dir, char *name, DWORD name_size, DWORD *serial, DWORD *maxcomp,
                                          DWORD *flags, char *fs, DWORD fs_size)
{
    char root[MAX_PATH];
    return volume_root(dir, root) && GetVolumeInformationA(root, name, name_size, serial, maxcomp, flags, fs, fs_size);
}

}

// CloseHandle (xapi_thread.cpp) asks here first: Xbox closes find handles with CloseHandle too.
bool port_close_find_handle(HANDLE h)
{
    EnterCriticalSection(&g_find_lock);
    bool found = g_find_handles.erase(h) != 0;
    LeaveCriticalSection(&g_find_lock);
    if (found)
        FindClose(h);
    return found;
}
