// Dog script debugger (the "Scripts" tab). Retail keeps the whole Dog VM but compiled the debugger out; this rebuilds
// one from the outside (map: scratchpad dog_vm_map.md):
//   0x1C6920  Script::Run's call to ReadNextOpcode: every instruction passes here first (esi = Script*, +0x1E0 = the
//             offset of the instruction about to run) - breakpoints, Break, stepping. Patched only while the debugger
//             is switched on, offline.
//   0x18ADA0  the DebugBreak() native (a stub returning 0): stops in the calling script.
//   0x184580  Script::RuntimeError(fmt, ...): the message retail throws away goes to the Game log; optionally stops.
// Stopping = parking the game thread inside the hook (audio held) while the tools window, on its own thread, reads
// the stopped VM. Release bytecode has no line numbers: breakpoints are code offsets in a disassembly, and "Step"
// runs to the next statement start, guessed from the instruction that ended the previous one.
// Online (netplay) nothing stops and nothing is written; the instruction hook comes out when a session starts.
#include "devtools.h"
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "imgui.h"

// ------------------------------------------------------------------------------------------------ VM layout
enum : uint32_t {
    SHARED_BIT = 0x20000000, GLOBAL_OBJ = 0x40000000,
    CUR_SCRIPT = 0x483230, TURN_DEPTH = 0x4C90D4, TURN_STACK = 0x4C90AC,
    SCRIPT_LIST = 0x483450, FILE_HASH = 0x483250, OP_TABLE = 0x380088,
};
enum Op {
    OP_GOENDWHILE = 6, OP_ENDWHILE = 7, OP_CALL = 9, OP_IF = 11, OP_ELSEIF = 12, OP_PUSHINT = 42, OP_PUSHFLOAT = 43,
    OP_PUSHSTRING = 44, OP_PUSHVECTOR = 45, OP_CALLFUNC = 53, OP_FUNCLABEL = 74, OP_BRA_ENDIF = 76, OP_PRINT_STRING = 83,
    OP_COUNT = 84
};
static const char *const OP_NAMES[OP_COUNT] = {
    "INVALID", "NOP", "ARG", "STATEMENT", "JUMPTARGET", "WHILE", "GOENDWHILE", "ENDWHILE", "GOTO", "CALL", "STOP", "IF",
    "ELSEIF", "ELSE", "ENDIF", "RETURN", "RETURNVAL", "DISCARD", "FOR", "SKIP", "NEXT_SYM", "PLUS_PLUS", "MINUS_MINUS",
    "PLUS_EQUALS", "MINUS_EQUALS", "MULT_EQUALS", "DIV_EQUALS", "OR_EQUALS", "AND_EQUALS", "XOR_EQUALS", "MOD_EQUALS",
    "SWITCH", "ENDSWITCH", "CASE", "BREAK", "CONTINUE", "DEFAULT", "INT", "FLOAT", "STRING", "VECTOR", "ARTICLE",
    "PUSHINT", "PUSHFLOAT", "PUSHSTRING", "PUSHVECTOR", "PUSHSYMBOL", "POPINT", "POPFLOAT", "POPSTRING", "POPVECTOR",
    "POPSYMBOL", "ARRAYREF", "CALLFUNC", "UNEG", "LNEG", "MULTIPLY", "DIVIDE", "MOD", "ADD", "SUB", "LT", "GT", "LE", "GE",
    "EQ", "NE", "AND", "XOR", "OR", "LAND", "LOR", "ASSIGN", "GOTRUE", "FUNCLABEL", "GOTOLABEL", "BRA_ENDIF", "LINE",
    "ALLOC_INT_ARRAY", "ALLOC_FLOAT_ARRAY", "ALLOC_ARRAY", "ALLOC_ARTICLE", "PRINT_ID", "PRINT_STRING"};
