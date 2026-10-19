// Startup, threads and crash diagnostics for the converted exe.
#include "port.h"
#include "memstats.h"
#include "container.h"
#include <tlhelp32.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <initializer_list>
#include <intrin.h>

static FILE *g_log;
const PortInfo *g_port_info;

// port.log is this session; the previous one moves to logs\port_<end time>.log (newest 20 kept).
static FILE *open_log()
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char *slash = strrchr(dir, 0x5C);
    if (slash)
        *slash = 0;
    // a second instance on this PC (-gnpad=N, N > 0) logs to port_padN.log
    char base[32] = "port";
    if (const char *pad = strstr(GetCommandLineA(), "-gnpad="))
        if (atoi(pad + 7) > 0)
            _snprintf(base, sizeof base, "port_pad%d", atoi(pad + 7));
    _snprintf(path, MAX_PATH, "%s\\%s.log", dir, base);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
        char logs[MAX_PATH], old[MAX_PATH];
        _snprintf(logs, MAX_PATH, "%s\\logs", dir);
        CreateDirectoryA(logs, nullptr);
        FILETIME local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
        _snprintf(old, MAX_PATH, "%s\\%s_%04u-%02u-%02u_%02u-%02u-%02u.log", logs, base, st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
        MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
        // keep the newest 20 (names sort by time)
        char pattern[MAX_PATH], names[256][64];
        int n = 0;
        _snprintf(pattern, MAX_PATH, "%s\\port_*.log", logs);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (n < 256)
                    lstrcpynA(names[n++], fd.cFileName, 64);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(names, n, 64, [](const void *x, const void *y) { return strcmp((const char *)x, (const char *)y); });
        for (int i = 0; i + 20 < n; i++) {
            _snprintf(old, MAX_PATH, "%s\\%s", logs, names[i]);
            DeleteFileA(old);
        }
    }
    return fopen(path, "w");
}

void (*port_log_hook)(const char *line);    // gn_core.cpp: forwards diagnostics to the netplay host

void port_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    fputs(buf, stdout);
    fflush(stdout);
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (!g_log)
        g_log = open_log();
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
    ReleaseSRWLockExclusive(&lock);
    if (port_log_hook)
        port_log_hook(buf);
}

// Reached through a krnl_* trap: [esp] = slot index, [esp+4] = return address into the caller.
extern "C" void __stdcall port_kernel_trap_report(int slot, uint32_t ret)
{
    port_log("KERNEL TRAP: %s called from 0x%08X - no Windows replacement for this path yet\n",
             krnl_name(slot), ret);
    ExitProcess(0xDEAD0000 | (slot & 0xFFFF));
}

extern "C" __declspec(naked) void port_kernel_trap()
{
    __asm {
        push dword ptr [esp + 4]     // caller return address
        push dword ptr [esp + 4]     // slot index
        call port_kernel_trap_report
    }
}

typedef void (__cdecl *InitFn)();
typedef int (__cdecl *MainFn)(int, int, int);

static void run_init_table(uint32_t begin, uint32_t end, const char *what)
{
    int n = 0;
    for (uint32_t p = begin; p < end; p += 4) {
        InitFn f = *(InitFn *)p;
        if (f && (uint32_t)f != 0xFFFFFFFF) {
            f();
            n++;
        }
    }
    port_log("  %s: %d initializers run\n", what, n);
}

// Developer menus ([game] DevMenus=1): the dev build's splash and battle debug menus (.mnu) still ship in gladius.bec.
// Splash: splash_startup (400), splash_school (0x191 / 0x192), splash_consolevars (0x193).
// Battle: kero_shortoption (0x6B), kero_optionoption (0x6C), kero_debugcamtrack (0x6D).
typedef int(__cdecl *MenuListLoadMenuFn)(void *list, const char *file, int id);
static const MenuListLoadMenuFn xbe_menu_load = (MenuListLoadMenuFn)0x5CD80;

static int __cdecl port_menu_load(void *list, const char *file, int id)
{
    if (!_stricmp(file, "splash_pressa.mnu")) {
        int r = xbe_menu_load(list, "splash_startup.mnu", 400);
        xbe_menu_load(list, "splash_school.mnu", 0x191);
        xbe_menu_load(list, "splash_school.mnu", 0x192);
        xbe_menu_load(list, "splash_consolevars.mnu", 0x193);
        return r;
    }
    return xbe_menu_load(list, file, id);
}

// The developer SplashState's methods survive in retail (Init 0x161E50, Handle 0x1612E0, Draw 0x161C50) but nothing
// builds it. With DevMenus the state factory's case 4 builds one: 0x84 bytes on the GameState base, with
// EmptyState's vtable (0x33BF88) and slots 4 / 7 / 8 replaced.
static DWORD g_splash_vtbl[11];

static void *__fastcall splash_construct(BYTE *mem)
{
    typedef void(__fastcall * CtorFn)(void *);
    ((CtorFn)0x116E50)(mem);                            // GameState::GameState (thiscall, no args)
    memset(mem + 0x14, 0, 0x84 - 0x14);
    *(DWORD **)mem = g_splash_vtbl;
    ((CtorFn)0x5BB30)(mem + 0x48);                      // MenuControl::MenuControl
    return mem;
}

static __declspec(naked) void splash_factory_case()  // edi = state manager, as at every factory case
{
    __asm {
        push 0x84
        mov eax, 0x117020                               // state allocator (cdecl size)
        call eax
        add esp, 4
        test eax, eax
        jz failed
        mov ecx, eax
        call splash_construct
        mov [edi + 0x14], eax
        mov dword ptr [eax + 8], 4
        mov eax, 0x20DCD                                // common tail (publishes [edi+0x14])
        jmp eax
    failed:
        mov eax, 0x20D8C                                // factory default case
        jmp eax
    }
}

// World map debug menu (world_options.mnu): retail never opens it. Player 1's Y opens it, through the world map's
// WorldMenu::IsOpen call (0xF99E8).
bool port_player1_y_pressed();

static bool __fastcall port_worldmenu_isopen(BYTE *menu)
{
    typedef bool(__fastcall * IsOpenFn)(void *);
    typedef void(__fastcall * OpenFn)(void *);
    if (!((IsOpenFn)0x4D8D0)(menu) && port_player1_y_pressed())
        ((OpenFn)0x4D9D0)(menu);                        // requests menu 200, map paused until it closes
    return ((IsOpenFn)0x4D8D0)(menu);
}

// RequestState (0x1EF40): with DevMenus the first MAINMENU (21) request becomes SPLASH (4), where the dev menu lives.
static bool g_dev_splash_pending;
static void __cdecl port_request_state(int id)
{
    if (g_dev_splash_pending && id == 21) {
        g_dev_splash_pending = false;
        id = port_is_editor() ? 7 : 4;
        port_log(port_is_editor() ? "editor: boot -> world map\n" : "game: boot -> developer splash menu\n");
    }
    *(int *)0x38E43C = id;
}

