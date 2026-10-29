// devtools.dll internals shared between its sources.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "devtools_api.h"

extern const DevHost *g_host;

#ifndef PORT_RELEASE                                    // src\build.bat release builds devtools.dll with /DPORT_RELEASE=1
#define PORT_RELEASE 0
#endif
// a release game or a release devtools.dll: nothing that writes process state to disk (crash reports, crashtest) -
// release builds make no dumps of any kind (port_main.cpp write_dump)
inline bool dt_release() { return PORT_RELEASE || (g_host && g_host->release); }

// crash-safe tails of the line stores (try-lock; false = the store was busy)
typedef void (*TailFn)(const char *line, void *ctx);
bool log_tail(TailFn fn, void *ctx, size_t n);          // port.log (devtools.cpp)
bool gamelog_tail(TailFn fn, void *ctx, size_t n);      // game output (gamelog.cpp)
bool con_tail(TailFn fn, void *ctx, size_t n);          // console output (console.cpp)
void crash_report(EXCEPTION_POINTERS *ep, const char *path, const char *why);   // crash.cpp
void crash_frame();                                     // crash.cpp: game thread, per frame

// ------------------------------------------------------------------------------------------------ console (console.cpp)
enum OutKind { OUT_TEXT, OUT_ECHO, OUT_ERROR, OUT_LOG, OUT_HELP };

struct OutLine {
    OutKind kind;
    std::string text;
};

// Retail console registry entry, copied on the game thread (the lists grow while subsystems initialise)
struct RegEntry {
    enum Kind { CMD, VAR, ALIAS } kind;
    std::string name, desc, alias_text;
    std::string usage;                                  // CMD: the usage text its handler passes to console_ShowUsage
    int vartype = -1, count = 0;                        // VAR: 0 UBYTE 1 SBYTE 2 USHORT 3 SSHORT 4 UINT 5 SINT 6 FLOAT 7 HEX
    float min = 0, max = 0;
    void *data = nullptr;
    uint16_t flags = 0;
};

void con_init();
void con_shutdown();                                    // online lobby: usage hook out
void con_print(OutKind kind, const char *fmt, ...);
void con_submit(const char *line);                      // any thread: queued, run on the game thread
void con_frame();                                       // game thread: drain the queue, refresh the registry
std::vector<OutLine> con_lines(size_t *gen);            // snapshot of the output (gen = change counter)
void con_clear();
std::vector<RegEntry> con_registry(size_t *gen);        // last registry snapshot
std::string con_var_value(const RegEntry &v);           // current value, read live
std::vector<std::string> con_complete(const std::string &prefix);   // names starting with prefix
const std::vector<std::string> &con_history();
bool con_online();                                      // netplay: state-changing commands are blocked

// ------------------------------------------------------------------------------------------------ game output (gamelog.cpp)
void gamelog_init();
void gamelog_shutdown();                                // online lobby: output hooks out
std::vector<std::string> gamelog_lines(size_t *gen);
void gamelog_clear();
void gamelog_print(const char *line);                    // one line into the Game log (+ port.log)
extern volatile long g_exec_depth;                      // a typed command is running: its output shows in the console
std::vector<std::string> log_lines(size_t *gen);        // port.log (devtools.cpp)

// ------------------------------------------------------------------------------------------------ command output (cmd_output.cpp)
bool cmdout_pre(const std::string &cmd, const std::vector<std::string> &args);   // false = skip the retail handler
void cmdout_post(const std::string &cmd, const std::vector<std::string> &args);

// ------------------------------------------------------------------------------------------------ in-game console
bool ingame_open();
int ingame_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void ingame_draw();                                     // game thread

// ------------------------------------------------------------------------------------------------ graphs (graphs.cpp)
void graphs_frame();                                    // game thread, per frame: sample
void panel_graphs();                                    // tools window tab

// ------------------------------------------------------------------------------------------------ script debugger (dogdbg.cpp)
enum DbgCmd { DCMD_NONE, DCMD_CONTINUE, DCMD_STEP, DCMD_STEP_INSTR, DCMD_OVER, DCMD_OUT };
void dogdbg_init();                                     // load: script error log hook
void dogdbg_shutdown();                                 // online lobby: every hook out (game thread)
void dogdbg_frame();                                    // game thread, per frame: hook in / out
bool dogdbg_stopped();                                  // the game thread is parked at a break
void dogdbg_command(int cmd);
void dogdbg_break_next();
std::string dogdbg_swap_file(const std::string &name, const std::string &path);   // console scriptswap
std::string dogdbg_reset_files(const std::vector<std::string> &names, bool run_kill, bool run_init); // scriptreset                               // switch on + stop at the next instruction                           // DbgCmd, while stopped
void panel_scripts();                                   // tools window tab
bool dogdbg_show_code(const std::string &file, uint32_t ip);   // show a loaded file's code at an offset (false: not loaded)
int dogdbg_handler_count(const std::string &file, const std::string &func);   // times registered as an event handler now
void dogdbg_dedup_all();                                // console scriptdedup: next frame

// ------------------------------------------------------------------------------------------------ quest flow (flow.cpp)
void flow_frame();                                      // game thread, per frame: queued writes, change history
void panel_quests();                                    // tools window tab
void flow_gamevar(const std::string &arg);              // console gamevar (game thread)
void flow_gameflow(const std::string &arg);             // console gameflow
// what a script function does to the game variables ("ImperiaTownsToBeat -1; sets X"), "" before the map is built
std::string flow_effects(const std::string &file, const std::string &func, bool counters_only);
std::string flow_counting_handlers(const std::string &file);   // its counting event handlers, for reset warnings

// ------------------------------------------------------------------------------------------------ tools window (tools_window.cpp)
void tools_window_start(bool show);                     // its own thread + window
void tools_window_stop();                               // online lobby: window closed, thread ends
void tools_window_toggle();
void tools_window_show();                               // any thread: show and bring to front