// operand counts (0 / 1 / 3), from the game's own opcode table at start
static uint8_t g_nops[OP_COUNT];
// ops whose operand 0 is a symbol
static bool sym_op(int op)
{
    switch (op) {
    case 8: case 37: case 38: case 39: case 40: case 41: case 46: case 47: case 48: case 49: case 51: case 74: case 75:
    case 82: return true;
    }
    return false;
}
// instructions that end a statement: the one after them starts the next ("Step")
static bool ends_statement(int op)
{
    switch (op) {
    case 5: case 6: case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 18: case 19:
    case 20: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 41: case 74:
    case 75: case 76: case 78: case 79: case 80: case 81: return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ guarded reads
static bool rd(uint32_t addr, void *out, size_t n)
{
    if (!addr) return false;
    __try {
        memcpy(out, (const void *)(uintptr_t)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static uint32_t rd32(uint32_t a) { uint32_t v = 0; return rd(a, &v, 4) ? v : 0; }
static uint8_t rd8(uint32_t a) { uint8_t v = 0; return rd(a, &v, 1) ? v : 0; }
static int16_t rd16s(uint32_t a) { int16_t v = 0; return rd(a, &v, 2) ? v : 0; }
static bool wr(uint32_t addr, const void *in, size_t n)
{
    __try {
        memcpy((void *)(uintptr_t)addr, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static std::string rd_str(uint32_t a, size_t cap = 200)
{
    std::string s;
    for (char c; s.size() < cap && rd(a + (uint32_t)s.size(), &c, 1) && c;) s += c >= 32 && c < 127 ? c : '?';
    return s;
}

// ScriptFile fields
static std::string file_name(uint32_t f)
{
    uint32_t b = rd32(f + 0x10);
    return b ? rd_str(b + 0xC) : "?";
}
static std::string sstr(uint32_t f, uint32_t off)
{
    uint32_t base = rd32(f + 0x64), size = rd32(f + 0x68);
    return base && off < size ? rd_str(base + off) : "?";
}
static std::string sym_name(uint32_t f, uint32_t idx)
{
    bool sh = (idx & SHARED_BIT) != 0;
    uint32_t i = idx & ~SHARED_BIT, tab = rd32(f + (sh ? 0x4C : 0x40)), n = rd32(f + (sh ? 0x54 : 0x48));
    if (!tab || i >= n) {
        char b[24];
        sprintf(b, "sym%X", idx);
        return b;
    }
    return sstr(f, rd32(tab + i * 12));
}

// the function a script is in: +0x1CC is set by FUNCLABEL, which a function the game starts directly (DoExecute)
// has not run yet - then the entry function +0x1D0
static uint32_t cur_func(uint32_t s)
{
    int32_t f = (int32_t)rd32(s + 0x1CC);
    return f >= 0 ? (uint32_t)f : rd32(s + 0x1D0);
}

// short file name for lists: "d:\data\script\Bin\X.scb" -> "X.scb"
static std::string base_name(const std::string &p)
{
    size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

// a DogVal {type flags, value}
static std::string fmt_val(uint32_t type, uint32_t v, int depth = 0)
{
    char b[300];
    switch (type & 0xFF) {
    case 0: sprintf(b, v ? "object %08X" : "null object", v); break;
    case 1: sprintf(b, "%d", (int8_t)v); break;
    case 2: sprintf(b, "%d", (int32_t)v); break;
    case 3: { float f; memcpy(&f, &v, 4); sprintf(b, "%g", f); break; }
    case 4: _snprintf(b, sizeof b, "\"%s\"", v ? rd_str(v, 240).c_str() : ""); break;
    case 5: _snprintf(b, sizeof b, "\"%s\"", v ? rd_str(v + 0xC, 240).c_str() : ""); break;
    case 0x10:                                          // reference to another symbol's value
        if (depth < 3 && v) return "-> " + fmt_val(rd32(v + 4), rd32(v + 8), depth + 1);
        sprintf(b, "symbol %08X", v);
        break;
    default: sprintf(b, "type %X: %08X", type, v); break;
    }
    b[sizeof b - 1] = 0;
    return b;
}
static const char *type_name(uint32_t type)
{
    switch (type & 0xFF) {
    case 0: return "object";
    case 1: return "int8";
    case 2: return "int";
    case 3: return "float";
    case 4: case 5: return "string";
    case 0x10: return "ref";
    }
    return "?";
}

// ------------------------------------------------------------------------------------------------ state
static volatile bool g_want_on;                         // UI: debugger switched on
static bool g_installed;                                // game thread: instruction hook in place
static volatile bool g_armed;                           // the hook does anything (read by the thunk)
static volatile bool g_break_next;                      // stop at the next instruction of any script
static volatile bool g_break_on_error = true, g_break_on_debugbreak = true;
static DWORD g_game_tid;

static volatile LONG g_cmd;                             // UI -> parked game thread
enum StepMode { STEP_NONE, STEP_INTO, STEP_INSTR, STEP_OVER, STEP_OUT };
static struct {
    StepMode mode;
    uint32_t s, id;                                     // the script being stepped (pointer + unique id)
    int depth, turn;                                    // its call depth and the nesting depth when stepping began
    int last_op;                                        // the last instruction it ran (statement boundaries)
    int last_line;                                      // ... and its source line (-1: no decompiled source)
} g_step;

struct Stop {                                           // where the game thread is parked
    uint32_t s, id, file, ip;
    bool after;                                         // stopped inside an instruction (DebugBreak, script error)
    char reason[300];
};
static Stop g_stop;
static volatile bool g_stopped;
static char g_status[300] = "";                         // last stop / step message for the UI

struct Bp {
    std::string file;
    uint32_t ip;
};
static std::mutex g_bp_lock;
static std::vector<Bp> g_bps;
static volatile LONG g_bp_gen, g_bp_count;

// execution trace of the file in the code view: every instruction any script runs in it (game thread writes)
static const int TRACE_RING = 8192;
static volatile uint32_t g_trace_file;
static uint32_t g_trace_ring[TRACE_RING];
static volatile LONG g_trace_n;

// source line tables (decompiler maps) the game thread steps by: ScriptFile* -> per code byte, its source line
struct LineTable {
    uint32_t id;
    std::vector<int> line;                              // -1 = no line
    std::vector<uint8_t> start;                         // a run of a line's code starts here
};
static std::mutex g_lt_lock;
static std::unordered_map<uint32_t, std::shared_ptr<LineTable>> g_line_tables;
static std::shared_ptr<LineTable> line_table(uint32_t file)
{
    std::lock_guard<std::mutex> l(g_lt_lock);
    auto it = g_line_tables.find(file);
    if (it == g_line_tables.end() || it->second->id != rd32(file + 0xC)) return nullptr;
    return it->second;
}
static int line_at(uint32_t file, uint32_t off)
{
    auto lt = line_table(file);
    return lt && off < lt->line.size() ? lt->line[off] : -1;
}

static void update_armed()
{
    g_armed = g_installed && (g_break_next || g_step.mode != STEP_NONE || g_bp_count > 0 || g_trace_file);
}

// ------------------------------------------------------------------------------------------------ game thread: stops
static void tools_show_scripts();

// Park the game thread until the UI says how to go on. ip = the instruction about to run, or (after) the one
// that is running.
static void stop_here(uint32_t s, uint32_t ip, bool after, const char *reason)
{
    if (g_host->net_running() || GetCurrentThreadId() != g_game_tid) return;
    if (g_host->locks_held()) {                         // a renderer / movie lock is held: stop a little later
        g_break_next = true;
        update_armed();
        return;
    }
    Stop &st = g_stop;
    st.s = s, st.id = rd32(s + 0x1B4), st.file = rd32(s + 0x80), st.ip = ip, st.after = after;
    _snprintf(st.reason, sizeof st.reason - 1, "%s", reason);
    g_break_next = false;
    g_step.mode = STEP_NONE;
    _snprintf(g_status, sizeof g_status - 1, "Stopped: %s - %s, %s @%04X", reason, file_name(st.file).c_str(),
              sym_name(st.file, cur_func(s)).c_str(), ip);
    g_host->log("dogdbg: %s\n", g_status);
    InterlockedExchange(&g_cmd, DCMD_NONE);
    g_host->audio_pause(true);
    g_stopped = true;
    tools_show_scripts();
    LONG cmd;
    while ((cmd = InterlockedExchange(&g_cmd, DCMD_NONE)) == DCMD_NONE && g_want_on) Sleep(5);
    g_stopped = false;
    g_host->audio_pause(false);
    g_step.s = s, g_step.id = st.id, g_step.depth = rd16s(s + 0x1BE), g_step.turn = (int)rd32(TURN_DEPTH);
    g_step.last_op = -1;
    g_step.last_line = line_at(st.file, after && ip ? ip - 1 : ip);
    switch (cmd) {
    case DCMD_STEP: g_step.mode = STEP_INTO; break;
    case DCMD_STEP_INSTR: g_step.mode = STEP_INSTR; break;
    case DCMD_OVER: g_step.mode = STEP_OVER; break;
    case DCMD_OUT:
        if (g_step.depth > 0) g_step.mode = STEP_OUT;
        else _snprintf(g_status, sizeof g_status - 1, "Step out at the outermost function: running on");
        break;
    default: g_status[0] = 0; break;
    }
    if (g_step.mode != STEP_NONE) strcpy(g_status, "Stepping...");
    update_armed();
}

static bool bp_hit(uint32_t s, uint32_t ip)
{
    if (!g_bp_count) return false;
    struct Cache {
        uint32_t id;
        LONG gen = -1;
        std::vector<uint32_t> ips;
    };
    static std::unordered_map<uint32_t, Cache> cache;  // ScriptFile* -> its breakpoint offsets
    uint32_t f = rd32(s + 0x80), id = rd32(f + 0xC);
    Cache &c = cache[f];
    if (c.gen != g_bp_gen || c.id != id) {
        std::string name = file_name(f);
        std::lock_guard<std::mutex> l(g_bp_lock);
        c.gen = g_bp_gen, c.id = id;
        c.ips.clear();
        for (auto &b : g_bps)
            if (b.file == name) c.ips.push_back(b.ip);
        std::sort(c.ips.begin(), c.ips.end());
    }
    return std::binary_search(c.ips.begin(), c.ips.end(), ip);
}

// every instruction while armed, before it is read (game thread)
static void __cdecl on_instr(uint32_t s)
{
    uint32_t ip = rd32(s + 0x1E0);
    if (g_trace_file && rd32(s + 0x80) == g_trace_file) {
        g_trace_ring[g_trace_n & (TRACE_RING - 1)] = ip;
        g_trace_n = g_trace_n + 1;
    }
    int op = rd8(rd32(s + 0x1DC) + ip);
    const char *why = nullptr;
    if (g_break_next) why = "Break";
    else if (g_step.mode != STEP_NONE) {
        bool target = s == g_step.s && rd32(s + 0x1B4) == g_step.id;
        int depth = rd16s(s + 0x1BE), turn = (int)rd32(TURN_DEPTH);
        bool nested = turn > g_step.turn && !target;    // a script the stepped one started (through a native)
        // a new statement: with decompiled source, the start of a different source line; without, the
        // instruction after one that ends a statement
        std::shared_ptr<LineTable> lt = target ? line_table(rd32(s + 0x80)) : nullptr;
        int line = lt && ip < lt->line.size() ? lt->line[ip] : -1;
        bool stmt = lt ? line >= 0 && lt->start[ip] && line != g_step.last_line
                       : g_step.last_op >= 0 && ends_statement(g_step.last_op);
        switch (g_step.mode) {
        case STEP_INSTR: if (target || nested) why = "step"; break;
        case STEP_INTO: if ((target && stmt) || nested) why = "step"; break;
        case STEP_OVER: if (target && stmt && depth <= g_step.depth && turn <= g_step.turn) why = "step"; break;
        case STEP_OUT: if (target && stmt && depth < g_step.depth) why = "step out"; break;
        default: break;
        }
        if (target) {
            g_step.last_op = op;
            if (line >= 0) g_step.last_line = line;
        }
    }
    if (!why && bp_hit(s, ip)) why = "breakpoint";
    if (why) stop_here(s, ip, false, why);
}

// DebugBreak() native: [esp+8] = the calling Script*
static void __cdecl on_debugbreak(uint32_t s)
{
    if (g_break_on_debugbreak && s) stop_here(s, rd32(s + 0x1E0), true, "DebugBreak()");
}

// Script::RuntimeError(fmt, ...): args = &fmt
static void format_guarded(char *out, size_t cap, uint32_t *args)   // no C++ objects here (__try)
{
    __try {
        _vsnprintf(out, cap - 1, (const char *)args[0], (va_list)&args[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy(out, "(unreadable message)");
    }
    out[cap - 1] = 0;
}

static void __cdecl on_runtime_error(uint32_t *args)
{
    char msg[512] = "";
    format_guarded(msg, sizeof msg, args);
    for (char *p = msg; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    uint32_t s = rd32(CUR_SCRIPT);
    char line[800];
    if (s) {
        uint32_t f = rd32(s + 0x80);
        _snprintf(line, sizeof line - 1, "script error: %s  [%s, %s @%04X]", msg, file_name(f).c_str(),
                  sym_name(f, cur_func(s)).c_str(), rd32(s + 0x1E0));
    } else
        _snprintf(line, sizeof line - 1, "script error: %s", msg);
    line[sizeof line - 1] = 0;
    gamelog_print(line);
    if (s && g_break_on_error && g_installed) {
        char why[600];
        _snprintf(why, sizeof why - 1, "script error: %s", msg);
        why[sizeof why - 1] = 0;
        stop_here(s, rd32(s + 0x1E0), true, why);
    }
}

// ------------------------------------------------------------------------------------------------ patches
static __declspec(naked) void instr_thunk()
{
    __asm {
        cmp byte ptr [g_armed], 0
        je go
        pushad
        push esi
        call on_instr
        add esp, 4
        popad
    go:
        mov eax, 0x1C6510                               // ReadNextOpcode (thiscall, ecx untouched)
        jmp eax
    }
}

static __declspec(naked) void debugbreak_thunk()
{
    __asm {
        pushad
        push dword ptr [esp + 0x20 + 8]                 // Script*
        call on_debugbreak
        add esp, 4
        popad
        push ecx                                        // displaced: push ecx; mov eax,[esp+8]
        mov eax, [esp + 8]
        push 0x18ADA5
        ret
    }
}

static __declspec(naked) void runtime_error_thunk()
{
    __asm {
        pushad
        lea eax, [esp + 0x20 + 4]                       // &fmt
        push eax
        call on_runtime_error
        add esp, 4
        popad
        mov eax, dword ptr ds:[0x483230]                // displaced
        push 0x184585
        ret
    }
}

static bool patch(uint32_t at, const uint8_t *expect, uint8_t opcode, void *to, uint8_t *saved, const char *what)
{
    uint8_t *p = (uint8_t *)(uintptr_t)at;
    if (expect && memcmp(p, expect, 5)) {
        g_host->log("dogdbg: %s at %06X not as expected - not hooked\n", what, at);
        return false;
    }
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    if (saved) memcpy(saved, p, 5);
    p[0] = opcode;
    *(int32_t *)(p + 1) = (int32_t)((uint8_t *)to - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
static void unpatch(uint32_t at, const uint8_t *saved)
{
    uint8_t *p = (uint8_t *)(uintptr_t)at;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, saved, 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

static const uint8_t k_instr_call[5] = {0xE8, 0xEB, 0xFB, 0xFF, 0xFF};       // call 0x1C6510
static const uint8_t k_debugbreak[5] = {0x51, 0x8B, 0x44, 0x24, 0x08};       // push ecx; mov eax,[esp+8]
static const uint8_t k_runtime_error[5] = {0xA1, 0x30, 0x32, 0x48, 0x00};    // mov eax,[0x483230]
static uint8_t g_saved_instr[5], g_saved_db[5], g_saved_err[5];

static void install(bool on)                            // game thread, between frames
{
    if (on) {
        if (!patch(0x1C6920, k_instr_call, 0xE8, (void *)instr_thunk, g_saved_instr, "instruction hook")) {
            g_want_on = false;
            return;
        }
        patch(0x18ADA0, k_debugbreak, 0xE9, (void *)debugbreak_thunk, g_saved_db, "DebugBreak native");
        g_installed = true;
        g_host->log("dogdbg: script debugger on\n");
    } else {
        unpatch(0x1C6920, g_saved_instr);
        if (g_saved_db[0]) unpatch(0x18ADA0, g_saved_db);
        g_installed = false;
        g_break_next = false;
        g_step.mode = STEP_NONE;
        g_host->log("dogdbg: script debugger off\n");
    }
    update_armed();
}

void dogdbg_init()                                      // devtools load: opcode operand counts, script error log
{
    for (int i = 0; i < OP_COUNT; i++) {
        uint32_t fl = rd32(OP_TABLE + i * 12) & 0xFF;
        g_nops[i] = fl >= 1 ? (uint8_t)((fl - 1) / 4) : 0;
        if (g_nops[i] == 2 || g_nops[i] > 3) g_nops[i] = 0;
    }
    patch(0x184580, k_runtime_error, 0xE9, (void *)runtime_error_thunk, g_saved_err, "Script::RuntimeError");
    g_want_on = GetPrivateProfileIntA("devtools", "ScriptDebugger", 0, g_host->ini) != 0;   // on from the start
}

// ------------------------------------------------------------------------------------------------ sandbox: swap / reset
// Replaces loaded script files' bytecode while the game runs (game thread, between frames: no script is executing),
// several files in the same frame. The ScriptFile object stays (every Script, call frame and the file table keep pointing
// at it); it gets a new image and the game's own parser (InitData 0x1C7260) rebuilds its tables. Each instance gets a
// fresh symbol table (AllocSymbols 0x1843B0).
//   carry (sandbox apply): values move across by name (shared variables; instance variables, locals matched as "name
//     in function"); a running instance at the top level restarts the function it is in, idle ones take the new code
//     on their next call
//   reset: everything starts from the file's defaults and running instances stop; optionally each instance's
//     shutdown function (OnKill / OnKillBattle ...) runs on the old state first (off by default: teardown code often
//     moves the game on - 50 retail OnKills call TriggerNewState, which ends a world map scene) and its start-up
//     function (OnInit / OnInitBattle ...) on the new - only ones without parameters (unit / class scripts get theirs
//     from the game), and never one that changes the game state (k_state_natives, checked in the function and the
//     local functions it calls)
// Instances inside a call (or other scripts mid-call into the file) make the whole batch wait for the top level.
// The old image and tables are kept (a few KB): strings and references into them may still be held anywhere, and
// freeing a table walks the entry count of the file's *current* template.
struct SwapItem {
    uint32_t file, id;
    std::vector<uint8_t> img;
};
struct SwapReq {
    std::vector<SwapItem> items;
    bool carry = true, run_kill = false, run_init = false;
    std::string what;                                   // "apply" / "revert" / "reset" (status text)
};
static std::mutex g_swap_lock;
static SwapReq *g_swap_req;                             // UI -> game thread (owned by whoever holds it)
static char g_swap_msg[600];
static volatile LONG g_swap_done;                       // completed swaps (UI reloads the file model)

typedef void *(__cdecl *GameAllocFn)(uint32_t tag, uint32_t size);
typedef bool(__fastcall *InitDataFn)(uint32_t file, uint32_t edx);
typedef uint32_t(__cdecl *LoadFileFn)(const char *name, int flags);
typedef bool(__fastcall *ConnectFn)(uint32_t script, uint32_t edx, uint32_t file);
typedef void(__fastcall *AllocSymFn)(uint32_t script, uint32_t edx);
typedef void(__cdecl *ExecFn)(uint32_t obj, uint32_t *ret, const char *func, int argc);
typedef void(__fastcall *DogValDtorFn)(uint32_t *val, uint32_t edx);

static const char *const k_kill_funcs[] = {"OnKill", "OnKillBattle", "OnKillClass", "OnKillUnit"};
static const char *const k_init_funcs[] = {"OnInit", "OnInitBattle", "OnInitClass", "OnInitUnit"};
// natives that move the game to another state / scene: a lifecycle function calling one is not run by a reset
static const char *const k_state_natives[] = {"TriggerNewState", "RequestState", "CutSceneComplete", "RequestCutScene",
                                              "ExitShop", "DemoQuit"};

// symbol table entry names: "x#N" (local of function symbol N) -> "x#<function name>"; others unchanged
static std::string canon_name(uint32_t sstr_base, uint32_t sstr_size, uint32_t tmpl, uint32_t count, uint32_t name_off)
{
    std::string n = name_off < sstr_size ? rd_str(sstr_base + name_off, 300) : "";
    size_t h = n.find('#');
    if (h == std::string::npos) return n;
    uint32_t fi = (uint32_t)strtoul(n.c_str() + h + 1, nullptr, 10);
    std::string fn = fi < count ? rd_str(sstr_base + rd32(tmpl + fi * 12), 300) : n.substr(h + 1);
    return n.substr(0, h + 1) + fn;
}

struct TableView {                                      // a symbol table + the file data to name its entries
    uint32_t tab, count, sstr, sstr_size, tmpl, tmpl_count;
};
static std::unordered_map<std::string, uint32_t> table_names(const TableView &v)
{
    std::unordered_map<std::string, uint32_t> m;        // canonical name -> entry address
    for (uint32_t i = 0; i < v.count && i < 20000; i++) {
        uint32_t e = v.tab + i * 12;
        m.emplace(canon_name(v.sstr, v.sstr_size, v.tmpl, v.tmpl_count, rd32(e)), e);
    }
    return m;
}

// move each value whose name exists in both tables (the old entry is cleared: the value has one owner)
static int move_values(const TableView &from, const TableView &to, bool self_only)
{
    auto old = table_names(from);
    int moved = 0;
    for (uint32_t i = 0; i < to.count && i < 20000; i++) {
        if (self_only && i) break;                      // a reset keeps only 'self' (entry 0, the owning object)
        uint32_t e = to.tab + i * 12;
        auto it = old.find(canon_name(to.sstr, to.sstr_size, to.tmpl, to.tmpl_count, rd32(e)));
        if (it == old.end()) continue;
        uint32_t val[2] = {rd32(it->second + 4), rd32(it->second + 8)}, zero[2] = {0, 0};
        wr(e + 4, val, 8);
        wr(it->second + 4, zero, 8);
        moved++;
    }
    return moved;
}

static TableView file_tables(uint32_t f, bool shared)
{
    TableView v;
    v.tab = rd32(f + (shared ? 0x4C : 0x40));
    v.count = rd32(f + (shared ? 0x54 : 0x48));
    v.sstr = rd32(f + 0x64), v.sstr_size = rd32(f + 0x68);
    v.tmpl = rd32(f + 0x40), v.tmpl_count = rd32(f + 0x48);
    return v;
}

// function `name` in the file: code offset after its parameter pops (-1 = not there), its symbol, parameter count
static int32_t body_start(uint32_t f, const std::string &name, uint32_t *fn_sym, int *params = nullptr)
{
    uint32_t code = rd32(f + 0x70), size = rd32(f + 0x74);
    for (uint32_t p = 0; p < size;) {
        uint8_t op = rd8(code + p);
        uint32_t len = 1 + 4 * (op < OP_COUNT ? g_nops[op] : 0);
        if (op == OP_FUNCLABEL && sym_name(f, rd32(code + p + 1)) == name) {
            *fn_sym = rd32(code + p + 1);
            uint32_t q = p + len;
            int n = 0;
            while (q < size && rd8(code + q) == 51 /* POPSYMBOL */) q += 5, n++;
            if (params) *params = n;
            return (int32_t)q;
        }
        p += len;
    }
    return -1;
}

// the first state-changing native function `name` calls (itself or through local calls, 4 deep), or ""
static std::string state_call(uint32_t f, const std::string &name, int depth = 0)
{
    uint32_t sym;
    int32_t at = body_start(f, name, &sym);
    if (at < 0 || depth > 4) return "";
    uint32_t code = rd32(f + 0x70), size = rd32(f + 0x74);
    for (uint32_t p = (uint32_t)at; p < size;) {
        uint8_t op = rd8(code + p);
        uint32_t len = 1 + 4 * (op < OP_COUNT ? g_nops[op] : 0);
        if (op == OP_FUNCLABEL) break;                  // the next function
        if (op == OP_CALLFUNC) {
            std::string n = sym_name(f, rd32(code + p + 1));
            for (const char *bad : k_state_natives)
                if (!_stricmp(n.c_str(), bad)) return n;
        } else if (op == OP_CALL && rd32(code + p + 1) == 1) {   // a local function: look inside
            uint32_t target = rd32(code + p + 5);
            if (target < size && rd8(code + target) == OP_FUNCLABEL) {
                std::string r = state_call(f, sym_name(f, rd32(code + target + 1)), depth + 1);
                if (!r.empty()) return r;
            }
        }
        p += len;
    }
    return "";
}

static bool call_init(uint32_t f)                       // guarded: InitData on a bad image must not take the game down
{
    __try {
        return ((InitDataFn)0x1C7260)(f, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool call_exec(uint32_t obj, const char *func)   // blocking execute, no arguments (as the game runs OnInit)
{
    uint32_t ret[2] = {0, 0};
    __try {
        ((ExecFn)0x186EF0)(obj, ret, func, 0);
        ((DogValDtorFn)0x1904E0)(ret, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// the first lifecycle function of the list the file has without parameters, or nullptr
static const char *lifecycle_func(uint32_t f, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t sym;
        int params = 0;
        if (body_start(f, names[i], &sym, &params) >= 0 && params == 0) return names[i];
    }
    return nullptr;
}

static uint32_t owner_of(uint32_t s)                    // the ScriptableObject running this Script (+0x1F8, its +8 = s)
{
    uint32_t o = rd32(s + 0x1F8);
    return o && rd32(o + 8) == s ? o : 0;
}

// instances currently in the middle of a call that involves file f (their position can't move to new code)
static void busy_scripts(uint32_t f, std::string &busy)
{
    int n = 0;
    for (uint32_t s = rd32(SCRIPT_LIST); s && n < 4000; s = rd32(s), n++) {
        int depth = rd16s(s + 0x1BE);
        if (!depth || !rd8(s + 0x1C0) || rd8(s + 0x1C6) || rd8(s + 0x1C4)) continue;
        bool uses = rd32(s + 0x80) == f;
        for (int i = 0; i < depth && i < 10; i++) uses |= rd32(s + 0xEC + i * 0x14 + 8) == f;
        if (!uses) continue;
        busy += busy.empty() ? "" : ", ";
        busy += base_name(file_name(rd32(s + 0x80))) + " " + sym_name(rd32(s + 0x80), cur_func(s));
    }
}

struct HandlerRow;
static std::vector<HandlerRow> read_handlers(int *native_count);
static int duplicate_handlers(const std::vector<uint32_t> &objs);

struct Inst {
    uint32_t s;
    bool running;
    std::string func, entry;
    TableView old;
};

// one file of the batch: new image in, tables rebuilt, instances moved over
static bool swap_file(SwapItem &it, std::vector<Inst> &insts, bool carry, std::string &msg)
{
    uint32_t f = it.file;
    TableView old_shared = file_tables(f, true);
    uint8_t saved[0x9C];
    rd(f, saved, sizeof saved);
    void *img = ((GameAllocFn)0x176BC0)(0x80000, (uint32_t)it.img.size());     // LoadFile's heap + tag
    if (!img) {
        msg = "out of game memory for the new script image";
        return false;
    }
    memcpy(img, it.img.data(), it.img.size());
    uint32_t p = (uint32_t)(uintptr_t)img, size = (uint32_t)it.img.size(), minus1 = 0xFFFFFFFF, zero = 0;
    wr(f + 0x38, &p, 4);
    for (uint32_t o = 0x18; o <= 0x34; o += 4) wr(f + o, &minus1, 4);  // TOC offsets
    for (uint32_t o = 0x3C; o <= 0x80; o += 4) wr(f + o, &zero, 4);    // tables, export table
    wr(f + 0x84, &p, 4);                                                // the stream InitData reads
    wr(f + 0x88, &zero, 4);
    wr(f + 0x8C, &size, 4);
    if (!call_init(f)) {
        wr(f, saved, sizeof saved);                     // the old image and tables are untouched: put them back
        msg = "the game rejected the new bytecode - the old script is still in place";
        return false;
    }
    uint32_t id = rd32(0x48323C), next = id + 1;        // a new file id: every cache keyed by (file, id) reloads
    wr(0x48323C, &next, 4);
    wr(f + 0xC, &id, 4);
    for (uint32_t i = 0, list = rd32(f + 0x78); list && i < rd32(f + 0x7C) && i < 64; i++) {   // files it calls into
        std::string dep = rd_str(list, 260);
        uint32_t d = ((LoadFileFn)0x1C7CC0)(dep.c_str(), 0);
        if (d) { uint32_t r = rd32(d + 0x14) + 1; wr(d + 0x14, &r, 4); }
        list += (uint32_t)dep.size() + 1;
    }
    if (carry) move_values(old_shared, file_tables(f, true), false);

    int restarted = 0, idle = 0, stopped = 0;
    for (auto &in : insts) {
        uint32_t s = in.s;
        ((ConnectFn)0x1C6700)(s, 0, f);                 // code stream + shared table (position 0)
        ((AllocSymFn)0x1843B0)(s, 0);                   // a fresh table from the new template
        TableView nv = file_tables(f, false);
        nv.tab = rd32(s + 0x84);
        move_values(in.old, nv, !carry);
        uint32_t e_sym, fn_sym;
        if (body_start(f, in.entry, &e_sym) >= 0) wr(s + 0x1D0, &e_sym, 4);
        int32_t at = carry && in.running ? body_start(f, in.func, &fn_sym) : -1;
        float wake = 0;
        int16_t empty = 0;
        uint8_t no = 0;
        int32_t none = -1;
        wr(s + 0x1BC, &empty, 2);                       // operand stack: values of the old code
        wr(s + 0x1D8, &wake, 4);
        wr(s + 0x1C2, &no, 1);
        if (at >= 0) {                                  // carry: restart the function it was in
            wr(s + 0x1E0, &at, 4);
            wr(s + 0x1CC, &fn_sym, 4);
            restarted++;
        } else if (in.running) {                        // reset (or its function is gone): stop
            wr(s + 0x1C0, &no, 1);
            wr(s + 0x1C4, &no, 1);
            wr(s + 0x1C6, &no, 1);
            wr(s + 0x1CC, &none, 4);
            stopped++;
        } else {
            wr(s + 0x1C4, &no, 1);                      // a script stopped by an error can run again
            idle++;
        }
    }
    char b[300];
    _snprintf(b, sizeof b - 1, "%s (%d idle, %d restarted, %d stopped)", base_name(file_name(f)).c_str(), idle,
              restarted, stopped);
    b[sizeof b - 1] = 0;
    msg = b;
    return true;
}

// the whole batch; false while it has to wait for scripts to come back to the top level
static bool do_swap(SwapReq &rq)
{
    std::string busy;
    std::vector<SwapItem *> items;
    for (auto &it : rq.items)
        if (rd32(it.file + 0xC) == it.id) items.push_back(&it);
    if (items.empty()) {
        strcpy(g_swap_msg, "the script file(s) were unloaded - nothing replaced");
        return true;
    }
    for (auto *it : items) busy_scripts(it->file, busy);
    if (!busy.empty()) {
        _snprintf(g_swap_msg, sizeof g_swap_msg - 1, "%s: waiting for script calls to return: %s", rq.what.c_str(),
                  busy.c_str());
        return false;
    }
    // instances per file (all at the top level now)
    std::vector<std::vector<Inst>> insts(items.size());
    for (size_t k = 0; k < items.size(); k++) {
        uint32_t f = items[k]->file;
        int n = 0;
        for (uint32_t s = rd32(SCRIPT_LIST); s && n < 4000; s = rd32(s), n++)
            if (rd32(s + 0x80) == f) {
                bool running = rd8(s + 0x1C0) && !rd8(s + 0x1C6) && !rd8(s + 0x1C4);
                insts[k].push_back({s, running, running ? sym_name(f, cur_func(s)) : "", sym_name(f, rd32(s + 0x1D0)), {}});
            }
    }
    int lifecycle_runs = 0, lifecycle_skipped = 0;
    std::string guarded;                                // lifecycle functions not run: they change the game state
    auto lifecycle = [&](uint32_t f, const char *func, std::vector<Inst> &list) {
        std::string bad = state_call(f, func);
        if (!bad.empty()) {
            guarded += (guarded.empty() ? "" : ", ") + base_name(file_name(f)) + " " + func + " (calls " + bad + ")";
            return;
        }
        for (auto &in : list) {
            uint32_t o = owner_of(in.s);
            if (o && call_exec(o, func)) lifecycle_runs++;
            else lifecycle_skipped++;
        }
    };
    if (rq.run_kill)                                    // shutdown on the old code and state
        for (size_t k = 0; k < items.size(); k++)
            if (const char *kill = lifecycle_func(items[k]->file, k_kill_funcs, 4)) lifecycle(items[k]->file, kill, insts[k]);
    for (size_t k = 0; k < items.size(); k++)           // the old tables, named by the old file data (kept)
        for (auto &in : insts[k]) {
            in.old = file_tables(items[k]->file, false);
            in.old.tab = rd32(in.s + 0x84);
            in.running = rd8(in.s + 0x1C0) && !rd8(in.s + 0x1C6) && !rd8(in.s + 0x1C4);
        }
    std::string done, failed;
    std::vector<bool> ok(items.size());
    for (size_t k = 0; k < items.size(); k++) {
        std::string m;
        ok[k] = swap_file(*items[k], insts[k], rq.carry, m);
        (ok[k] ? done : failed) += (ok[k] ? done : failed).empty() ? m : "; " + m;
    }
    if (rq.run_init)                                    // start-up on the new code, fresh state
        for (size_t k = 0; k < items.size(); k++)
            if (const char *init = ok[k] ? lifecycle_func(items[k]->file, k_init_funcs, 4) : nullptr)
                lifecycle(items[k]->file, init, insts[k]);
    std::string life;
    if (rq.run_kill || rq.run_init) {
        const char *what = rq.run_kill && rq.run_init ? "OnKill/OnInit" : rq.run_kill ? "OnKill" : "OnInit";
        if (lifecycle_runs) life = std::string(" | ") + what + " run";
        if (lifecycle_skipped) life += " | some instances have no owning object: skipped for them";
        if (!guarded.empty()) life += " | NOT run (they change the game state): " + guarded;
        if (life.empty()) life = std::string(" | no ") + what + " without parameters in these files";
    }
    std::vector<uint32_t> objs;                         // event handlers these scripts now have twice or more
    for (auto &list : insts)
        for (auto &in : list)
            if (uint32_t o = owner_of(in.s)) objs.push_back(o);
    if (int d = duplicate_handlers(objs))
        life += " | WARNING: " + std::to_string(d) + " event handler(s) of these scripts are now registered more than once "
                "(Event handlers panel)";
    _snprintf(g_swap_msg, sizeof g_swap_msg - 1, "%s: %s%s%s%s", rq.what.c_str(), done.empty() ? "nothing" : done.c_str(),
              failed.empty() ? "" : " | FAILED: ", failed.c_str(), life.c_str());
    g_swap_msg[sizeof g_swap_msg - 1] = 0;
    g_host->log("dogdbg: sandbox %s\n", g_swap_msg);
    return true;
}

static void process_swap()                              // game thread, between frames
{
    std::lock_guard<std::mutex> l(g_swap_lock);
    if (!g_swap_req) return;
    if (g_host->net_running()) {
        strcpy(g_swap_msg, "not in an online match");
    } else if (!do_swap(*g_swap_req)) {
        return;                                         // try again next frame
    }
    delete g_swap_req;
    g_swap_req = nullptr;
    InterlockedIncrement(&g_swap_done);
}

static void request_batch(SwapReq *rq)                  // UI / console: takes ownership
{
    std::lock_guard<std::mutex> l(g_swap_lock);
    delete g_swap_req;
    g_swap_req = rq;
    _snprintf(g_swap_msg, sizeof g_swap_msg - 1, "%s: at the next frame...", rq->what.c_str());
}

static void request_swap(uint32_t file, const std::vector<uint8_t> &img, const char *what, bool carry = true,
                         bool run_kill = false, bool run_init = false)
{
    SwapReq *rq = new SwapReq;
    rq->items.push_back({file, rd32(file + 0xC), img});
    rq->carry = carry, rq->run_kill = run_kill, rq->run_init = run_init, rq->what = what;
    request_batch(rq);
}

static std::vector<uint8_t> loaded_image(uint32_t f)  // the image a file has now (a reset reloads it unchanged)
{
    std::vector<uint8_t> img;
    uint32_t data = rd32(f + 0x84), size = rd32(f + 0x8C);
    if (data && size && size < (8 << 20)) {
        img.resize(size);
        if (!rd(data, img.data(), size)) img.clear();
    }
    return img;
}

static void process_dedup();

void dogdbg_frame()                                     // game thread, per frame
{
    if (!g_game_tid) g_game_tid = GetCurrentThreadId();
    bool want = g_want_on && !g_host->net_running();
    if (want != g_installed) install(want);
    process_swap();
    process_dedup();
}

bool dogdbg_stopped() { return g_stopped; }

void dogdbg_break_next()
{
    g_want_on = true;
    g_break_next = true;
    update_armed();
}

void dogdbg_command(int cmd)                            // also the game window's F5 / F10 / F11
{
    if (g_stopped) InterlockedExchange(&g_cmd, cmd);
}

// ------------------------------------------------------------------------------------------------ UI: file model
struct Ins {
    uint32_t off;
    uint8_t op, n;
    uint32_t a[3];
};
enum SrcState { SRC_NONE, SRC_OK, SRC_STALE };
struct Source {                                         // decompiled source of a file (devtools\scripts)
    SrcState state = SRC_NONE;
    bool tried = false;                                 // decompiled once this session already
    std::vector<std::string> lines;
    std::vector<int> line_of;                           // per instruction: its source line (-1 none)
    std::vector<std::vector<int>> ins_of_line;          // per source line: its instructions
};

struct FileModel {
    uint32_t ptr = 0, id = 0;
    std::string name;
    std::vector<uint8_t> code;
    std::vector<Ins> ins;
    std::map<uint32_t, std::string> funcs;              // FUNCLABEL offset -> name
    std::unordered_map<uint32_t, std::string> names;    // symbol index -> name (cache)
    std::vector<std::string> text;                      // one per instruction
    std::vector<DWORD> last_hit;                        // trace: when each instruction last ran (GetTickCount)
    std::vector<uint32_t> hits;                         // trace: how often it ran since the file was opened
    LONG trace_seen = 0;
    int latest = -1;                                    // trace: the instruction that ran last
    Source src;
    int index_of(uint32_t off) const
    {
        size_t lo = 0, hi = ins.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (ins[mid].off < off) lo = mid + 1;
            else hi = mid;
        }
        return lo < ins.size() && ins[lo].off == off ? (int)lo : -1;
    }
    std::string sym(uint32_t idx)
    {
        auto it = names.find(idx);
        if (it != names.end()) return it->second;
        return names[idx] = sym_name(ptr, idx);
    }
};

static std::string describe(FileModel &m, const Ins &i)
{
    char b[400];
    const char *name = i.op < OP_COUNT ? OP_NAMES[i.op] : "?";
    auto target = [&](uint32_t v) { return (int32_t)v < 0 ? (uint32_t)(m.code.size() + (int32_t)v) : v; };
    if (i.n >= 1 && sym_op(i.op)) _snprintf(b, sizeof b, "%-11s %s", name, m.sym(i.a[0]).c_str());
    else if (i.op == OP_PUSHINT) _snprintf(b, sizeof b, "%-11s %d", name, (int32_t)i.a[0]);
    else if (i.op == OP_PUSHFLOAT) { float f; memcpy(&f, &i.a[0], 4); _snprintf(b, sizeof b, "%-11s %g", name, f); }
    else if (i.op == OP_PUSHSTRING || i.op == OP_PRINT_STRING) {
        uint32_t base = rd32(m.ptr + 0x58), size = rd32(m.ptr + 0x5C);
        _snprintf(b, sizeof b, "%-11s \"%s\"", name, base && i.a[0] < size ? rd_str(base + i.a[0], 300).c_str() : "?");
    } else if (i.op == OP_PUSHVECTOR) {
        float f[3];
        memcpy(f, i.a, 12);
        _snprintf(b, sizeof b, "%-11s %g, %g, %g", name, f[0], f[1], f[2]);
    } else if (i.op == OP_CALLFUNC)
        _snprintf(b, sizeof b, "%-11s %s.%s  (%u args)", name, i.a[2] == GLOBAL_OBJ ? "global" : m.sym(i.a[2]).c_str(),
                  m.sym(i.a[0]).c_str(), i.a[1]);
    else if (i.op == OP_CALL) {
        if (i.a[0] == 1) {
            auto f = m.funcs.find(i.a[1]);
            _snprintf(b, sizeof b, "%-11s %s  (%u args)", name, f != m.funcs.end() ? f->second.c_str() : "?", i.a[2]);
        } else
            _snprintf(b, sizeof b, "%-11s %s  (%u args, other file)", name, m.sym(i.a[1]).c_str(), i.a[2]);
    } else if (i.op == OP_IF || i.op == OP_ELSEIF || i.op == OP_BRA_ENDIF || i.op == OP_GOENDWHILE || i.op == OP_ENDWHILE)
        _snprintf(b, sizeof b, "%-11s -> %04X", name, target(i.a[0]));
    else {
        int k = _snprintf(b, sizeof b, "%s", name);
        for (int j = 0; j < i.n && k > 0; j++) k += _snprintf(b + k, sizeof b - k, " %X", i.a[j]);
    }
    b[sizeof b - 1] = 0;
    return b;
}

static void load_source(FileModel &m);

static bool load_model(FileModel &m, uint32_t f)
{
    m = FileModel();
    m.ptr = f, m.id = rd32(f + 0xC), m.name = file_name(f);
    uint32_t code = rd32(f + 0x70), size = rd32(f + 0x74);
    if (!code || !size || size > (1 << 20)) return false;
    m.code.resize(size);
    if (!rd(code, m.code.data(), size)) return false;
    for (uint32_t p = 0; p < size;) {
        Ins i = {p, m.code[p], 0, {}};
        i.n = i.op < OP_COUNT ? g_nops[i.op] : 0;
        if (p + 1 + 4 * i.n > size) i.n = 0;
        memcpy(i.a, &m.code[p + 1], 4 * i.n);
        if (i.op == OP_FUNCLABEL && i.n) m.funcs[p] = m.sym(i.a[0]);
        m.ins.push_back(i);
        p += 1 + 4 * i.n;
    }
    for (auto &i : m.ins) m.text.push_back(describe(m, i));
    m.last_hit.assign(m.ins.size(), 0);
    m.hits.assign(m.ins.size(), 0);
    m.trace_seen = g_trace_n;
    load_source(m);
    return true;
}

// ------------------------------------------------------------------------------------------------ UI: lists
struct ScriptRow {
    uint32_t s, id, file, ip;
    std::string file_name, func, flags;
};
static std::vector<ScriptRow> read_scripts()
{
    std::vector<ScriptRow> out;
    int n = 0;
    for (uint32_t s = rd32(SCRIPT_LIST); s && n < 2000; s = rd32(s), n++) {
        ScriptRow r;
        r.s = s, r.id = rd32(s + 0x1B4), r.file = rd32(s + 0x80), r.ip = rd32(s + 0x1E0);
        r.file_name = base_name(file_name(r.file));
        int32_t fn = (int32_t)cur_func(s);
        r.func = fn >= 0 ? sym_name(r.file, fn) : "-";
        uint8_t fl[10];
        if (rd(s + 0x1C0, fl, 10)) {
            r.flags = fl[4] ? "error" : fl[6] ? "finished" : !fl[0] ? "idle" : fl[1] ? "paused" : fl[2] ? "sleeping" : "running";
            if (!fl[8]) r.flags += " (disabled)";
        }
        out.push_back(r);
    }
    return out;
}

struct FileRow {
    uint32_t f, id, refs;
    std::string name;
};
static std::vector<FileRow> read_files()
{
    std::vector<FileRow> out;
    for (int b = 0; b < 64; b++) {
        int n = 0;
        for (uint32_t node = rd32(FILE_HASH + b * 8); node && n < 1000; node = rd32(node), n++) {
            uint32_t f = rd32(node + 0x10);
            if (f) out.push_back({f, rd32(f + 0xC), rd32(f + 0x14), base_name(file_name(f))});
        }
    }
    std::sort(out.begin(), out.end(), [](const FileRow &a, const FileRow &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// ------------------------------------------------------------------------------------------------ UI: state
static FileModel g_model;
static uint32_t g_view_file;                            // ScriptFile* shown
static int g_scroll_to = -1;                            // instruction index to bring into view
static uint32_t g_sel_script, g_sel_id;                 // script whose variables are shown (not stopped)
static volatile bool g_focus_tab;

static void tools_show_scripts()
{
    g_focus_tab = true;
    tools_window_show();
}

static void view(uint32_t file, uint32_t ip)
{
    if (file != g_model.ptr || rd32(file + 0xC) != g_model.id) load_model(g_model, file);
    g_view_file = file;
    g_trace_file = file;                                // trace what is on screen
    update_armed();
    g_scroll_to = -1;
    for (size_t k = 0; k < g_model.ins.size(); k++)
        if (g_model.ins[k].off <= ip) g_scroll_to = (int)k;
}

static bool bp_has(const std::string &file, uint32_t ip)
{
    std::lock_guard<std::mutex> l(g_bp_lock);
    for (auto &b : g_bps)
        if (b.file == file && b.ip == ip) return true;
    return false;
}
static void bp_toggle(const std::string &file, uint32_t ip)
{
    {
        std::lock_guard<std::mutex> l(g_bp_lock);
        auto it = std::find_if(g_bps.begin(), g_bps.end(), [&](const Bp &b) { return b.file == file && b.ip == ip; });
        if (it != g_bps.end()) g_bps.erase(it);
        else g_bps.push_back({file, ip});
        g_bp_count = (LONG)g_bps.size();
    }
    InterlockedIncrement(&g_bp_gen);
    update_armed();
}

// ------------------------------------------------------------------------------------------------ UI: panels
static void toolbar()
{
    bool on = g_want_on;
    if (ImGui::Checkbox("Debugger on", &on)) {
        if (!on && g_stopped) InterlockedExchange(&g_cmd, DCMD_CONTINUE);
        g_want_on = on;
    }
    if (con_online()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "online: debugger off"); }
    ImGui::SameLine(0, 20);
    bool stopped = g_stopped;
    ImGui::BeginDisabled(!g_installed || stopped);
    if (ImGui::Button("Break")) { g_break_next = true; update_armed(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button("Continue (F5)") || (stopped && ImGui::IsKeyPressed(ImGuiKey_F5))) dogdbg_command(DCMD_CONTINUE);
    ImGui::SameLine();
    if (ImGui::Button("Step (F11)") || (stopped && ImGui::IsKeyPressed(ImGuiKey_F11) && !ImGui::GetIO().KeyShift))
        dogdbg_command(DCMD_STEP);
    ImGui::SameLine();
    if (ImGui::Button("Step over (F10)") || (stopped && ImGui::IsKeyPressed(ImGuiKey_F10))) dogdbg_command(DCMD_OVER);
    ImGui::SameLine();
    if (ImGui::Button("Step out (Shift+F11)") || (stopped && ImGui::IsKeyPressed(ImGuiKey_F11) && ImGui::GetIO().KeyShift))
        dogdbg_command(DCMD_OUT);
    ImGui::SameLine();
    if (ImGui::Button("Step instruction")) dogdbg_command(DCMD_STEP_INSTR);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 20);
    bool e = g_break_on_error, d = g_break_on_debugbreak;
    if (ImGui::Checkbox("Stop on script errors", &e)) g_break_on_error = e;
    ImGui::SameLine();
    if (ImGui::Checkbox("Stop on DebugBreak()", &d)) g_break_on_debugbreak = d;
    if (stopped) ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s  (game paused)", g_status);
    else if (g_installed && g_step.mode != STEP_NONE) ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "Stepping - waiting for the script to run on");
    else if (!g_installed) ImGui::TextDisabled("Switch the debugger on to set breakpoints and step. Script errors are always "
                                              "written to the Game log.");
    else ImGui::TextDisabled("%s", g_status[0] ? g_status : "Running. Click the margin of the code view to set a breakpoint.");
}

static void reset_panel(const std::vector<FileRow> &files);
static void handlers_panel();

static void left_lists()
{
    static std::vector<ScriptRow> scripts;
    static std::vector<FileRow> files;
    static DWORD last;
    if (g_stopped || GetTickCount() - last > 500) {
        scripts = read_scripts();
        files = read_files();
        last = GetTickCount();
    }
    if (ImGui::CollapsingHeader("Running scripts", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%d instances - click to show its code and variables", (int)scripts.size());
        if (ImGui::BeginTable("scripts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, 260))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("file");
            ImGui::TableSetupColumn("function");
            ImGui::TableSetupColumn("state");
            ImGui::TableHeadersRow();
            for (auto &r : scripts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID((int)r.s);
                bool sel = r.s == g_sel_script;
                if (ImGui::Selectable(r.file_name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                    g_sel_script = r.s, g_sel_id = r.id;
                    view(r.file, r.ip);
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.func.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.flags.c_str());
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Loaded script files")) {
        static char filter[64];
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##ff", "filter", filter, sizeof filter);
        ImGui::BeginChild("files", ImVec2(0, 220));
        for (auto &f : files) {
            if (*filter && !StrStrIA(f.name.c_str(), filter)) continue;
            ImGui::PushID((int)f.f);
            if (ImGui::Selectable(f.name.c_str(), f.f == g_view_file)) view(f.f, 0);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (ImGui::CollapsingHeader("Reset scripts")) reset_panel(files);
    if (ImGui::CollapsingHeader("Event handlers")) handlers_panel();
    if (ImGui::CollapsingHeader("Breakpoints", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::vector<Bp> bps;
        {
            std::lock_guard<std::mutex> l(g_bp_lock);
            bps = g_bps;
        }
        if (bps.empty()) ImGui::TextDisabled("none - click left of an instruction");
        for (auto &b : bps) {
            ImGui::PushID((b.file + std::to_string(b.ip)).c_str());
            if (ImGui::SmallButton("x")) bp_toggle(b.file, b.ip);
            ImGui::SameLine();
            ImGui::Text("%s @%04X", b.file.c_str(), b.ip);
            ImGui::PopID();
        }
        if (!bps.empty() && ImGui::SmallButton("Remove all")) {
            {
                std::lock_guard<std::mutex> l(g_bp_lock);
                g_bps.clear();
                g_bp_count = 0;
            }
            InterlockedIncrement(&g_bp_gen);
            update_armed();
        }
    }
}

// ------------------------------------------------------------------------------------------------ UI: decompiled source
// Source comes from the Dog decompiler in devtools\dog next to gladius.exe (src\devtools\dog; `python -m dog <file> --map`):
// <name>.scp (.moo for moods) and <name>.map (source line -> code offsets) in devtools\scripts. The map's header
// carries the code size and CRC, so a modded or changed script never shows the wrong source. A file without a matching
// pair is decompiled on demand from the copy the game has in memory. GladiusPort Builder's developer tools option
// (debug builds) installs the decompiler and writes the Python it found to [devtools] Python=.
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFF;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

static std::string scripts_dir()                        // <exe dir>\devtools\scripts\ (created)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "devtools");
    CreateDirectoryA(path, nullptr);
    strcat(path, "\\scripts");
    CreateDirectoryA(path, nullptr);
    return std::string(path) + "\\";
}

static std::string stem_of(const std::string &name)     // "d:\...\Bin\X.scb" -> "X"
{
    std::string b = base_name(name);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}
static bool is_mood(const std::string &name)
{
    size_t dot = name.find_last_of('.');
    return dot != std::string::npos && !_stricmp(name.c_str() + dot, ".boo");
}

static void publish_lines(FileModel &m);

static void load_source(FileModel &m)
{
    Source &src = m.src;
    src = Source();
    std::string dir = scripts_dir(), stem = stem_of(m.name);
    FILE *f = fopen((dir + stem + ".map").c_str(), "rb");
    if (!f) { src.state = SRC_NONE; return; }
    char line[8192];
    unsigned size = 0, crc = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "dogmap 1 %u %x", &size, &crc) != 2) {
        fclose(f);
        src.state = SRC_NONE;
        return;
    }
    if (size != m.code.size() || crc != crc32_of(m.code.data(), m.code.size())) {
        fclose(f);
        src.state = SRC_STALE;                          // a different build of this script (mod, edit)
        return;
    }
    src.line_of.assign(m.ins.size(), -1);
    while (fgets(line, sizeof line, f)) {
        char *p = line, *e;
        long n = strtol(p, &e, 10);
        if (e == p || n <= 0) continue;
        for (p = e;;) {
            unsigned long off = strtoul(p, &e, 16);
            if (e == p) break;
            p = e;
            int k = m.index_of((uint32_t)off);
            if (k >= 0) src.line_of[k] = (int)n - 1;
        }
    }
    fclose(f);
    FILE *s = fopen((dir + stem + (is_mood(m.name) ? ".moo" : ".scp")).c_str(), "rb");
    if (!s) { src.state = SRC_NONE; return; }
    while (fgets(line, sizeof line, s)) {
        std::string t;
        for (char *c = line; *c && *c != '\r' && *c != '\n'; c++)
            if (*c == '\t') t += "    ";
            else t += *c;
        src.lines.push_back(t);
    }
    fclose(s);
    src.ins_of_line.assign(src.lines.size(), {});
    for (size_t k = 0; k < src.line_of.size(); k++)
        if (src.line_of[k] >= 0 && src.line_of[k] < (int)src.lines.size()) src.ins_of_line[src.line_of[k]].push_back((int)k);
    src.state = SRC_OK;
    publish_lines(m);
}

// the game thread's copy for stepping by line: per code byte, its source line and whether a line run starts there
static void publish_lines(FileModel &m)
{
    auto lt = std::make_shared<LineTable>();
    lt->id = m.id;
    lt->line.assign(m.code.size() + 1, -1);
    lt->start.assign(m.code.size() + 1, 0);
    for (size_t k = 0; k < m.ins.size(); k++) {
        int ln = m.src.line_of[k];
        uint32_t a = m.ins[k].off, b = a + 1 + 4 * m.ins[k].n;
        for (uint32_t o = a; o < b && o < m.code.size(); o++) lt->line[o] = ln;
        lt->start[a] = ln >= 0 && (k == 0 || m.src.line_of[k - 1] != ln);
    }
    std::lock_guard<std::mutex> l(g_lt_lock);
    g_line_tables[m.ptr] = lt;
}

// decompiling a file the game has loaded, on a worker thread
static volatile LONG g_dec_busy, g_dec_done;
static char g_dec_msg[400];

struct DecJob {
    std::string cmd, cwd, log;
};
static DWORD WINAPI decompile_thread(void *p)
{
    DecJob *j = (DecJob *)p;
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileA(j->log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(j->cmd.begin(), j->cmd.end());
    cmd.push_back(0);
    DWORD code = 1;
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, j->cwd.c_str(), &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        _snprintf(g_dec_msg, sizeof g_dec_msg - 1, code ? "decompiler failed (exit %u) - see %s" : "decompiled",
                  code, j->log.c_str());
    } else
        _snprintf(g_dec_msg, sizeof g_dec_msg - 1, "could not start Python (%s) - install Python 3, then run GladiusPort "
                  "Builder's developer tools option again (it records where Python is)", j->cmd.c_str());
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    delete j;
    InterlockedIncrement(&g_dec_done);
    InterlockedExchange(&g_dec_busy, 0);
    return 0;
}

static bool decompiler_configured(std::string *root, std::string *python)
{
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char *slash = strrchr(buf, '\\');
    strcpy(slash ? slash + 1 : buf, "devtools");        // runs as `python -m dog` from here
    *root = buf;
    GetPrivateProfileStringA("devtools", "Python", "python", buf, sizeof buf, g_host->ini);
    *python = buf;
    return GetFileAttributesA((*root + "\\dog\\decompile.py").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void start_decompile(FileModel &m)
{
    std::string root, python;
    if (!decompiler_configured(&root, &python)) {
        strcpy(g_dec_msg, "the decompiler (devtools\\dog) is not installed - install with GladiusPort Builder's developer tools option");
        return;
    }
    if (InterlockedCompareExchange(&g_dec_busy, 1, 0)) return;
    uint32_t data = rd32(m.ptr + 0x84), size = rd32(m.ptr + 0x8C);   // the file image the game loaded
    char sig[4] = {};
    if (!data || !size || size > (8 << 20) || !rd(data, sig, 4) || memcmp(sig, "SGOD", 4)) {
        strcpy(g_dec_msg, "the loaded file image is not available");
        g_dec_busy = 0;
        return;
    }
    std::vector<uint8_t> img(size);
    if (!rd(data, img.data(), size)) {
        strcpy(g_dec_msg, "could not read the loaded file image");
        g_dec_busy = 0;
        return;
    }
    std::string dir = scripts_dir(), bin = dir + stem_of(m.name) + (is_mood(m.name) ? ".boo" : ".scb");
    FILE *f = fopen(bin.c_str(), "wb");
    if (!f) {
        _snprintf(g_dec_msg, sizeof g_dec_msg - 1, "cannot write %s", bin.c_str());
        g_dec_busy = 0;
        return;
    }
    fwrite(img.data(), 1, size, f);
    fclose(f);
    DecJob *j = new DecJob;
    j->cmd = "\"" + python + "\" -m dog \"" + bin + "\" -o \"" + dir.substr(0, dir.size() - 1) + "\" --map";
    j->cwd = root;
    j->log = dir + "decompile.log";
    strcpy(g_dec_msg, "decompiling...");
    CloseHandle(CreateThread(nullptr, 0, decompile_thread, j, 0, nullptr));
}

// ------------------------------------------------------------------------------------------------ UI: code view
static bool g_show_source = true;

static void draw_source(FileModel &m, DWORD now, bool here, int cur_k, int sel_k, bool follow)
{
    Source &src = m.src;
    float lh = ImGui::GetTextLineHeightWithSpacing();
    int cur_line = cur_k >= 0 ? src.line_of[cur_k] : -1, sel_line = sel_k >= 0 ? src.line_of[sel_k] : -1;
    int latest_line = m.latest >= 0 ? src.line_of[m.latest] : -1;
    static int followed = -1;
    if (follow && !g_stopped && g_installed && latest_line >= 0 && latest_line != followed) {
        float y = latest_line * lh, top = ImGui::GetScrollY(), h = ImGui::GetWindowHeight();
        if (y < top || y > top + h - lh * 2) ImGui::SetScrollY(y - h * 0.4f);
        followed = latest_line;
    }
    if (g_scroll_to >= 0) {
        int ln = g_scroll_to < (int)src.line_of.size() ? src.line_of[g_scroll_to] : -1;
        if (ln >= 0) ImGui::SetScrollY(ln * lh - ImGui::GetWindowHeight() * 0.4f);
        g_scroll_to = -1;
    }
    ImGuiListClipper clip;
    clip.Begin((int)src.lines.size(), lh);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    while (clip.Step())
        for (int ln = clip.DisplayStart; ln < clip.DisplayEnd; ln++) {
            const std::vector<int> &ks = src.ins_of_line[ln];
            ImGui::PushID(ln);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool bp = false;
            DWORD last = 0;
            uint32_t hits = 0;
            for (int k : ks) {
                bp |= bp_has(m.name, m.ins[k].off);
                if (m.last_hit[k] && (!last || (LONG)(m.last_hit[k] - last) > 0)) last = m.last_hit[k];
                hits = m.hits[k] > hits ? m.hits[k] : hits;
            }
            if (ImGui::InvisibleButton("bp", ImVec2(18, lh)) && !ks.empty()) {
                // a breakpoint at the start of each run of the line's code (a 'for' header has three)
                for (size_t q = 0; q < ks.size(); q++) {
                    uint32_t off = m.ins[ks[q]].off;
                    bool has = bp_has(m.name, off);
                    if (bp ? has : !has && (q == 0 || ks[q - 1] != ks[q] - 1)) bp_toggle(m.name, off);
                }
            }
            if (bp) dl->AddCircleFilled(ImVec2(p.x + 9, p.y + lh * 0.5f), 5, IM_COL32(220, 60, 60, 255));
            ImGui::SameLine(0, 2);
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImVec2 b(a.x + ImGui::GetContentRegionAvail().x + 2000, a.y + lh);
            DWORD age = last ? now - last : 0xFFFFFFFF;
            bool is_cur = here && ln == cur_line;
            bool is_sel = ln == sel_line || (g_installed && !here && ln == latest_line);
            if (is_cur || is_sel)
                dl->AddRectFilled(a, b, is_cur ? IM_COL32(110, 90, 20, 255) : IM_COL32(40, 70, 120, 255));
            else if (age < 1500)
                dl->AddRectFilled(a, b, IM_COL32(40, 110, 60, (int)(30 + 150 * (1.0f - age / 1500.0f))));
            const std::string &t = src.lines[ln];
            size_t lead = t.find_first_not_of(' ');
            const char *body = t.c_str() + (lead == std::string::npos ? t.size() : lead);
            ImVec4 col = ImVec4(0.88f, 0.88f, 0.88f, 1);
            if (!strncmp(body, "//", 2)) col = ImVec4(0.5f, 0.6f, 0.5f, 1);
            else if (!strncmp(body, "function ", 9) || !strncmp(body, "endfunction", 11)) col = ImVec4(0.55f, 0.85f, 1, 1);
            ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.5f, 1), "%4d", ln + 1);
            ImGui::SameLine();
            ImGui::TextColored(is_cur ? ImVec4(1, 1, 1, 1) : col, "%s%s", is_cur ? (g_stop.after ? "=>" : "->") : "  ",
                               t.c_str());
            if (hits) {
                ImGui::SameLine(0, 16);
                ImGui::TextDisabled("x%u", hits);
            }
            ImGui::PopID();
        }
}

static void draw_disasm(FileModel &m, DWORD now, bool here, int cur_k, int sel_k, bool follow)
{
    float lh = ImGui::GetTextLineHeightWithSpacing();
    static int followed = -1;
    if (follow && !g_stopped && g_installed && m.latest >= 0 && m.latest != followed) {
        float y = m.latest * lh, top = ImGui::GetScrollY(), h = ImGui::GetWindowHeight();
        if (y < top || y > top + h - lh * 2) g_scroll_to = m.latest;   // only when it leaves the view
        followed = m.latest;
    }
    if (g_scroll_to >= 0) {
        ImGui::SetScrollY(g_scroll_to * lh - ImGui::GetWindowHeight() * 0.4f);
        g_scroll_to = -1;
    }
    ImGuiListClipper clip;
    clip.Begin((int)m.ins.size(), lh);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    while (clip.Step())
        for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
            const Ins &i = m.ins[k];
            ImGui::PushID(k);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("bp", ImVec2(18, lh))) bp_toggle(m.name, i.off);
            if (bp_has(m.name, i.off)) dl->AddCircleFilled(ImVec2(p.x + 9, p.y + lh * 0.5f), 5, IM_COL32(220, 60, 60, 255));
            ImGui::SameLine(0, 2);
            bool is_cur = here && k == cur_k;
            bool is_sel = k == sel_k || (g_installed && !here && k == m.latest);
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImVec2 b(a.x + ImGui::GetContentRegionAvail().x + 2000, a.y + lh);
            DWORD age = m.last_hit[k] ? now - m.last_hit[k] : 0xFFFFFFFF;
            if (is_cur || is_sel)
                dl->AddRectFilled(a, b, is_cur ? IM_COL32(110, 90, 20, 255) : IM_COL32(40, 70, 120, 255));
            else if (age < 1500)                        // ran recently: a trail that fades over 1.5 s
                dl->AddRectFilled(a, b, IM_COL32(40, 110, 60, (int)(30 + 150 * (1.0f - age / 1500.0f))));
            if (i.op == OP_FUNCLABEL)
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1, 1), "%04X  function %s", i.off, m.text[k].c_str() + 12);
            else
                ImGui::Text("%04X  %s%s", i.off, is_cur ? (g_stop.after ? "=> " : "-> ") : "   ", m.text[k].c_str());
            if (m.hits[k]) {                            // how often it ran
                ImGui::SameLine(0, 16);
                ImGui::TextDisabled("x%u", m.hits[k]);
            }
            ImGui::PopID();
        }
}

// ------------------------------------------------------------------------------------------------ UI: event handlers
// The game's event handler list (head 0x38E7AC, tail 0x38E7B0; RegisterEventHandler 0x2A930 -> node ctor 0x22C50,
// appended with no duplicate check): node +0 vtable, +4 next, +8 prev, +0xC event name (BString*), +0x10 owning
// ScriptableObject (its +8 = the Script), +0x14 function name (BString*), +0x24 = 0 for a script handler. A duplicate
// runs its function twice per event (a counter like ImperiaTownsToBeat drops by 2). UnregisterEventHandler (0x22900)
// removes every match, so OnKill clears duplicates too.
struct HandlerRow {
    std::string event, func, script;
    uint32_t obj;
    int count;
};
static std::vector<HandlerRow> read_handlers(int *native_count)
{
    std::vector<HandlerRow> rows;
    int natives = 0, n = 0;
    for (uint32_t node = rd32(0x38E7AC); node && n < 20000; node = rd32(node + 4), n++) {
        if (rd32(node + 0x24)) { natives++; continue; }
        uint32_t ev = rd32(node + 0xC), fn = rd32(node + 0x14), obj = rd32(node + 0x10);
        HandlerRow r{ev ? rd_str(ev + 0xC, 120) : "?", fn ? rd_str(fn + 0xC, 120) : "?", "?", obj, 1};
        uint32_t s = obj ? rd32(obj + 8) : 0;
        if (s) r.script = base_name(file_name(rd32(s + 0x80)));
        bool dup = false;
        for (auto &x : rows)
            if (x.obj == r.obj && x.event == r.event && !_stricmp(x.func.c_str(), r.func.c_str())) { x.count++; dup = true; break; }
        if (!dup) rows.push_back(r);
    }
    if (native_count) *native_count = natives;
    return rows;
}

// Remove duplicates: every script handler entry after the first with the same event, owner and function is unlinked
// and deleted the way 0x22900 does (vtable[0](1)). Game thread between frames - no event is being dispatched then;
// never while stopped at a break (the dispatch loop may be up the stack).
struct DedupReq {
    uint32_t obj;                                       // 0 = all of them
    std::string event, func;
};
static std::mutex g_dedup_lock;
static std::unique_ptr<DedupReq> g_dedup_req;
static char g_dedup_msg[200];
typedef void(__fastcall *NodeDeleteFn)(uint32_t self, uint32_t edx, int flags);

static void delete_node(uint32_t node)
{
    __try {
        ((NodeDeleteFn)rd32(rd32(node)))(node, 0, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void process_dedup()
{
    std::unique_ptr<DedupReq> rq;
    {
        std::lock_guard<std::mutex> l(g_dedup_lock);
        rq.swap(g_dedup_req);
    }
    if (!rq) return;
    if (g_host->net_running()) { strcpy(g_dedup_msg, "not in an online match"); return; }
    struct Key { uint32_t obj; std::string ev, fn; };
    std::vector<Key> kept;
    int removed = 0, n = 0;
    for (uint32_t node = rd32(0x38E7AC), next; node && n < 20000; node = next, n++) {
        next = rd32(node + 4);
        if (rd32(node + 0x24)) continue;
        uint32_t ev = rd32(node + 0xC), fn = rd32(node + 0x14), obj = rd32(node + 0x10);
        Key k{obj, ev ? rd_str(ev + 0xC, 120) : "?", fn ? rd_str(fn + 0xC, 120) : "?"};
        if (rq->obj && (obj != rq->obj || _stricmp(k.ev.c_str(), rq->event.c_str()) || _stricmp(k.fn.c_str(), rq->func.c_str())))
            continue;
        bool dup = false;
        for (auto &x : kept)
            dup |= x.obj == k.obj && !_stricmp(x.ev.c_str(), k.ev.c_str()) && !_stricmp(x.fn.c_str(), k.fn.c_str());
        if (!dup) { kept.push_back(k); continue; }
        uint32_t prev = rd32(node + 8);
        if (prev) wr(prev + 4, &next, 4);
        else wr(0x38E7AC, &next, 4);
        if (next) wr(next + 8, &prev, 4);
        else wr(0x38E7B0, &prev, 4);
        delete_node(node);
        removed++;
    }
    _snprintf(g_dedup_msg, sizeof g_dedup_msg - 1, "removed %d duplicate handler entr%s", removed, removed == 1 ? "y" : "ies");
    g_host->log("dogdbg: %s\n", g_dedup_msg);
}

static void request_dedup(uint32_t obj, const std::string &event, const std::string &func)
{
    std::lock_guard<std::mutex> l(g_dedup_lock);
    g_dedup_req.reset(new DedupReq{obj, event, func});
    strcpy(g_dedup_msg, "removing at the next frame...");
}

void dogdbg_dedup_all() { request_dedup(0, "", ""); }

int dogdbg_handler_count(const std::string &file, const std::string &func)
{
    int c = 0, n = 0;
    for (uint32_t node = rd32(0x38E7AC); node && n < 20000; node = rd32(node + 4), n++) {
        if (rd32(node + 0x24)) continue;
        uint32_t fn = rd32(node + 0x14), obj = rd32(node + 0x10), s = obj ? rd32(obj + 8) : 0;
        if (!fn || !s || _stricmp(rd_str(fn + 0xC, 120).c_str(), func.c_str())) continue;
        std::string name = base_name(file_name(rd32(s + 0x80)));
        size_t d = name.rfind('.');
        if (!_stricmp((d == std::string::npos ? name : name.substr(0, d)).c_str(), file.c_str())) c++;
    }
    return c;
}

static int duplicate_handlers(const std::vector<uint32_t> &objs)
{
    int d = 0;
    for (auto &r : read_handlers(nullptr))
        if (r.count > 1 && std::find(objs.begin(), objs.end(), r.obj) != objs.end()) d++;
    return d;
}

static void handlers_panel()
{
    static std::vector<HandlerRow> rows;
    static std::vector<std::string> effects;            // what each row's function does to the game variables
    static int natives;
    static DWORD last;
    if (g_stopped || GetTickCount() - last > 500) {
        rows = read_handlers(&natives);
        effects.clear();
        for (auto &r : rows) effects.push_back(flow_effects(r.script, r.func, false));
        last = GetTickCount();
    }
    static bool only_sel, only_dup;
    static char filter[64];
    int dups = 0;
    for (auto &r : rows) dups += r.count > 1;
    ImGui::TextDisabled("%d script handlers (+%d engine ones)%s", (int)rows.size(), natives, dups ? "" : ", no duplicates");
    if (dups) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%d registered more than once", dups);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_stopped || con_online());
        if (ImGui::SmallButton("Remove duplicates")) request_dedup(0, "", "");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(g_stopped ? "continue the game first" : "keep one entry of each - the function then runs once per event again");
    }
    if (*g_dedup_msg) ImGui::TextDisabled("%s", g_dedup_msg);
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##hf", "filter", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::Checkbox("selected script", &only_sel);
    ImGui::SameLine();
    ImGui::Checkbox("duplicates", &only_dup);
    std::string sel = g_model.ptr ? base_name(g_model.name) : "";
    if (ImGui::BeginTable("handlers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, 220))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("event");
        ImGui::TableSetupColumn("function");
        ImGui::TableSetupColumn("script");
        ImGui::TableSetupColumn("changes");
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < rows.size(); k++) {
            const HandlerRow &r = rows[k];
            if (only_sel && _stricmp(r.script.c_str(), sel.c_str())) continue;
            if (only_dup && r.count < 2) continue;
            if (*filter && !StrStrIA(r.event.c_str(), filter) && !StrStrIA(r.func.c_str(), filter) &&
                !StrStrIA(r.script.c_str(), filter))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.event.c_str());
            ImGui::TableNextColumn();
            if (r.count > 1) {
                ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s  x%d", r.func.c_str(), r.count);
                ImGui::SameLine();
                ImGui::PushID((int)k);
                ImGui::BeginDisabled(g_stopped || con_online());
                if (ImGui::SmallButton("remove extra")) request_dedup(r.obj, r.event, r.func);
                ImGui::EndDisabled();
                ImGui::PopID();
            } else
                ImGui::TextUnformatted(r.func.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.script.c_str());
            ImGui::TableNextColumn();
            static const std::string none;
            const std::string &e = k < effects.size() ? effects[k] : none;
            bool counts = e.find(" +") != std::string::npos || e.find(" -") != std::string::npos;
            if (counts && r.count > 1) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s (x%d per event)", e.c_str(), r.count);
            else if (counts) ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%s", e.c_str());
            else ImGui::TextDisabled("%s", e.c_str());
            if (!e.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("game variables this handler changes (Quests tab)");
        }
        ImGui::EndTable();
    }
}

// what a lifecycle function does that a reset would repeat: native calls counted in it and the local functions it
// calls (lower-case native name -> count)
static void count_calls(uint32_t f, const std::string &name, std::unordered_map<std::string, int> &out, int depth = 0)
{
    uint32_t sym;
    int32_t at = body_start(f, name, &sym);
    if (at < 0 || depth > 4) return;
    uint32_t code = rd32(f + 0x70), size = rd32(f + 0x74);
    for (uint32_t p = (uint32_t)at; p < size;) {
        uint8_t op = rd8(code + p);
        uint32_t len = 1 + 4 * (op < OP_COUNT ? g_nops[op] : 0);
        if (op == OP_FUNCLABEL) break;
        if (op == OP_CALLFUNC) {
            std::string n = sym_name(f, rd32(code + p + 1));
            for (auto &c : n) c = (char)tolower((unsigned char)c);
            out[n]++;
        } else if (op == OP_CALL && rd32(code + p + 1) == 1) {
            uint32_t target = rd32(code + p + 5);
            if (target < size && rd8(code + target) == OP_FUNCLABEL)
                count_calls(f, sym_name(f, rd32(code + target + 1)), out, depth + 1);
        }
        p += len;
    }
}

// warnings for resetting file f with the current options ("" = none)
static std::string reset_warning(uint32_t f, bool run_kill, bool run_init)
{
    const char *init = run_init ? lifecycle_func(f, k_init_funcs, 4) : nullptr;
    if (!init || !state_call(f, init).empty()) return "";
    std::unordered_map<std::string, int> in, kill;
    count_calls(f, init, in);
    const char *kf = run_kill ? lifecycle_func(f, k_kill_funcs, 4) : nullptr;
    bool kill_runs = kf && state_call(f, kf).empty();
    if (kill_runs) count_calls(f, kf, kill);
    std::string w;
    int reg = in["registereventhandler"];
    int unreg = kill["unregistereventhandler"];
    if (reg > unreg) {
        w += std::string(init) + " registers " + std::to_string(reg) + " event handler(s)" +
             (kill_runs ? " and OnKill removes only " + std::to_string(unreg) : " and no OnKill runs first") +
             ": they will be registered again (duplicates - Remove duplicates in Event handlers). ";
        std::string c = flow_counting_handlers(file_name(f));
        if (!c.empty()) w += "Counting handlers would count twice: " + c + ". ";
    }
    int summons = in["summonentity"] + in["summonschoolentity"] + in["createentity"] + in["spawnentity"];
    int paths = in["addwaypointfromprop"] + in["addwaypoint"];
    if (summons) w += std::string(init) + " summons " + std::to_string(summons) + " character(s) again (the old ones stay). ";
    if (paths && !summons) w += std::string(init) + " gives characters their waypoint paths again. ";
    return w;
}

// ------------------------------------------------------------------------------------------------ UI: reset scripts
// Tick files, reset them together (fresh variables, running instances stopped, optionally OnKill / OnInit again). The
// panel suggests linked files - what the ticked ones call into, what calls into them, scripts mid-call into them - but
// only ticked files are touched.
static bool g_reset_init = true;                        // a reset runs OnInit (start-up) again
static bool g_reset_kill;                               // ... and OnKill (shutdown) first: off, teardown moves the game on

static std::string stem_lower(const std::string &name)
{
    std::string s = stem_of(base_name(name));
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// files this one calls into: its FILE list and the file part of its external symbols ("Func@file")
static std::vector<std::string> file_deps(uint32_t f)
{
    std::vector<std::string> out;
    auto add = [&](const std::string &n) {
        std::string s = stem_lower(n);
        if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    for (uint32_t i = 0, list = rd32(f + 0x78); list && i < rd32(f + 0x7C) && i < 64; i++) {
        std::string dep = rd_str(list, 260);
        add(dep);
        list += (uint32_t)dep.size() + 1;
    }
    uint32_t tab = rd32(f + 0x40), n = rd32(f + 0x48);
    for (uint32_t i = 0; tab && i < n && i < 20000; i++)
        if (rd32(tab + i * 12 + 4) & 0x20) {
            std::string s = sstr(f, rd32(tab + i * 12));
            size_t at = s.find('@');
            if (at != std::string::npos) add(s.substr(at + 1));
        }
    return out;
}

static void reset_panel(const std::vector<FileRow> &files)
{
    static std::vector<std::string> picked;             // stems
    static char filter[64];
    auto is_picked = [&](const std::string &s) { return std::find(picked.begin(), picked.end(), s) != picked.end(); };
    auto toggle = [&](const std::string &s) {
        auto it = std::find(picked.begin(), picked.end(), s);
        if (it == picked.end()) picked.push_back(s);
        else picked.erase(it);
    };
    ImGui::TextDisabled("Tick scripts to reset together; unticked ones keep running.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##rf", "filter", filter, sizeof filter);
    ImGui::BeginChild("rfiles", ImVec2(0, 160), ImGuiChildFlags_Borders);
    for (auto &f : files) {
        if (*filter && !StrStrIA(f.name.c_str(), filter)) continue;
        std::string s = stem_lower(f.name);
        bool on = is_picked(s);
        ImGui::PushID((int)f.f);
        if (ImGui::Checkbox(f.name.c_str(), &on)) toggle(s);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (picked.empty()) return;

    // links of the ticked files (re-read twice a second)
    struct Link {
        std::string stem, why;
    };
    static std::vector<Link> links;
    static DWORD last;
    static size_t last_n;
    if (GetTickCount() - last > 500 || last_n != picked.size()) {
        last = GetTickCount(), last_n = picked.size();
        links.clear();
        auto add = [&](const std::string &s, const std::string &why) {
            if (is_picked(s)) return;
            for (auto &l : links)
                if (l.stem == s) { if (l.why.find(why) == std::string::npos) l.why += ", " + why; return; }
            links.push_back({s, why});
        };
        std::unordered_map<std::string, uint32_t> by_stem;
        for (auto &f : files) by_stem[stem_lower(f.name)] = f.f;
        for (auto &p : picked) {
            auto it = by_stem.find(p);
            if (it == by_stem.end()) continue;
            for (auto &d : file_deps(it->second)) add(d, p + " calls it");
            for (auto &f : files) {
                auto deps = file_deps(f.f);
                if (std::find(deps.begin(), deps.end(), p) != deps.end()) add(stem_lower(f.name), "calls " + p);
            }
            int n = 0;                                  // scripts of other files in the middle of a call into it
            for (uint32_t s = rd32(SCRIPT_LIST); s && n < 4000; s = rd32(s), n++) {
                int depth = rd16s(s + 0x1BE);
                if (depth <= 0 || rd32(s + 0x80) != it->second) continue;
                uint32_t home = rd32(s + 0xEC + 8);     // the file the outermost frame returns to
                if (home && home != it->second) add(stem_lower(file_name(home)), "running " + p + " code now");
            }
        }
    }
    ImGui::Text("Reset %d: ", (int)picked.size());
    for (size_t i = 0; i < picked.size(); i++) {
        ImGui::SameLine();
        ImGui::PushID((int)i);
        if (ImGui::SmallButton((picked[i] + " x").c_str())) { picked.erase(picked.begin() + i); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (!links.empty()) {
        ImGui::TextDisabled("Linked (not ticked):");
        for (auto &l : links) {
            ImGui::PushID(l.stem.c_str());
            if (ImGui::SmallButton("+")) toggle(l.stem);
            ImGui::SameLine();
            ImGui::Text("%s", l.stem.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", l.why.c_str());
            ImGui::PopID();
        }
    }
    for (auto &f : files)                                // what this reset would repeat
        if (is_picked(stem_lower(f.name))) {
            std::string w = reset_warning(f.f, g_reset_kill, g_reset_init);
            if (!w.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s: %s", f.name.c_str(), w.c_str());
        }
    ImGui::Checkbox("Run OnInit after (start-up)", &g_reset_init);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Start-up again on the fresh variables - only where it takes no parameters (unit / class scripts "
                          "get theirs from the game) and does not change the game state. A world map scene's OnInit "
                          "summons its characters again.");
    ImGui::SameLine();
    ImGui::Checkbox("Run OnKill first (shutdown)", &g_reset_kill);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Teardown on the old state first (unregisters event handlers ...). Skipped where it changes the "
                          "game state: 50 retail OnKills call TriggerNewState, which ends the scene.");
    ImGui::BeginDisabled(con_online());
    if (ImGui::Button("Reset selected")) {
        SwapReq *rq = new SwapReq;
        rq->carry = false, rq->run_kill = g_reset_kill, rq->run_init = g_reset_init, rq->what = "reset";
        for (auto &f : files)
            if (is_picked(stem_lower(f.name))) {
                std::vector<uint8_t> img = loaded_image(f.f);
                if (!img.empty()) rq->items.push_back({f.f, rd32(f.f + 0xC), img});
            }
        if (rq->items.empty()) delete rq;
        else request_batch(rq);
    }
    ImGui::EndDisabled();
    std::lock_guard<std::mutex> l(g_swap_lock);
    if (g_swap_req) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s", g_swap_msg);
        if (ImGui::SmallButton("Cancel##reset")) {
            delete g_swap_req;
            g_swap_req = nullptr;
            strcpy(g_swap_msg, "cancelled");
        }
    } else if (g_swap_msg[0])
        ImGui::TextWrapped("%s", g_swap_msg);
}

// console 'scriptreset <file> [<file> ...] [-nolife]'
std::string dogdbg_reset_files(const std::vector<std::string> &names, bool run_kill, bool run_init)
{
    SwapReq *rq = new SwapReq;
    rq->carry = false, rq->run_kill = run_kill, rq->run_init = run_init, rq->what = "scriptreset";
    std::string missing;
    for (auto &n : names) {
        uint32_t f = 0;
        for (int b = 0; b < 64 && !f; b++)
            for (uint32_t node = rd32(FILE_HASH + b * 8), k = 0; node && k < 1000 && !f; node = rd32(node), k++) {
                uint32_t c = rd32(node + 0x10);
                if (c && stem_lower(file_name(c)) == stem_lower(n)) f = c;
            }
        std::vector<uint8_t> img = f ? loaded_image(f) : std::vector<uint8_t>();
        if (img.empty()) missing += " " + n;
        else rq->items.push_back({f, rd32(f + 0xC), img});
    }
    if (rq->items.empty()) {
        delete rq;
        return "nothing to reset (not loaded:" + missing + ")";
    }
    size_t n = rq->items.size();
    request_batch(rq);
    return std::to_string(n) + " file(s) reset at the next frame" + (missing.empty() ? "" : " (not loaded:" + missing + ")");
}

// ------------------------------------------------------------------------------------------------ UI: sandbox
// Edit a script's source and swap it into the running game: devtools\sandbox\<name>.scp (started from the decompiled
// source) is opened in the user's editor; each save is compiled with devtools\dogc\DOGCodeCompiler.exe and, when it
// compiles, replaces the loaded file (process_swap). Revert puts back the image the game had loaded. Nothing in the
// game's data changes: the sandbox ends with the session.
struct Sandbox {
    std::string name;                                   // file name the game knows it by
    std::string src;                                    // devtools\sandbox\<stem>.scp
    std::vector<uint8_t> original;                      // the image the game loaded (for Revert)
    FILETIME stamp = {};                                // last compiled version of src
    bool swapped = false;
};
static std::vector<Sandbox> g_sandboxes;
static bool g_apply_on_save = true;
static bool g_apply_reset;                              // sandbox apply: reset instead of carrying values

struct CompileErr {
    int line;
    std::string msg;
};
static std::mutex g_cc_lock;
static std::vector<CompileErr> g_cc_errors;
static std::string g_cc_status;
static volatile LONG g_cc_busy;
static std::vector<uint8_t> g_cc_result;               // compiled image waiting to be applied
static std::string g_cc_for;                            // ... for this sandbox (file name)

static std::string devtools_dir()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "devtools\\");
    return path;
}

static Sandbox *sandbox_for(const std::string &name)
{
    for (auto &s : g_sandboxes)
        if (s.name == name) return &s;
    return nullptr;
}

static FILETIME file_stamp(const std::string &path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME none = {};
    return GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) ? a.ftLastWriteTime : none;
}

struct CcJob {
    std::string exe, src, out, name;
};
// run the compiler on j.src -> j.out; img = the bytecode when it compiled
static void run_compiler(const CcJob *j, std::vector<uint8_t> &img, std::vector<CompileErr> &errs, std::string &status)
{
    DeleteFileA(j->out.c_str());
    std::string log = j->out + ".log";
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = h;
    PROCESS_INFORMATION pi = {};
    char src[MAX_PATH];                                 // it runs in the output folder: the source path must be absolute
    if (!GetFullPathNameA(j->src.c_str(), MAX_PATH, src, nullptr)) lstrcpynA(src, j->src.c_str(), MAX_PATH);
    std::string cmd = "\"" + j->exe + "\" \"" + src + "\" \"" + j->out + "\"";
    std::vector<char> c(cmd.begin(), cmd.end());
    c.push_back(0);
    std::string dir = j->out.substr(0, j->out.find_last_of('\\'));
    if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        // its errors: "<file>(<line>) Error: "<message>"" (after an ESC colour code)
        if (FILE *f = fopen(log.c_str(), "rb")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                const char *e = strstr(line, ") Error: ");
                if (!e) continue;
                const char *open = e;
                while (open > line && *open != '(') open--;
                std::string msg = e + 9;
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
                if (msg.size() >= 2 && msg.front() == '"' && msg.back() == '"') msg = msg.substr(1, msg.size() - 2);
                errs.push_back({atoi(open + 1), msg});
            }
            fclose(f);
        }
        if (FILE *f = errs.empty() ? fopen(j->out.c_str(), "rb") : nullptr) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 16 && n < (8 << 20)) {
                img.resize(n);
                fread(img.data(), 1, n, f);
            }
            fclose(f);
        }
        if (!errs.empty()) status = std::to_string(errs.size()) + " compile error(s)";
        else if (img.size() < 8 || memcmp(img.data(), "SGOD", 4) || img[4] != 0x86) {
            status = "the compiler produced no usable bytecode (see " + log + ")";
            img.clear();
        } else
            status = "compiled (" + std::to_string(img.size()) + " bytes)";
    } else
        status = "could not run " + j->exe;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static DWORD WINAPI compile_thread(void *p)
{
    CcJob *j = (CcJob *)p;
    std::vector<uint8_t> img;
    std::vector<CompileErr> errs;
    std::string status;
    run_compiler(j, img, errs, status);
    {
        std::lock_guard<std::mutex> l(g_cc_lock);
        g_cc_errors = errs;
        g_cc_status = status;
        g_cc_result = img;
        g_cc_for = j->name;
    }
    delete j;
    InterlockedExchange(&g_cc_busy, 0);
    return 0;
}

static void start_compile(Sandbox &sb)
{
    std::string exe = devtools_dir() + "dogc\\DOGCodeCompiler.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::lock_guard<std::mutex> l(g_cc_lock);
        g_cc_status = "the script compiler (devtools\\dogc) is not installed - GladiusPort Builder's developer tools option "
                      "installs it";
        return;
    }
    if (InterlockedCompareExchange(&g_cc_busy, 1, 0)) return;
    sb.stamp = file_stamp(sb.src);
    std::string dir = devtools_dir() + "sandbox\\out";
    CreateDirectoryA(dir.c_str(), nullptr);
    CcJob *j = new CcJob{exe, sb.src, dir + "\\" + stem_of(sb.name) + (is_mood(sb.name) ? ".boo" : ".scb"), sb.name};
    {
        std::lock_guard<std::mutex> l(g_cc_lock);
        g_cc_status = "compiling...";
        g_cc_errors.clear();
    }
    CloseHandle(CreateThread(nullptr, 0, compile_thread, j, 0, nullptr));
}

// open (or start) the sandbox copy of the file in the code view
static void sandbox_open(FileModel &m)
{
    Sandbox *sb = sandbox_for(m.name);
    if (!sb) {
        g_sandboxes.push_back(Sandbox());
        sb = &g_sandboxes.back();
        sb->name = m.name;
        std::string dir = devtools_dir() + "sandbox";
        CreateDirectoryA(dir.c_str(), nullptr);
        sb->src = dir + "\\" + stem_of(m.name) + (is_mood(m.name) ? ".moo" : ".scp");
        uint32_t data = rd32(m.ptr + 0x84), size = rd32(m.ptr + 0x8C);   // what the game has loaded: for Revert
        if (data && size && size < (8 << 20)) {
            sb->original.resize(size);
            if (!rd(data, sb->original.data(), size)) sb->original.clear();
        }
        if (GetFileAttributesA(sb->src.c_str()) == INVALID_FILE_ATTRIBUTES) {     // start from the decompiled source
            std::string from = scripts_dir() + stem_of(m.name) + (is_mood(m.name) ? ".moo" : ".scp");
            CopyFileA(from.c_str(), sb->src.c_str(), TRUE);
        }
        sb->stamp = file_stamp(sb->src);                // edits from here on are new
    }
    // [devtools] Editor= (e.g. code, notepad++.exe); Notepad otherwise
    char editor[MAX_PATH];
    GetPrivateProfileStringA("devtools", "Editor", "notepad.exe", editor, sizeof editor, g_host->ini);
    std::string cmd = std::string("\"") + editor + "\" \"" + sb->src + "\"";
    std::vector<char> c(cmd.begin(), cmd.end());
    c.push_back(0);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        std::lock_guard<std::mutex> l(g_cc_lock);
        g_cc_status = std::string("could not start the editor ") + editor + " - set [devtools] Editor= in gladiusport.ini";
    }
}

// console 'scriptswap <file> <path>': a loaded file by name (with or without extension), new code from a .scp (compiled
// here) or a compiled .scb / .boo; the swap happens at the next frame (game thread)
std::string dogdbg_swap_file(const std::string &name, const std::string &path)
{
    uint32_t f = 0;
    std::string want = name;
    for (int b = 0; b < 64 && !f; b++)
        for (uint32_t node = rd32(FILE_HASH + b * 8), n = 0; node && n < 1000 && !f; node = rd32(node), n++) {
            uint32_t c = rd32(node + 0x10);
            std::string bn = base_name(file_name(c));
            if (c && (!_stricmp(bn.c_str(), want.c_str()) || !_stricmp(stem_of(bn).c_str(), want.c_str()))) f = c;
        }
    if (!f) return "no loaded script file called " + name + " (Scripts tab: Loaded script files)";
    std::vector<uint8_t> img;
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    if (!_stricmp(ext.c_str(), ".scp") || !_stricmp(ext.c_str(), ".moo")) {
        std::string dir = devtools_dir() + "sandbox";
        CreateDirectoryA(dir.c_str(), nullptr);
        CreateDirectoryA((dir + "\\out").c_str(), nullptr);
        CcJob j{devtools_dir() + "dogc\\DOGCodeCompiler.exe", path,
                dir + "\\out\\" + stem_of(base_name(file_name(f))) + (is_mood(file_name(f)) ? ".boo" : ".scb"), name};
        std::vector<CompileErr> errs;
        std::string status;
        run_compiler(&j, img, errs, status);
        for (auto &e : errs) con_print(OUT_ERROR, "  line %d: %s", e.line, e.msg.c_str());
        if (img.empty()) return "not swapped: " + status;
    } else if (FILE *fp = fopen(path.c_str(), "rb")) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n > 16 && n < (8 << 20)) {
            img.resize(n);
            fread(img.data(), 1, n, fp);
        }
        fclose(fp);
        if (img.size() < 8 || memcmp(img.data(), "SGOD", 4) || img[4] != 0x86) return "not a release Dog script: " + path;
    } else
        return "cannot read " + path;
    Sandbox *sb = sandbox_for(file_name(f));           // remember the original for the tab's Revert
    if (!sb) {
        g_sandboxes.push_back(Sandbox());
        sb = &g_sandboxes.back();
        sb->name = file_name(f);
        sb->src = path;
        uint32_t data = rd32(f + 0x84), size = rd32(f + 0x8C);
        if (data && size && size < (8 << 20)) {
            sb->original.resize(size);
            if (!rd(data, sb->original.data(), size)) sb->original.clear();
        }
        sb->stamp = file_stamp(path);
    }
    sb->swapped = true;
    request_swap(f, img, "scriptswap");
    return base_name(file_name(f)) + ": new code (" + std::to_string(img.size()) + " bytes) goes in at the next frame";
}

static void sandbox_panel(FileModel &m)
{
    Sandbox *sb = sandbox_for(m.name);
    bool online = con_online();
    ImGui::BeginDisabled(online || m.src.state != SRC_OK);
    if (ImGui::SmallButton(sb ? "Edit source" : "Sandbox: edit this script")) sandbox_open(m);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens a copy of the source in your editor (devtools\\sandbox). Each save is compiled and "
                          "swapped into the running game. Nothing in the game's data changes.");
    if (!sb) return;
    // a saved edit: compile it
    FILETIME now = file_stamp(sb->src);
    static DWORD last_poll;
    bool changed = CompareFileTime(&now, &sb->stamp) != 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(online || g_cc_busy);
    if (ImGui::SmallButton("Compile + apply") || (changed && g_apply_on_save && GetTickCount() - last_poll > 400)) {
        last_poll = GetTickCount();
        start_compile(*sb);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("on save", &g_apply_on_save);
    ImGui::SameLine();
    ImGui::Checkbox("reset", &g_apply_reset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: variables keep their values across the swap. On: the script starts over - fresh variables, "
                          "running instances stopped%s (options under Reset scripts).",
                          g_reset_init ? ", OnInit run again" : "");
    ImGui::SameLine();
    ImGui::BeginDisabled(online || sb->original.empty() || !sb->swapped);
    if (ImGui::SmallButton("Revert")) {
        request_swap(m.ptr, sb->original, "revert", !g_apply_reset, g_apply_reset && g_reset_kill,
                     g_apply_reset && g_reset_init);
        sb->swapped = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(g_swap_lock);
        if (g_swap_req) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s", g_swap_msg);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                delete g_swap_req;
                g_swap_req = nullptr;
                strcpy(g_swap_msg, "cancelled");
            }
        } else if (g_swap_msg[0])
            ImGui::TextDisabled("%s", g_swap_msg);
    }
    // a finished compile of this file: apply it
    std::vector<CompileErr> errs;
    std::string status;
    {
        std::lock_guard<std::mutex> l(g_cc_lock);
        if (g_cc_for == sb->name && !g_cc_result.empty()) {
            request_swap(m.ptr, g_cc_result, "apply", !g_apply_reset, g_apply_reset && g_reset_kill,
                         g_apply_reset && g_reset_init);
            sb->swapped = true;
            g_cc_result.clear();
        }
        errs = g_cc_errors;
        status = g_cc_status;
    }
    ImGui::TextDisabled("Sandbox: %s  -  %s", sb->src.c_str(), status.c_str());
    for (auto &e : errs)
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "  line %d: %s", e.line, e.msg.c_str());
}

static void code_view()
{
    FileModel &m = g_model;
    if (!g_view_file || !m.ptr) {
        ImGui::TextDisabled("Pick a running script or a loaded file on the left.");
        return;
    }
    static LONG swaps_seen;
    if (g_swap_done != swaps_seen) {                    // a sandbox swap gave the file new code (and a new id)
        swaps_seen = g_swap_done;
        if (g_view_file && rd32(g_view_file + 0x70)) load_model(m, g_view_file);
    }
    if (rd32(m.ptr + 0xC) != m.id) {                    // unloaded / replaced
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s is no longer loaded", m.name.c_str());
        return;
    }
    // a finished decompile: pick up its output; a file without source: decompile it once
    static LONG dec_seen;
    if (g_dec_done != dec_seen) {
        dec_seen = g_dec_done;
        load_source(m);
    }
    if (m.src.state != SRC_OK && !m.src.tried && !g_dec_busy) {
        m.src.tried = true;
        start_decompile(m);
    }

    ImGui::Text("%s", base_name(m.name).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%u bytes of code, %d functions)", (unsigned)m.code.size(), (int)m.funcs.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##fn", "go to function")) {
        for (auto &f : m.funcs)
            if (ImGui::Selectable(f.second.c_str())) view(m.ptr, f.first);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool src_ok = m.src.state == SRC_OK;
    ImGui::BeginDisabled(!src_ok);
    if (ImGui::RadioButton("Source", g_show_source && src_ok)) g_show_source = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::RadioButton("Disassembly", !g_show_source || !src_ok)) g_show_source = false;

    // trace intake: what ran in this file since the last UI frame
    DWORD now = GetTickCount();
    if (g_trace_file == m.ptr) {
        LONG n = g_trace_n;
        if (n - m.trace_seen > TRACE_RING) m.trace_seen = n - TRACE_RING;
        for (LONG j = m.trace_seen; j < n; j++) {
            int k = m.index_of(g_trace_ring[j & (TRACE_RING - 1)]);
            if (k < 0) continue;
            m.last_hit[k] = now ? now : 1;
            m.hits[k]++;
            m.latest = k;
        }
        m.trace_seen = n;
    }
    static bool follow = true;
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("keep the line that ran last in view");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset counts")) {
        std::fill(m.hits.begin(), m.hits.end(), 0);
        std::fill(m.last_hit.begin(), m.last_hit.end(), 0);
        m.latest = -1;
    }
    // source status
    if (!src_ok) {
        const char *why = m.src.state == SRC_STALE ? "saved source is for a different build of this script" : "no source yet";
        if (g_dec_busy) ImGui::TextDisabled("Source: decompiling...");
        else {
            ImGui::TextDisabled("Source: %s. %s", why, g_dec_msg);
            ImGui::SameLine();
            if (ImGui::SmallButton("Decompile")) start_decompile(m);
        }
    }
    if (!g_installed) ImGui::TextDisabled("Switch the debugger on to see the path the scripts take.");
    sandbox_panel(m);

    // the stopped instruction, and the selected script's position (debugger off: sampled between frames)
    bool here = g_stopped && g_stop.file == m.ptr;
    int cur_k = -1, sel_k = -1;
    if (here)
        for (size_t k = 0; k < m.ins.size(); k++) {
            const Ins &i = m.ins[k];
            if (g_stop.after ? i.off < g_stop.ip && i.off + 1 + 4 * i.n >= g_stop.ip : i.off == g_stop.ip) cur_k = (int)k;
        }
    if (!here && !g_installed && g_sel_script && rd32(g_sel_script + 0x1B4) == g_sel_id && rd32(g_sel_script + 0x80) == m.ptr)
        sel_k = m.index_of(rd32(g_sel_script + 0x1E0));

    ImGui::BeginChild("code", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (src_ok && g_show_source) draw_source(m, now, here, cur_k, sel_k, follow);
    else draw_disasm(m, now, here, cur_k, sel_k, follow);
    ImGui::EndChild();
}

static void var_rows(uint32_t tab, uint32_t count, uint32_t file, const char *suffix, bool globals, bool editable)
{
    if (!tab || count > 20000) return;
    size_t suf = suffix ? strlen(suffix) : 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = tab + i * 12, type = rd32(e + 4), val = rd32(e + 8);
        std::string name = sstr(file, rd32(e));
        size_t hash = name.find('#');
        if (suffix) {
            if (name.size() <= suf || name.compare(name.size() - suf, suf, suffix)) continue;
            name.resize(name.size() - suf);
        } else if (globals) {
            if (hash != std::string::npos || i == 0 || (type & 0x20)) continue;   // locals, self, functions
            if (g_model.ptr == file) {
                bool fn = false;
                for (auto &f : g_model.funcs) fn |= f.second == name;
                if (fn) continue;
            }
        }
        ImGui::PushID((int)e);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TableNextColumn();
        uint32_t t = type & 0xFF;
        if (editable && (t == 2 || t == 3)) {           // int / float: edit in place while stopped
            char buf[64];
            strcpy(buf, fmt_val(type, val).c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
                uint32_t nv;
                if (t == 2) nv = (uint32_t)strtol(buf, nullptr, 0);
                else { float f = (float)atof(buf); memcpy(&nv, &f, 4); }
                wr(e + 8, &nv, 4);
            }
        } else
            ImGui::TextUnformatted(fmt_val(type, val).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", type_name(type));
        ImGui::PopID();
    }
}

static bool var_table(const char *id)
{
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 44);
    return true;
}

static void inspector()
{
    uint32_t s = g_stopped ? g_stop.s : g_sel_script;
    if (!s || (!g_stopped && rd32(s + 0x1B4) != g_sel_id)) {
        ImGui::TextDisabled("Stop in a script, or pick a running one, to see its call stack and variables.");
        return;
    }
    bool editable = g_stopped && !con_online();
    uint32_t file = rd32(s + 0x80);
    int32_t fn = (int32_t)cur_func(s);
    ImGui::Text("%s  (instance %u)", file_name(file).c_str(), rd32(s + 0x1B4));
    if (!g_stopped) ImGui::TextDisabled("live values - stop the script to edit them");

    if (ImGui::CollapsingHeader("Call stack", ImGuiTreeNodeFlags_DefaultOpen)) {
        int depth = rd16s(s + 0x1BE);
        char top[300];
        _snprintf(top, sizeof top, "%s  @%04X##top", sym_name(file, fn).c_str(), rd32(s + 0x1E0));
        top[sizeof top - 1] = 0;
        if (ImGui::Selectable(top)) view(file, rd32(s + 0x1E0));
        for (int i = depth - 1; i >= 0 && i < 10; i--) {
            uint32_t fr = s + 0xEC + i * 0x14, ffile = rd32(fr + 8), ret = rd32(fr + 4);
            char line[300];
            _snprintf(line, sizeof line, "%s  @%04X%s%s##%d", sym_name(ffile, rd32(fr + 0x10)).c_str(), ret,
                      ffile != file ? "  in " : "", ffile != file ? file_name(ffile).c_str() : "", i);
            line[sizeof line - 1] = 0;
            if (ImGui::Selectable(line)) view(ffile, ret);
        }
        int turns = (int)rd32(TURN_DEPTH);
        if (turns > 1 && turns <= 10) {
            ImGui::TextDisabled("script nesting (outer first; each called the next through a native):");
            for (int t = 1; t <= turns; t++) {
                uint32_t o = rd32(TURN_STACK + 4 * t);
                if (!o) continue;
                uint32_t of = rd32(o + 0x80);
                ImGui::BulletText("%s%s, %s @%04X", o == s ? "* " : "", file_name(of).c_str(),
                                  sym_name(of, cur_func(o)).c_str(), rd32(o + 0x1E0));
            }
        }
    }
    if (ImGui::CollapsingHeader("Locals and arguments", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (fn < 0) ImGui::TextDisabled("not in a function");
        else if (var_table("locals")) {
            char suf[16];
            sprintf(suf, "#%d", fn);
            var_rows(rd32(s + 0x84), rd32(file + 0x48), file, suf, false, editable);
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("File globals (this instance)")) {
        if (var_table("globals")) {
            var_rows(rd32(s + 0x84), rd32(file + 0x48), file, nullptr, true, editable);
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Shared variables (every instance of the file)")) {
        if (var_table("shared")) {
            var_rows(rd32(s + 0x88), rd32(file + 0x54), file, nullptr, false, editable);
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Operand stack")) {
        int n = rd16s(s + 0x1BC);
        if (n <= 0) ImGui::TextDisabled("empty");
        for (int i = n - 1; i >= 0 && i < 12; i--)
            ImGui::Text("[%d] %s", i, fmt_val(rd32(s + 0x8C + i * 8), rd32(s + 0x8C + i * 8 + 4)).c_str());
    }
}

void panel_scripts()
{
    bool focus = g_focus_tab;
    if (!ImGui::BeginTabItem("Scripts", nullptr, focus ? ImGuiTabItemFlags_SetSelected : 0)) return;
    g_focus_tab = false;
    static bool was_stopped;
    if (g_stopped && !was_stopped) view(g_stop.file, g_stop.ip);   // jump to where it stopped
    was_stopped = g_stopped;

    toolbar();
    ImGui::Separator();
    if (ImGui::BeginTable("layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("code", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("vars", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("left");
        left_lists();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        code_view();
        ImGui::TableNextColumn();
        ImGui::BeginChild("vars");
        inspector();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

// the Quests tab's links into the code view
bool dogdbg_show_code(const std::string &file, uint32_t ip)
{
    for (auto &f : read_files())
        if (!_stricmp(stem_of(f.name).c_str(), stem_of(file).c_str())) {
            view(f.f, ip);
            tools_show_scripts();
            return true;
        }
    return false;
}

void dogdbg_shutdown()                                  // game thread (not stopped: the frame hook is running)
{
    g_want_on = false;
    if (g_installed) install(false);
    if (g_saved_err[0]) unpatch(0x184580, g_saved_err);
    g_saved_err[0] = 0;
}