// [debug] WatchTls=1: hardware write watch on the game thread's CRT per-thread-data slot
static volatile DWORD g_tls_watch_req;
static DWORD g_tls_index0, g_tls_block0;

// The game's static TLS block (CRT per-thread data, 0x90 bytes) must really be that big. Exes converted with an empty
// TLS template got a 0-byte block, so for those every game thread gets a full-size block of ours; the loader's own
// pointer is put back before the thread exits.
static const DWORD TLS_NEED = 0x90, TLS_MAGIC = 0x534C5447;    // 'GTLS'
static int tls_template_ok()                                     // 1 = exe template big enough
{
    static int ok = -1;
    if (ok < 0) {
        const BYTE *exe = (const BYTE *)GetModuleHandleA(nullptr);
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        const IMAGE_TLS_DIRECTORY32 *td = dd.VirtualAddress ? (const IMAGE_TLS_DIRECTORY32 *)(exe + dd.VirtualAddress) : nullptr;
        ok = !td || td->EndAddressOfRawData - td->StartAddressOfRawData >= TLS_NEED;
        port_log("tls: exe TLS template %u bytes%s\n", td ? td->EndAddressOfRawData - td->StartAddressOfRawData : 0,
                 ok ? "" : " - too small (old converter): game TLS blocks are replaced per thread");
    }
    return ok;
}

void port_fix_game_tls()
{
    if (tls_template_ok())
        return;
    void **tls = (void **)__readfsdword(0x2C);
    if (!tls) return;
    DWORD idx = *(DWORD *)0x418B8C;
    DWORD *nb = (DWORD *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TLS_NEED + 8);
    if (!nb) return;
    nb[TLS_NEED / 4] = TLS_MAGIC;
    nb[TLS_NEED / 4 + 1] = (DWORD)tls[idx];               // the loader's block, restored at thread exit
    tls[idx] = nb;
}

void port_restore_game_tls()
{
    void **tls = (void **)__readfsdword(0x2C);
    if (!tls) return;
    DWORD idx = *(DWORD *)0x418B8C;
    DWORD *b = (DWORD *)tls[idx];
    if (!b || tls_template_ok() || b[TLS_NEED / 4] != TLS_MAGIC) return;
    tls[idx] = (void *)b[TLS_NEED / 4 + 1];
    HeapFree(GetProcessHeap(), 0, b);
}

// Level editor: the same exe installed as gladius_editor.exe; boots straight into the world map.
bool port_is_editor()
{
    static int editor = -1;
    if (editor < 0) {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char *base = strrchr(exe, 0x5C) ? strrchr(exe, 0x5C) + 1 : exe;
        editor = _strnicmp(base, "gladius_editor", 14) == 0;
    }
    return editor != 0;
}

// Settings file next to the exe; a second instance (-gnpad=N) reads gladiusport_padN.ini when it exists.
const char *port_ini_name()
{
    static char name[32];
    if (!name[0]) {
        const char *pad = strstr(GetCommandLineA(), "-gnpad=");
        int n = pad ? atoi(pad + 7) : 0;
        char alt[32], path[MAX_PATH];
        _snprintf(alt, sizeof alt, "gladiusport_pad%d.ini", n);
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        strcpy(strrchr(path, 0x5C) + 1, alt);
        strcpy(name, n > 0 && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? alt : "gladiusport.ini");
    }
    return name;
}

static DWORD WINAPI game_thread(LPVOID param)
{
    port_fix_game_tls();                                // before any game code runs on this thread
    memstats_set_role(MS_MAIN);
    port_set_vtid(0x1000);                              // virtual thread id of the main game thread
    const PortInfo *info = (const PortInfo *)param;
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    if (!PORT_RELEASE && GetPrivateProfileIntA("debug", "WatchTls", 0, ini)) {
        DWORD idx = *(DWORD *)0x418B8C, *tls = (DWORD *)__readfsdword(0x2C);
        g_tls_watch_req = tls[idx] + 0xC;
        g_tls_index0 = idx, g_tls_block0 = tls[idx];
        port_log("debug: game thread TLS block %08X, CRT ptd slot %08X\n", tls[idx], g_tls_watch_req);
        Sleep(200);                                     // let the watchdog arm it before the CRT runs
    }
    run_init_table(info->rt_init_begin, info->rt_init_end, "runtime init (_rtinit: _mtinit, _ioinit)");
    InitFn fpinit = *(InitFn *)info->fpinit_ptr;
    if (fpinit) {
        port_log("  _FPinit 0x%08X\n", (uint32_t)fpinit);
        fpinit();
    }
    run_init_table(info->c_init_begin, info->c_init_end, "C init (__xi)");
    run_init_table(info->cxx_init_begin, info->cxx_init_end, "C++ constructors (__xc)");
    port_log("calling game main 0x%08X\n", info->game_main);
    int r = ((MainFn)info->game_main)(0, 0, 0);
    port_log("game main returned %d\n", r);
    port_restore_game_tls();
    return r;
}

// Fatal exceptions: log fault, registers and the image return addresses on the stack.
static DWORD g_watch_addr;                             // [debug] WatchWrite: hardware write breakpoint

// The game was written for one CPU core and its shared engine structures have no locks, so all game threads run on
// one dedicated core; the port's own threads use the rest. [cpu] SingleCore=0 disables it.
static DWORD_PTR g_game_core_mask;

static DWORD_PTR g_pin_mask_saved;                     // the single game core (also while unpinned)
static bool g_pin_init;

void port_pin_game_thread(HANDLE thread)
{
    bool &init = g_pin_init;
    if (!init) {
        init = true;
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        if (proc & (proc - 1)) {                        // >= 2 usable cores: work out the game core
            DWORD_PTR rest = proc & (proc - 1);         // prefer the second usable core (core 0 takes interrupts)
            // two instances on one PC (-gnpad=N): instance N takes the N-th core after that
            const char *pad = strstr(GetCommandLineA(), "-gnpad=");
            for (int skip = pad ? atoi(pad + 7) : 0; skip > 0 && (rest & (rest - 1)); skip--)
                rest &= rest - 1;
            g_game_core_mask = rest & ~(rest - 1);
            int core = 0;
            for (DWORD_PTR m = g_game_core_mask; m > 1; m >>= 1)
                core++;
            g_pin_mask_saved = g_game_core_mask;
            if (GetPrivateProfileIntA("cpu", "SingleCore", 1, ini)) {
                port_log("cpu: game threads pinned to core %d (Xbox single-core scheduling)\n", core);
            } else {
                g_game_core_mask = 0;
                port_log("cpu: game threads unpinned (SingleCore=0, experimental)\n");
            }
        } else {
            port_log("cpu: game threads unpinned (one usable core)\n");
        }
    }
    if (g_game_core_mask)
        SetThreadAffinityMask(thread, g_game_core_mask);
}

