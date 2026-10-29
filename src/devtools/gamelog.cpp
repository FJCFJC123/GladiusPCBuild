// Game output retail still formats but throws away, caught and shown:
//   TexWin::printf 0x1DFC70 (cdecl win, fmt, ...): 91 sites print into the never-drawn DebugWin 0x481D30 - command
//     output (heap track, timer, ...), battle / anim / status debug lines.
//   sys_DebugTrace 0x1CCC90 (cdecl fmt, ...): a `ret` stub - unit / encounter loading, mem_Free, separators.
//   0x1CCCA0 (cdecl fmt, ...): a `ret` stub, sys_MessageBox's text ("%s : %s") - out of memory, bad data, script errors.
// Lines go to the Game log; while a console command runs they also show in the console. Traces / message boxes also
// go to port.log (DebugWin lines can come every frame, so they stay in the Game log).
#include "devtools.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <mutex>

static std::mutex g_lock;
static std::deque<std::string> g_lines;
static size_t g_gen;
static std::string g_partial;                           // DebugWin text arrives in pieces: a line ends at '\n'
static std::string g_last;
static int g_repeat;
volatile long g_exec_depth;                             // console.cpp: a typed command is running

static void add_line(const std::string &s, bool to_file)
{
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (s == g_last) {                              // collapse repeats (per-frame debug lines)
            g_repeat++;
            if (!g_lines.empty()) g_lines.back() = s + "  (x" + std::to_string(g_repeat + 1) + ")";
            g_gen++;
            return;
        }
        g_last = s;
        g_repeat = 0;
        g_lines.push_back(s);
        if (g_lines.size() > 20000) g_lines.pop_front();
        g_gen++;
    }
    if (g_exec_depth > 0) con_print(OUT_TEXT, "%s", s.c_str());
    if (to_file) g_host->log("game: %s\n", s.c_str());
}

static void add_text(const char *text, bool to_file)
{
    std::string s = text;
    size_t a = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '\n') {
            std::string line = g_partial + s.substr(a, i - a);
            g_partial.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            add_line(line, to_file);
            a = i + 1;
        }
    g_partial += s.substr(a);
    if (g_partial.size() > 400) { add_line(g_partial, to_file); g_partial.clear(); }
}

void gamelog_print(const char *line) { add_text((std::string(line) + "\n").c_str(), true); }

// crash report: the last n lines without waiting on a lock the crashing thread may hold
bool gamelog_tail(TailFn fn, void *ctx, size_t n)
{
    std::unique_lock<std::mutex> l(g_lock, std::try_to_lock);
    if (!l.owns_lock()) return false;
    for (size_t i = g_lines.size() > n ? g_lines.size() - n : 0; i < g_lines.size(); i++) fn(g_lines[i].c_str(), ctx);
    return true;
}

std::vector<std::string> gamelog_lines(size_t *gen)
{
    std::lock_guard<std::mutex> l(g_lock);
    if (gen) *gen = g_gen;
    return std::vector<std::string>(g_lines.begin(), g_lines.end());
}

void gamelog_clear()
{
    std::lock_guard<std::mutex> l(g_lock);
    g_lines.clear();
    g_last.clear();
    g_gen++;
}

static int __cdecl texwin_printf(void *win, const char *fmt, ...)
{
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(buf, sizeof buf - 1, fmt ? fmt : "", a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    (void)win;
    add_text(buf, false);
    return n < 0 ? (int)strlen(buf) : n;
}

static void __cdecl trace(const char *fmt, ...)
{
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt ? fmt : "", a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    add_text(buf, true);
}

// jmp at `at` when the expected bytes are there (the originals kept for gamelog_shutdown)
struct Hooked { DWORD at; uint8_t orig[5]; };
static Hooked g_hooked[3];
static int g_nhooked;
static bool jump(DWORD at, const uint8_t *expect, size_t n, void *to, const char *what)
{
    uint8_t *p = (uint8_t *)at;
    if (memcmp(p, expect, n)) {
        g_host->log("devtools: %s at %06X not as expected - not hooked\n", what, at);
        return false;
    }
    DWORD old;
    if (g_nhooked < 3) {
        g_hooked[g_nhooked].at = at;
        memcpy(g_hooked[g_nhooked++].orig, p, 5);
    }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uint8_t *)to - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void gamelog_shutdown()
{
    for (int i = 0; i < g_nhooked; i++) {
        uint8_t *p = (uint8_t *)g_hooked[i].at;
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p, g_hooked[i].orig, 5);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    g_nhooked = 0;
}

void gamelog_init()
{
    static const uint8_t stub[5] = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC};
    static const uint8_t texwin[5] = {0x8B, 0x4C, 0x24, 0x08, 0x56};   // mov ecx,[esp+8]; push esi
    int n = jump(0x1DFC70, texwin, 5, (void *)texwin_printf, "TexWin::printf") +
            jump(0x1CCC90, stub, 5, (void *)trace, "sys_DebugTrace stub") +
            jump(0x1CCCA0, stub, 5, (void *)trace, "message box stub");
    g_host->log("devtools: game output hooks %d of 3\n", n);
}
