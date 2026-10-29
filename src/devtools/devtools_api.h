// Interface between gladiusport.dll (devtools_host.cpp) and devtools.dll (developer tools: console, variables,
// log, graphs, Dog script debugger). Plain C across the boundary (both link the CRT statically); bump DEVTOOLS_ABI on
// any change.
#pragma once
#include <stdint.h>
#include <windows.h>

#define DEVTOOLS_ABI 6

// Graphs: the last finished frame (d3d_prof.h ProfLast)
enum { DEV_ZONES = 32, DEV_PASSES = 16, DEV_COUNTERS = 16 };
struct DevFrameStats {
    uint32_t frame;                                     // Present count (0 = none yet)
    float wall_ms, cpu_ms, present_ms;                  // Present to Present, game + draw submission, blocked in Present
    uint32_t draws;
    bool prof;                                          // profiler on and the fields below valid
    bool prof_ini;                                      // on by [debug] Profile (can't be switched off live)
    int n_zones, n_passes, n_counters;
    float game_ms, zone_ms[DEV_ZONES], gpu_ms, gpu_pass_ms[DEV_PASSES];
    uint32_t gpu_frame;                                 // the frame the GPU numbers belong to (a few frames back)
    uint32_t counters[DEV_COUNTERS];
};
struct DevArena {                                       // memory container arena (container.h)
    const char *name;
    uint32_t size, used, hwm;                           // bytes: reserved, in use, high-water mark of addresses
    long failed;                                        // allocations that did not fit
};

// What gladiusport.dll offers the tools
struct DevHost {
    uint32_t abi;
    const char *ini;                                    // gladiusport.ini path
    void(__cdecl *log)(const char *fmt, ...);           // port.log
    bool(__cdecl *net_running)();                       // online lockstep session active: no state-changing commands
    HWND(__cdecl *game_window)();
    // port overlay (overlay.h), game thread
    void(__cdecl *ov_begin)();
    void(__cdecl *ov_box)(float x, float y, float w, float h, uint32_t argb);
    void(__cdecl *ov_text)(const char *s, float x, float y, int align, uint32_t argb, bool sm);
    float(__cdecl *ov_text_width)(const char *s, bool sm);
    void(__cdecl *ov_end)(bool visible);
    void(__cdecl *ov_panel)(bool on);
    // graphs
    void(__cdecl *frame_stats)(DevFrameStats *out);     // game thread
    void(__cdecl *prof_live)(bool on);                  // renderer profiler without [debug] Profile (next frame)
    const char *(__cdecl *prof_name)(int kind, int i);  // kind 0 CPU zone, 1 GPU pass, 2 counter
    bool(__cdecl *arena)(int i, DevArena *out);         // any thread; false = no such arena / container off
    // script debugger (game thread)
    void(__cdecl *audio_pause)(bool on);                // hold the mixer while the game thread is stopped at a break
    bool(__cdecl *locks_held)();                        // this thread owns the D3D or CRI lock: don't stop here
    // game data (any thread): "data/script/bin/x.scb" - the loose copy under game_dir, else gladius.bec. The buffer is
    // the host's: give it back with free_game_file
    const char *game_dir;
    void *(__cdecl *read_game_file)(const char *key, uint32_t *size);
    void(__cdecl *free_game_file)(void *p);
    // release game (gladiusport.dll built with PORT_RELEASE): no crash reports or crash commands, whatever this
    // devtools.dll was built as
    bool release;
};

// What devtools.dll offers the host
struct DevToolsApi {
    uint32_t abi;
    void(__cdecl *frame)();                             // game thread, once per frame (gn frame hook)
    int(__cdecl *wndproc)(HWND h, UINT msg, WPARAM wp, LPARAM lp);   // game window: nonzero = consumed
    void(__cdecl *log_line)(const char *line);          // every port.log line, any thread
    bool(__cdecl *keyboard_captured)();                 // in-game console open: keyboard is not a gamepad
    // a fatal crash, before the dump is written (crashing thread, other threads still running): write the readable
    // report to path. why = "crash" / "abort" / "purecall" / "hang"; ep may be NULL
    void(__cdecl *crash_report)(EXCEPTION_POINTERS *ep, const char *path, const char *why);
    // an online lobby was hosted / joined (game thread): take every code patch out and close the tools window. The
    // host never calls the tools again (they stay off until the game restarts); the DLL stays loaded.
    void(__cdecl *shutdown)();
};

typedef const DevToolsApi *(__cdecl *DevToolsEntry)(const DevHost *host);