// Options > Video "CPU Cores": Single = game threads on the game core, Multi = all cores (experimental).
void port_set_single_core(bool on)
{
    if (!g_pin_init || !g_pin_mask_saved)
        return;
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    g_game_core_mask = on ? g_pin_mask_saved : 0;
    DWORD_PTR mask = on ? g_pin_mask_saved : proc;
    extern HANDLE port_main_game_thread();
    if (HANDLE m = port_main_game_thread())
        SetThreadAffinityMask(m, mask);
    for (int i = 0; i < g_game_thread_count && i < 32; i++)
        SetThreadAffinityMask(g_game_threads[i].handle, mask);
    port_log("cpu: game threads %s\n", on ? "pinned to the game core again" : "unpinned (all cores, experimental)");
}

// [debug] Profile: CPU time of every game thread, logged every 10 s as "threads:".
static HANDLE g_main_game_thread;
static const void *g_main_game_start;
HANDLE port_main_game_thread() { return g_main_game_thread; }

void port_register_game_thread(HANDLE thread, const void *start)
{
    DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &g_main_game_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    g_main_game_start = start;
}

static ULONGLONG thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!h || !GetThreadTimes(h, &c, &e, &k, &u))
        return 0;
    return ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

void port_thread_stats()
{
    extern bool g_prof_on;
    if (!g_prof_on)
        return;
    static ULONGLONG last_tick, last_main, last[32];
    ULONGLONG now = GetTickCount64();
    if (last_tick && now - last_tick < 10000)
        return;
    double wall = (now - last_tick) * 10000.0;          // 100 ns units
    bool first = !last_tick;
    last_tick = now;
    char line[1024];
    int n = 0;
    ULONGLONG t = thread_cpu(g_main_game_thread);
    double total = 100.0 * (t - last_main) / wall, pct;
    n += _snprintf(line + n, sizeof line - n, " main@%06X %.1f%%", (unsigned)(uintptr_t)g_main_game_start, total);
    last_main = t;
    for (int i = 0; i < g_game_thread_count && i < 32; i++) {
        t = thread_cpu(g_game_threads[i].handle);
        pct = 100.0 * (t - last[i]) / wall;
        last[i] = t;
        total += pct;
        if (pct >= 0.5 && n < (int)sizeof line - 40)
            n += _snprintf(line + n, sizeof line - n, " %06X %.1f%%", g_game_threads[i].start, pct);
    }
    if (!first)
        port_log("threads: %d game threads, %.1f%% of one core in total (10 s):%s\n", g_game_thread_count + 1, total,
                 line);
}

// Memory watches (pkgdiag.cpp): two more 4-byte write breakpoints (DR2 / DR3) on every thread of the process.
static volatile uint32_t g_mw_addr[2];
static volatile LONG g_mw_gen, g_mw_applied;

void port_memwatch_set(int slot, uint32_t addr)
{
    if (slot < 0 || slot > 1 || g_mw_addr[slot] == addr) return;
    g_mw_addr[slot] = addr;
    InterlockedIncrement(&g_mw_gen);
}

uint32_t port_memwatch_get(int slot) { return slot >= 0 && slot < 2 ? g_mw_addr[slot] : 0; }

static void fill_debug_regs(CONTEXT &c)
{
    c.Dr7 = 0;
    if (g_watch_addr) {
        c.Dr0 = g_watch_addr;
        c.Dr1 = 0x418B8C;                               // the game's TLS index variable (CRT _getptd)
        c.Dr7 |= 1 | (1u << 16) | (3u << 18) | 4 | (1u << 20) | (3u << 22);   // L0+L1: 4-byte writes
    }
    for (int i = 0; i < 2; i++) {
        uint32_t a = g_mw_addr[i];
        (&c.Dr2)[i] = a;
        if (a) c.Dr7 |= (1u << (4 + 2 * i)) | (1u << (24 + 4 * i)) | (3u << (26 + 4 * i));   // L2/L3, write, 4 bytes
    }
}

void port_apply_watch(HANDLE thread)
{
    if (!g_watch_addr && !g_mw_addr[0] && !g_mw_addr[1])
        return;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    fill_debug_regs(c);
    SetThreadContext(thread, &c);
}

// Watchdog: push changed memory watches into every thread.
static void memwatch_apply_all()
{
    LONG gen = g_mw_gen;
    if (gen == g_mw_applied) return;
    g_mw_applied = gen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof te};
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            fill_debug_regs(c);
            SetThreadContext(h, &c);
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

// "module+offset" for an address in a loaded DLL (empty for the game image / unmapped memory).
static const char *module_of(uint32_t a, char *out, size_t n)
{
    out[0] = 0;
    HMODULE m = nullptr;
    if (a < 0x12000 || a >= 0x5E1000) {
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)a, &m) && m) {
            char path[MAX_PATH];
            GetModuleFileNameA(m, path, MAX_PATH);
            const char *base = strrchr(path, 0x5C);
            _snprintf(out, n - 1, " [%s+0x%X]", base ? base + 1 : path, a - (uint32_t)m);
            out[n - 1] = 0;
        }
    }
    return out;
}

// Full-memory minidump: <exe dir>\crash\<time>_<why>.dmp (newest 4 kept), once per reason per session, written by a
// helper thread with a bounded wait.
volatile DWORD g_port_probe_tid;                       // pkgdiag.cpp: thread inside a guarded probe read

struct DumpJob {
    char path[MAX_PATH];
    EXCEPTION_POINTERS *ep;
    DWORD tid;
};

