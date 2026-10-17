// gladiusport.dll - Windows replacement layers for the converted Gladius XBE (no kernel emulation).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

// Release build (src\build.bat release -> /DPORT_RELEASE=1): developer overrides that weaken security or drive
// the game from outside (NoDEP, AllowVersionMismatch, -gn* switches, autotest, record/replay,
// injected presses, hardware watches, page probes) are ignored.
#ifndef PORT_RELEASE
#define PORT_RELEASE 0
#endif

// Info block written by tools/xbe2pe.py into the exe's .port section ('PORT').
struct PortInfo {
    uint32_t magic;
    uint32_t game_main;          // main(0,0,0)
    uint32_t cxx_init_begin, cxx_init_end;
    uint32_t c_init_begin, c_init_end;
    uint32_t fpinit_ptr;
    uint32_t reserved;
    uint32_t rt_init_begin, rt_init_end;    // XAPI _rtinit table (CRT _mtinit, _ioinit)
    uint32_t sections, section_count;       // copy of the XBE section headers (56 bytes each)
};
extern const PortInfo *g_port_info;

void port_log(const char *fmt, ...);
void port_init_heap();
void port_init_files();
void port_thread_yield_point();     // release threads this thread created (Xbox start order)
void port_thread_timer_tick();      // release any pending thread older than 50 ms
void port_pin_game_thread(HANDLE thread);   // all game threads on one core (port_main.cpp)
const char *port_ini_name();                // "gladiusport.ini", or gladiusport_padN.ini for -gnpad=N when present
bool port_is_editor();                      // started as gladius_editor.exe (level editor mode, port_main.cpp)
void port_set_single_core(bool on);         // Options > Video "CPU Cores": re-pin / unpin the running game threads
void port_register_game_thread(HANDLE thread, const void *start);   // CPU time stats ([debug] Profile)
void port_thread_stats();                   // per frame: logs game threads' CPU use every 10 s
void port_fix_game_tls();                   // full-size game TLS block on this thread (port_main.cpp)
void port_restore_game_tls();               // undo it before the thread exits (port_main.cpp)
void port_apply_watch(HANDLE thread);   // [debug] WatchWrite hardware breakpoint
void gn_install(bool netplay);      // frame + deterministic-mode hooks; netplay: load gladiusnet.dll (gn_hooks.cpp)
extern "C" DWORD port_vtid();      // this thread's game-visible (virtual) id (xapi_thread.cpp)
extern "C" void port_set_vtid(DWORD vtid);
int port_lock_suspend();            // CRI lock: give up while blocking (cri_lock.cpp)
void port_lock_resume(int depth);
void port_set_elevated(bool on);    // single-core priority exclusion for the current thread (cri_lock.cpp)
bool port_close_find_handle(HANDLE h);
void port_forget_file_handle(HANDLE h);
const char *krnl_name(int i);
extern "C" void port_kernel_trap();
extern "C" __declspec(dllexport) void __stdcall port_start(const PortInfo *info);

// threads the game created (xapi_CreateThread), dumped by the watchdog
struct GameThread {
    HANDLE handle;
    uint32_t start;
};
extern GameThread g_game_threads[32];
extern int g_game_thread_count;