static DWORD WINAPI dump_writer(LPVOID p)
{
    DumpJob *job = (DumpJob *)p;
    typedef BOOL(WINAPI * WriteFn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    WriteFn write = dbg ? (WriteFn)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!write)
        return 1;
    HANDLE f = CreateFileA(job->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 2;
    struct {
        DWORD ThreadId;
        EXCEPTION_POINTERS *ExceptionPointers;
        BOOL ClientPointers;
    } mei = {job->tid, job->ep, FALSE};
    // FullMemory 0x2 | HandleData 0x4 | UnloadedModules 0x20 | FullMemoryInfo 0x800 | ThreadInfo 0x1000
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x2 | 0x4 | 0x20 | 0x800 | 0x1000,
                    job->ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
    return ok ? 0 : 3;
}

static void write_dump(const char *why, EXCEPTION_POINTERS *ep)
{
    static char done[8][24];
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    for (auto &d : done)
        if (!strcmp(d, why) || !d[0]) {
            bool first = !d[0];
            if (first) lstrcpynA(d, why, sizeof d);
            if (!first) {
                ReleaseSRWLockExclusive(&lock);
                return;
            }
            break;
        }
    static DumpJob job;
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    *strrchr(dir, 0x5C) = 0;
    lstrcatA(dir, "\\crash");
    CreateDirectoryA(dir, nullptr);
    {                                                   // keep the newest 3 (+ the one written now)
        char pattern[MAX_PATH], names[64][64], old[MAX_PATH];
        int n = 0;
        _snprintf(pattern, MAX_PATH, "%s\\*.dmp", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (n < 64) lstrcpynA(names[n++], fd.cFileName, 64);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(names, n, 64, [](const void *x, const void *y) { return strcmp((const char *)x, (const char *)y); });
        for (int i = 0; i + 3 < n; i++) {
            _snprintf(old, MAX_PATH, "%s\\%s", dir, names[i]);
            DeleteFileA(old);
        }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(job.path, MAX_PATH, "%s\\%04u-%02u-%02u_%02u-%02u-%02u_%s.dmp", dir, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, why);
    job.ep = ep;
    job.tid = GetCurrentThreadId();
    port_log("dump: writing full-memory dump %s ...\n", job.path);
    DWORD t0 = GetTickCount(), code = 99;
    HANDLE t = CreateThread(nullptr, 0, dump_writer, &job, 0, nullptr);
    if (t) {
        if (WaitForSingleObject(t, 180000) == WAIT_OBJECT_0)
            GetExitCodeThread(t, &code);
        CloseHandle(t);
    }
    port_log("dump: %s (%u ms)\n", code == 0 ? "done" : code == 99 ? "timed out" : "FAILED", GetTickCount() - t0);
    ReleaseSRWLockExclusive(&lock);
}

void port_write_dump(const char *why) { write_dump(why, nullptr); }

// A fatal exception nobody handled: full dump, then default handling (WER).
static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    write_dump("crash", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && g_port_probe_tid == GetCurrentThreadId())
        return EXCEPTION_CONTINUE_SEARCH;               // guarded probe read (pkgdiag), handled by its __except
    if (code == EXCEPTION_SINGLE_STEP && g_watch_addr && (ep->ContextRecord->Dr6 & 1)) {
        static int hits;
        if (hits++ < 200) {
            uint32_t *sp = (uint32_t *)ep->ContextRecord->Esp;
            port_log("WATCH write %08X = %08X at EIP %08X (thread %u) ret %08X %08X %08X\n", g_watch_addr,
                     *(DWORD *)g_watch_addr, ep->ContextRecord->Eip, GetCurrentThreadId(), sp[0], sp[1], sp[2]);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_watch_addr && (ep->ContextRecord->Dr6 & 2)) {
        static int hits1;
        if (hits1++ < 50) {
            uint32_t *sp = (uint32_t *)ep->ContextRecord->Esp;
            port_log("WATCH write TLS index 418B8C = %08X at EIP %08X (thread %u) ret %08X %08X %08X\n",
                     *(DWORD *)0x418B8C, ep->ContextRecord->Eip, GetCurrentThreadId(), sp[0], sp[1], sp[2]);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xC)) {
        static int mwhits;
        int slot = ep->ContextRecord->Dr6 & 4 ? 0 : 1;
        uint32_t a = g_mw_addr[slot];
        if (a && mwhits++ < 100) {
            uint32_t *sp = (uint32_t *)ep->ContextRecord->Esp;
            char st[200]; int o = 0, found = 0;
            const uint32_t *top = (const uint32_t *)__readfsdword(4);
            for (int k = 0; k < 400 && sp + k < top && found < 12 && o < 180; k++) {
                uint32_t v = sp[k];
                if (v < 0x12000 || v > 0x2F0000) continue;
                o += _snprintf(st + o, sizeof st - o, " %06X", v); found++;
            }
            st[o < (int)sizeof st ? o : (int)sizeof st - 1] = 0;
            char mod[MAX_PATH + 32];
            port_log("memwatch: WRITE %08X = %08X at EIP %08X%s (thread %u) stack%s\n", a, *(DWORD *)a,
                     ep->ContextRecord->Eip, module_of(ep->ContextRecord->Eip, mod, sizeof mod), GetCurrentThreadId(), st);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == 0xE06D7363) {                           // C++ throw: name it and where (an uncaught one = abort,
        static volatile LONG cxx;                       // which skips every handler and leaves no dump)
        if (InterlockedIncrement(&cxx) <= 20) {
            const char *type = "?";
            ULONG_PTR *info = ep->ExceptionRecord->ExceptionInformation;
            if (ep->ExceptionRecord->NumberParameters >= 3 && info[2]) {
                // ThrowInfo +0xC -> CatchableTypeArray {count, CatchableType *[]} -> +4 TypeDescriptor, name at +8
                DWORD cta = *(DWORD *)(info[2] + 0xC);
                if (cta && *(DWORD *)cta) {
                    DWORD ct = *(DWORD *)(cta + 4), td = ct ? *(DWORD *)(ct + 4) : 0;
                    if (td) type = (const char *)(td + 8);
                }
            }
            char mod[MAX_PATH + 32];
            port_log("C++ exception %s (thread %u), return addresses:", type, GetCurrentThreadId());
            uint32_t *sp = (uint32_t *)ep->ContextRecord->Esp;
            uint32_t *top = (uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
            int shown = 0;
            for (int i = 0; i < 512 && shown < 14 && sp + i < top; i++) {
                uint32_t v = sp[i];
                if (v >= 0x10000 && *module_of(v, mod, sizeof mod)) {
                    port_log(" %08X%s", v, mod);
                    shown++;
                }
            }
            port_log("\n");
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code < 0xC0000000)                              // informational: let SEH handle it
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    char mod[MAX_PATH + 32];
    {                                                   // CRT per-thread data path: index var -> TLS array -> block
        DWORD idx = *(DWORD *)0x418B8C, *tls = (DWORD *)__readfsdword(0x2C);
        port_log("TLS: index %u, array %p [%08X %08X %08X %08X], slot %08X (startup: index %u block %08X)\n", idx,
                 tls, tls ? tls[0] : 0, tls ? tls[1] : 0, tls ? tls[2] : 0, tls ? tls[3] : 0,
                 tls && idx < 64 ? *(DWORD *)(tls[idx] + 0xC) : 0, g_tls_index0, g_tls_block0);
    }
    port_log("EXCEPTION %08X at 0x%08X%s (thread %u)", code, c->Eip, module_of(c->Eip, mod, sizeof mod),
             GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION)
        port_log(" (%s 0x%08X)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
    port_log("\n  EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X EBP %08X ESP %08X\n",
             c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    if (code == EXCEPTION_ACCESS_VIOLATION)             // [debug] PageHeap: name the game block that was touched
        pageheap_describe((uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
    port_log("  stack (image return addresses):");
    uint32_t *sp = (uint32_t *)c->Esp;
    uint32_t *top = (uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    int shown = 0;
    for (int i = 0; i < 1024 && shown < 16 && sp + i < top; i++) {
        uint32_t v = sp[i];
        if (v >= 0x12000 && v < 0x5E1000) {
            port_log(" %08X", v);
            shown++;
        }
    }
    port_log("\n  stack (module return addresses):");
    shown = 0;
    for (int i = 0; i < 256 && shown < 12 && sp + i < top; i++) {
        uint32_t v = sp[i];
        if (v >= 0x10000 && *module_of(v, mod, sizeof mod)) {
            port_log(" %08X%s", v, mod);
            shown++;
        }
    }
    port_log("\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() fast-fails without running exception handlers: log the callers and write the dump here.
static void log_callers(const char *what)
{
    char mod[MAX_PATH + 32];
    uint32_t *sp = (uint32_t *)_AddressOfReturnAddress();
    uint32_t *top = (uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    port_log("%s (thread %u)\n  stack (image return addresses):", what, GetCurrentThreadId());
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 24 && sp + i < top; i++)
        if (sp[i] >= 0x12000 && sp[i] < 0x5E1000) {
            port_log(" %08X", sp[i]);
            shown++;
        }
    port_log("\n  stack (module return addresses):");
    shown = 0;
    for (int i = 0; i < 1024 && shown < 16 && sp + i < top; i++)
        if (sp[i] >= 0x10000 && *module_of(sp[i], mod, sizeof mod)) {
            port_log(" %08X%s", sp[i], mod);
            shown++;
        }
    port_log("\n");
}

static void __cdecl on_abort(int)
{
    log_callers("ABORT called");
    write_dump("abort", nullptr);
}

static void __cdecl on_purecall()
{
    log_callers("PURE VIRTUAL call");
    write_dump("purecall", nullptr);
    abort();
}

GameThread g_game_threads[32];
int g_game_thread_count;

// Snapshot another thread: EIP, registers and image return addresses on its stack. Nothing that takes a lock runs
// while it is suspended.
static void dump_thread(HANDLE t, const char *why)
{
    if (SuspendThread(t) == (DWORD)-1) {
        DWORD code = 0;
        GetExitCodeThread(t, &code);
        port_log("[%s] not running (exit code %08X, error %u)\n", why, code, GetLastError());
        return;
    }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(t, &c);
    uint32_t ret[20];
    int shown = 0;
    for (uint32_t a = c.Esp; ok && shown < 20 && a < c.Esp + 0x2000; a += 4) {
        uint32_t v;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)a, &v, 4, &got) || got != 4)
            break;
        if (v >= 0x12000 && v < 0x5E1000)
            ret[shown++] = v;
    }
    ResumeThread(t);
    if (!ok)
        return;
    port_log("[%s tid %u] EIP %08X  EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X EBP %08X ESP %08X\n  stack:",
             why, GetThreadId(t), c.Eip, c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp);
    for (int i = 0; i < shown; i++)
        port_log(" %08X", ret[i]);
    port_log("\n");
}

// [debug] Profile=1: ~1 kHz sampling profiler for the main thread (EIP + first game caller). Frames slower than 25 ms
// feed a "slow" histogram; the top buckets are logged every 10 s ("prof:").
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mmsystem.h>
void port_game_resolution(const char *ini);     // game_resolution.cpp
void port_tex_replace_init(const char *ini);    // tex_replace.cpp
void d3d_install_fx_hooks();                    // d3d_render.cpp
void port_battle_trace_init();                  // battle_trace.cpp
void port_battle_restart_init();                // battle_restart.cpp
void port_save_pages_init(const char *ini);     // save_pages.cpp
void port_reboot_init();                        // reboot.cpp
void port_options_video_init();                 // options_video.cpp
void port_menu_quit_init();                     // menu_quit.cpp
void port_mods_init();                          // mods.cpp
void port_editor_init();                        // editor.cpp
extern "C" volatile LONG g_swap_count;                  // d3d_core.cpp
static uint32_t first_game_return(uint32_t esp)         // same process: read the stack directly, guarded
{
    const uint32_t *sp = (const uint32_t *)esp;
    __try {
        for (int i = 0; i < 256; i++)
            if (sp[i] >= 0x12000 && sp[i] < 0x2F0000) return sp[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}
static DWORD WINAPI profiler_thread(LPVOID param)
{
    HANDLE t = (HANDLE)param;
    timeBeginPeriod(1);
    struct Sample { uint32_t eip, caller; };
    std::vector<Sample> frame;
    std::unordered_map<uint32_t, uint32_t> leaf, caller;
    LARGE_INTEGER f, frame_start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&frame_start);
    LONG last_swap = g_swap_count;
    DWORD last_report = GetTickCount(), slow_frames = 0, frames = 0, slow_samples = 0;
    for (;;) {
        Sleep(1);
        LONG sc = g_swap_count;
        if (sc != last_swap) {                          // frame boundary: classify the samples taken
            QueryPerformanceCounter(&now);
            double ms = (now.QuadPart - frame_start.QuadPart) * 1000.0 / f.QuadPart / (sc - last_swap);
            frame_start = now;
            last_swap = sc;
            frames++;
            if (ms > 25.0) {
                slow_frames++;
                for (auto &s : frame) {
                    leaf[s.eip]++;
                    caller[s.caller]++;
                    slow_samples++;
                }
            }
            frame.clear();
        }
        if (SuspendThread(t) == (DWORD)-1)
            return 0;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(t, &c);
        uint32_t first = ok ? first_game_return(c.Esp) : 0;
        ResumeThread(t);
        if (!ok)
            continue;
        uint32_t e = c.Eip;
        uint32_t key = (e >= 0x12000 && e < 0x5E1000) ? (e & ~0x3Fu) : e;
        if (frame.size() < 4096)
            frame.push_back({key, first});
        if (GetTickCount() - last_report >= 10000 && slow_samples) {
            last_report = GetTickCount();
            auto top = [](std::unordered_map<uint32_t, uint32_t> &m, int n) {
                std::vector<std::pair<uint32_t, uint32_t>> v(m.begin(), m.end());
                std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
                if ((int)v.size() > n) v.resize(n);
                return v;
            };
            port_log("prof: %u of %u frames slower than 25 ms, %u samples in them\n", slow_frames, frames, slow_samples);
            char mod[MAX_PATH + 32], line[1024];
            int o = 0;
            for (auto &kv : top(leaf, 30)) {
                o += _snprintf(line + o, sizeof line - o, " %08X%s=%u", kv.first, module_of(kv.first, mod, sizeof mod),
                               kv.second * 100 / slow_samples);
                if (o > 700) { line[o] = 0; port_log("prof: leaf%s\n", line); o = 0; }
            }
            if (o) { line[o] = 0; port_log("prof: leaf%s\n", line); o = 0; }
            for (auto &kv : top(caller, 30)) {
                o += _snprintf(line + o, sizeof line - o, " %06X=%u", kv.first, kv.second * 100 / slow_samples);
                if (o > 700) { line[o] = 0; port_log("prof: caller%s\n", line); o = 0; }
            }
            if (o) { line[o] = 0; port_log("prof: caller%s\n", line); }
            leaf.clear();
            caller.clear();
            slow_frames = frames = slow_samples = 0;
        }
    }
}

extern "C" __declspec(dllexport) void __stdcall port_start(const PortInfo *info)
{
    AddVectoredExceptionHandler(1, crash_handler);
    signal(SIGABRT, on_abort);                          // abort() ends in a fast-fail no handler sees: log it first
    _set_purecall_handler(on_purecall);
    SetUnhandledExceptionFilter(unhandled_filter);
    g_port_info = info;
    port_init_heap();
    port_init_files();
    {
        char msini[MAX_PATH];
        GetModuleFileNameA(nullptr, msini, MAX_PATH);
        strcpy(strrchr(msini, 0x5C) + 1, port_ini_name());
        memstats_init(msini);                           // before any game code allocates
        container_init(msini);                          // [mem] Container: fixed-address game memory
        port_init_heap();                               // XapiProcessHeap = the container's fixed handle if on
        pageheap_init(msini);
        heapcheck_install();
    }
    port_log("Gladius PC port - gladiusport.dll\n");
    HMODULE exe = GetModuleHandleA(nullptr);
    port_log("exe base 0x%08X, info 0x%08X magic %08X main 0x%08X\n", (uint32_t)exe, (uint32_t)info,
             info->magic, info->game_main);
    if (info->magic != 0x54524F50) {
        port_log("bad port info block\n");
        ExitProcess(1);
    }
    // the main thread on Xbox is created by XAPI mainCRTStartup; same here with a large stack
    char ini[MAX_PATH], val[32];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    GetPrivateProfileStringA("debug", "WatchWrite", "", val, sizeof val, ini);
    g_watch_addr = PORT_RELEASE ? 0 : strtoul(val, nullptr, 16);
    if (g_watch_addr)
        port_log("debug: hardware write watch on %08X\n", g_watch_addr);
    // Fixed-size name/string buffers, all built by C++ ctors (`push size` in each thunk of the __xc table) that run on
    // the game thread, after this. A full buffer hands out NULL and callers don't check: stock items.tok fills ~99% of
    // ItemDBStringBuffer, so modded item lists crashed in the first lookup by name (0x77200). Raise every one to the
    // block allocator's ceiling (0x177F00: block sizes in 15 bits of 4-byte units -> 0x7FFF units). Their memory comes
    // from the CRT heap (0x1768A0 -> malloc), so the cost is ~3 MB of container main.
    {
        static const struct { DWORD push; DWORD size; const char *name; } bufs[] = {
            {0x29AEB0, 0x200, "GlobalVarStringBuffer"},   {0x29AF40, 0x6000, "FaqStrings"},
            {0x29B050, 0x1000, "StoryEventStrings"},      {0x29B0C0, 0x3000, "ActorStringBuffer"},
            {0x29B1B0, 0x71C, "BadgeStringBuffer"},       {0x29B2E0, 0x1000, "CharacterNameBuffer"},
            {0x29B330, 0x2800, "ClassDefStrings"},        {0x29B4A0, 0x800, "TextFloaterStringBuffer"},
            {0x29B6F0, 0x14820, "ItemDBStringBuffer"},    {0x29B720, 0x3000, "JournalStringBuffer"},
            {0x29BD60, 0xD8CC, "TokenStringBuffer"},      {0x29C555, 0x2000, "ConsoleSmallSize"},
            {0x29C790, 0x4000, "AnimNames"},              {0x29C7B5, 0x3000, "anim_CharacterIndex"},
            {0x29C7D5, 0x4000, "anim_RotNames"},          {0x29C7F5, 0x1000, "anim_RawRotNames"},
            {0x29C815, 0x4000, "anim_PosNames"},          {0x29C835, 0x800, "anim_FloatNames"},
            {0x29C855, 0x800, "anim_IntNames"},           {0x29C875, 0x6000, "anim_BoolNames"},
            {0x29C895, 0x200, "anim_EventCommands"},      {0x29C8B5, 0x19000, "anim_XPosNames"},
            {0x29C8D5, 0x1E000, "anim_XRotNames"},        {0x29C980, 0xC000, "fxnames"},
            {0x29CA60, 0x1000, "TintSetStringBuffer"},    {0x29D6E5, 0x1000, "PaxName"},
            {0x29D735, 0x4000, "MeshLoader"},             {0x29D790, 0xBB8, "PaxConfigStrings"},
        };
        const DWORD ceiling = 0x1FFF0;
        int raised = 0;
        for (const auto &b : bufs) {
            BYTE *p = (BYTE *)b.push;
            if (p[0] == 0x68 && *(DWORD *)(p + 1) == b.size) {
                *(DWORD *)(p + 1) = ceiling;
                raised++;
            } else {
                port_log("game: %s - unexpected bytes at %06X, not enlarged\n", b.name, b.push);
            }
        }
        port_log("game: %d of %d string buffers raised to %u bytes (ItemDB was 84000)\n", raised,
                 (int)(sizeof bufs / sizeof bufs[0]), ceiling);
    }
    // Fixed object pools: static arrays in the XBE's data, threaded into a freelist by one init function each (run from
    // a C++ ctor). Nothing else addresses the arrays (their base getters have no callers) and the capacity the stats
    // ctor (0x1776B0) records is display only, so a pool moves by rewriting its init's six constants: head = base,
    // link = base + elem, loop bound = (n-1) * elem, terminator at base + (n-1) * elem, stats bytes = n * elem.
    // The new arrays come from container main (synced, same address on every PC). School (5) and LeagueOfficeData (1)
    // use a different init and stay.
    {
        static const struct { DWORD init, len, base, elem, count; const char *name; } pools[] = {
            {0x01B070, 0x52, 0x38C7B0, 0x2C, 150, "WayPoint"},      {0x0247D0, 0x52, 0x38EC88, 0x10, 1200, "GlobalVar"},
            {0x054860, 0x52, 0x3959D8, 0xC, 65, "Badge"},           {0x054800, 0x52, 0x3952B8, 0x1C, 65, "BadgeData"},
            {0x054940, 0x52, 0x395CE8, 0x38, 455, "Prizes"},        {0x05DAF0, 0x55, 0x39DDF8, 0xAC, 245, "Character"},
            {0x06B460, 0x52, 0x3A91C0, 0x40, 64, "bar_Floater"},    {0x06BD00, 0x52, 0x3AA1E8, 0x38, 64, "box_Floater"},
            {0x06C590, 0x52, 0x3AB010, 0x38, 64, "text_Floater"},   {0x079860, 0x52, 0x3AD830, 0x28, 1000, "Item"},
            {0x079920, 0x52, 0x3B9090, 0x10, 6528, "InventoryItem"}, {0x0798C0, 0x52, 0x3B7470, 0x18, 300, "ShopItemData"},
            {0x09BD80, 0x52, 0x3D51C0, 0x14, 840, "Encounter"},     {0x09BD20, 0x52, 0x3D44A0, 0x30, 70, "EncounterData"},
            {0x09BE40, 0x52, 0x3DA390, 0x14, 225, "League"},        {0x09BDE0, 0x52, 0x3D9360, 0x5C, 45, "LeagueData"},
            {0x09C020, 0x52, 0x3DDA68, 0x30, 45, "LeagueOffice"},   {0x09BF30, 0x52, 0x3DBFB8, 0x10, 225, "Recruit"},
            {0x09BED0, 0x52, 0x3DB528, 0xC, 225, "RecruitData"},    {0x09BF90, 0x52, 0x3DCDC8, 0x10, 200, "OpponentSchool"},
            {0x0B10F0, 0x52, 0x3EFE68, 0x28, 45, "Shop"},           {0x0B1090, 0x52, 0x3EF810, 0x24, 45, "ShopData"},
            {0x0B5F00, 0x52, 0x3F0BF8, 0x20, 4000, "Skill"},        {0x0C9050, 0x52, 0x410DA8, 0x2C, 45, "Town"},
            {0x0CA650, 0x52, 0x411578, 0x48, 245, "TrackerCharacter"}, {0x178F80, 0x52, 0x46C978, 0x14, 4000, "Texture"},
            {0x178EF0, 0x55, 0x41E778, 0xA0, 2000, "tex_Bitmap"},   {0x1C5380, 0x52, 0x4C5D88, 0x50, 160, "tint_Set"},
            {0x1E5E50, 0x52, 0x4CF208, 0x14, 32, "Pak"},
        };
        const DWORD scale = 4;
        int moved = 0;
        size_t bytes = 0;
        for (const auto &p : pools) {
            const DWORD n = p.count, N = p.count * scale;
            const DWORD from[5] = {p.base, p.base + p.elem, (n - 1) * p.elem, p.base + (n - 1) * p.elem, n * p.elem};
            BYTE *f = (BYTE *)p.init;
            int sites[8], vals[8], hits = 0;
            for (DWORD k = 0; k + 4 <= p.len && hits <= 6; k++)
                for (int v = 0; v < 5; v++)
                    if (*(DWORD *)(f + k) == from[v]) {
                        if (hits < 8) sites[hits] = k, vals[hits] = v;
                        hits++;
                        k += 3;
                        break;
                    }
            if (hits != 6) {
                port_log("game: pool %s - unexpected init at %06X (%d constants), not moved\n", p.name, p.init, hits);
                continue;
            }
            DWORD nb = (DWORD)(uintptr_t)game_alloc((size_t)N * p.elem, true);
            if (!nb) {
                port_log("game: pool %s - no memory for %u, not moved\n", p.name, N);
                continue;
            }
            const DWORD to[5] = {nb, nb + p.elem, (N - 1) * p.elem, nb + (N - 1) * p.elem, N * p.elem};
            for (int i = 0; i < 6; i++)
                *(DWORD *)(f + sites[i]) = to[vals[i]];
            moved++;
            bytes += (size_t)N * p.elem;
        }
        port_log("game: %d of %d object pools moved to container memory at %ux capacity (%.2f MB)\n", moved,
                 (int)(sizeof pools / sizeof pools[0]), scale, bytes / 1048576.0);
    }
    // [game] UnlockVersus=1: always offer the main menu's "VersuS" item
    if (GetPrivateProfileIntA("game", "UnlockVersus", 0, ini)) {
        WORD *jcc = (WORD *)0xFD5E0;                    // je 0xFD62E (74 4C)
        if (*jcc == 0x4C74) {
            *jcc = 0x9090;
            port_log("game: versus unlocked (main menu VersuS entry always shown)\n");
        } else {
            port_log("game: UnlockVersus - unexpected bytes %04X at 0xFD5E0, not patched\n", *jcc);
        }
    }
    // [game] UnlockArenas=1 (default): every versus arena reads as unlocked (the save's mask is untouched). Must match on
    // every machine of a netplay session.
    if (GetPrivateProfileIntA("game", "UnlockArenas", 1, ini)) {
        BYTE *rd = (BYTE *)0x1001DC;                    // movzx ecx, word ptr [0x4180FC]
        if (!memcmp(rd, "\x0F\xB7\x0D\xFC\x80\x41\x00", 7)) {
            static const BYTE all[7] = {0xB9, 0xFF, 0x7F, 0x00, 0x00, 0x90, 0x90};   // mov ecx, 0x7FFF
            memcpy(rd, all, 7);
            port_log("game: all 15 versus arenas unlocked\n");
        } else {
            port_log("game: UnlockArenas - unexpected bytes at 0x1001DC, not patched\n");
        }
    }
    // frame hook + deterministic-mode hooks (gn_hooks.cpp); [net] Netplay=1 (default) also loads gladiusnet.dll
    gn_install(GetPrivateProfileIntA("net", "Netplay", 1, ini) != 0);
    // [game] DevMenus=1: load the developer menus (their handlers are intact; only the loading was cut) by redirecting two
    // MenuListLoadMenu (0x5CD80) calls to port_menu_load.
    if (GetPrivateProfileIntA("game", "DevMenus", 1, ini) || port_is_editor()) {   // editor: its boot redirect
        // battle: show the pause menu's "Debug Menu" (kero_shortoption.mnu) by keeping console var KeroDebugMenu (0x37D024)
        // set; KeroberusState::InitState zeroes it at 0x7FD74
        BYTE *kd = (BYTE *)0x7FD74;
        if (!memcmp(kd, "\x89\x1D\x24\xD0\x37\x00", 6)) {
            memset(kd, 0x90, 6);
            *(DWORD *)0x37D024 = 1;
        } else {
            port_log("game: DevMenus - unexpected bytes at 0x7FD74, battle Debug Menu not enabled\n");
        }
        const DWORD sites[] = {0x161E9E /* splash_pressa.mnu */};
        for (DWORD site : sites) {
            BYTE *p = (BYTE *)site;
            if (p[0] == 0xE8 && site + 5 + *(int32_t *)(p + 1) == 0x5CD80)
                *(int32_t *)(p + 1) = (int32_t)((DWORD)port_menu_load - (site + 5));
            else
                port_log("game: DevMenus - unexpected call at %08X, not patched\n", site);
        }
        BYTE *wm = (BYTE *)0xF99E8;                     // call WorldMenu::IsOpen in the world map update
        if (wm[0] == 0xE8 && 0xF99E8 + 5 + *(int32_t *)(wm + 1) == 0x4D8D0)
            *(int32_t *)(wm + 1) = (int32_t)((DWORD)port_worldmenu_isopen - (0xF99E8 + 5));
        else
            port_log("game: DevMenus - unexpected world map call, Y world menu not enabled\n");
        DWORD *case4 = (DWORD *)(0x20DFC + 3 * 4);      // factory jump table, state 4 (SPLASH)
        if (*case4 == 0x206A1) {
            memcpy(g_splash_vtbl, (void *)0x33BF88, sizeof g_splash_vtbl);
            g_splash_vtbl[4] = 0x161E50;                // InitState
            g_splash_vtbl[7] = 0x1612E0;                // HandleState
            g_splash_vtbl[8] = 0x161C50;                // DrawState
            *case4 = (DWORD)splash_factory_case;
        } else {
            port_log("game: DevMenus - unexpected state factory table, splash state not restored\n");
        }
        BYTE *rs = (BYTE *)0x1EF40;                     // mov eax,[esp+4]; mov [0x38E43C],eax; ret
        if (*(DWORD *)rs == 0x0424448B && rs[4] == 0xA3 && *(DWORD *)(rs + 5) == 0x38E43C) {
            rs[0] = 0xE9;
            *(int32_t *)(rs + 1) = (int32_t)((DWORD)port_request_state - (0x1EF40 + 5));
            g_dev_splash_pending = !strstr(GetCommandLineA(), "-gnauto=");   // autotest boots to the main menu
        } else {
            port_log("game: DevMenus - unexpected RequestState bytes, boot splash not redirected\n");
        }
        port_log("game: developer menus enabled (splash startup menu, battle debug menu)\n");
    }
    port_log("settings: %s\n", port_ini_name());
    port_log("%s\n", PORT_RELEASE ? "GPBUILD=release" : "GPBUILD=dev");   // builder\build_exe.bat checks this tag
    // DEP: the converted exe has no NX_COMPAT flag, and netplay state syncs let a host write this process's game memory,
    // so data must never be executable. [debug] NoDEP=1 leaves it off (dev builds only).
    if (PORT_RELEASE || !GetPrivateProfileIntA("debug", "NoDEP", 0, ini)) {
        typedef BOOL(WINAPI * DepFn)(DWORD);
        DepFn dep = (DepFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessDEPPolicy");
        DWORD flags = 0;
        BOOL perm = FALSE;
        typedef BOOL(WINAPI * GetDepFn)(HANDLE, LPDWORD, PBOOL);
        GetDepFn get = (GetDepFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessDEPPolicy");
        bool ok = dep && dep(1 /* PROCESS_DEP_ENABLE */);
        if (get) get(GetCurrentProcess(), &flags, &perm);
        port_log("security: DEP %s (policy flags %lu%s)\n", ok || (flags & 1) ? "on" : "NOT on", flags,
                 perm ? ", permanent" : "");
    }
    port_game_resolution(ini);                          // [video] Resolution (game_resolution.cpp)
    port_tex_replace_init(ini);                         // [textures] Pack (tex_replace.cpp)
    d3d_install_fx_hooks();                             // particle renderer bracket (d3d_render.cpp)
    port_battle_trace_init();                           // skill stage tracer (battle_trace.cpp)
    port_battle_restart_init();                         // debug menu battle restart (loading screen fix)
    port_save_pages_init(ini);                          // save screen: pages of 3, up to 27 saves
    port_reboot_init();                                 // soft reboot -> seamless relaunch
    port_options_video_init();                          // Options screen: Video page
    port_menu_quit_init();                              // main menu: Quit Game
    port_mods_init();                                   // loose data\ files win over gladius.bec
    port_editor_init();                                 // gladius_editor.exe: the editor window
    HANDLE t = CreateThread(nullptr, 0x100000, game_thread, (LPVOID)info, CREATE_SUSPENDED, nullptr);
    port_pin_game_thread(t);
    port_register_game_thread(t, (const void *)info->game_main);
    port_apply_watch(t);
    ResumeThread(t);
    if (GetPrivateProfileIntA("debug", "Profile", 0, ini)) {
        HANDLE pt = CreateThread(nullptr, 0, profiler_thread, t, 0, nullptr);
        SetThreadPriority(pt, THREAD_PRIORITY_HIGHEST);
        CloseHandle(pt);
        port_log("debug: main-thread sampling profiler on (prof: lines every 10 s)\n");
    }
    // watchdog: while the game thread runs, dump where every game thread is every 6 s
    for (DWORD tick = 0; WaitForSingleObject(t, 50) == WAIT_TIMEOUT;) {
        port_thread_timer_tick();
        memwatch_apply_all();
        memstats_tick();
        if (tick % 1200 == 1199)                        // every 60 s
            container_report();
        if (g_tls_watch_req && g_watch_addr != g_tls_watch_req) {
            g_watch_addr = g_tls_watch_req;
            SuspendThread(t);
            port_apply_watch(t);
            ResumeThread(t);
            for (int i = 0; i < g_game_thread_count; i++) {
                SuspendThread(g_game_threads[i].handle);
                port_apply_watch(g_game_threads[i].handle);
                ResumeThread(g_game_threads[i].handle);
            }
            port_log("debug: TLS watch armed on %08X\n", g_watch_addr);
        }
        if (++tick % 120)
            continue;
        dump_thread(t, "watchdog main");
        extern volatile DWORD g_cri_owner, g_d3d_owner;
        extern volatile LONG g_swap_count;
        port_log("  CRI lock owner %u, D3D lock owner %u, frame %d\n", g_cri_owner, g_d3d_owner, g_swap_count);
        for (uint32_t q : {0x4FCA00u, 0x526EF0u}) {           // engine I/O queues: +0 busy, +0xC running, +0xD stop
            const BYTE *b = (const BYTE *)q;
            port_log("  queue %08X: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
                     q, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
        for (int i = 0; i < g_game_thread_count; i++) {
            char why[64];
            _snprintf(why, sizeof why, "watchdog thread %08X", g_game_threads[i].start);
            dump_thread(g_game_threads[i].handle, why);
        }
    }
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    ExitProcess(code);
}

// Engine halt (0x1D5D10, `jmp $`, also CRI's error callback): log the reason, then park the thread like the original.
extern "C" __declspec(dllexport) void __cdecl port_engine_halt(DWORD a, DWORD b)
{
    const char *msg = nullptr;
    __try {
        if (b && !IsBadStringPtrA((const char *)b, 512))
            msg = (const char *)b;
    } __except (1) {
    }
    port_log("ENGINE HALT (thread %u): args %08X %08X  \"%s\"\n", GetCurrentThreadId(), a, b, msg ? msg : "");
    for (;;)
        Sleep(1000);
}
