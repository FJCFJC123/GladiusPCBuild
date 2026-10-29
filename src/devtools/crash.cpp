// Readable crash report: crash\<time>_<why>.txt next to the minidump (port_main.cpp write_dump calls in first).
// Written on the crashing thread while the process is failing: no heap allocation, every game read guarded, the line
// stores read with try-locks (crash_report never waits on a lock the crashing thread may hold).
//   exception + registers, game call stack (return addresses checked against a preceding call, each with the start
//   of its function: MSVC pads functions with int3 to 16 bytes), DLL frames, game state, running Dog scripts, memory
//   counters, and the last lines of port.log, the game's own output and the console.
#include "devtools.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const uint32_t IMG_LO = 0x12000, IMG_HI = 0x5E1000;   // the converted game image

static DWORD g_start;
static volatile uint32_t g_frames;

void crash_frame()                                      // game thread
{
    if (!g_start) g_start = GetTickCount();
    g_frames++;
}

// ------------------------------------------------------------------------------------------------ output
struct Report {
    HANDLE f;
    char buf[8192];
    size_t n;
};

static void flush(Report &r)
{
    DWORD w;
    if (r.n) WriteFile(r.f, r.buf, (DWORD)r.n, &w, nullptr);
    r.n = 0;
}

static void put(Report &r, const char *fmt, ...)
{
    char line[1200];
    va_list a;
    va_start(a, fmt);
    int k = _vsnprintf(line, sizeof line - 3, fmt, a);
    va_end(a);
    if (k < 0) k = sizeof line - 3;
    line[k] = 0;
    for (char *p = line; *p; p++) {                     // CRLF for Notepad
        if (r.n + 2 >= sizeof r.buf) flush(r);
        if (*p == '\n') r.buf[r.n++] = '\r';
        r.buf[r.n++] = *p;
    }
}

static void section(Report &r, const char *name) { put(r, "\n== %s ==\n", name); }

// ------------------------------------------------------------------------------------------------ guarded reads
static bool rd(uint32_t addr, void *out, size_t n)
{
    __try {
        memcpy(out, (const void *)(uintptr_t)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint32_t rd32(uint32_t addr, bool *ok = nullptr)
{
    uint32_t v = 0;
    bool g = rd(addr, &v, 4);
    if (ok) *ok = g;
    return g ? v : 0;
}

static const char *rd_str(uint32_t addr, char *buf, size_t cap)
{
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        char c;
        if (!rd(addr + (uint32_t)i, &c, 1)) break;
        if (!c) break;
        buf[i] = c >= 32 && c < 127 ? c : '?';
    }
    buf[i] = 0;
    if (!i) lstrcpynA(buf, "?", (int)cap);
    return buf;
}

// retail BString*: text at +0xC
static const char *rd_bstr(uint32_t rep, char *buf, size_t cap)
{
    if (!rep) { lstrcpynA(buf, "", (int)cap); return buf; }
    return rd_str(rep + 0xC, buf, cap);
}

// ------------------------------------------------------------------------------------------------ code addresses
static bool is_return_address(uint32_t v)
{
    uint8_t b[7];
    if (v < 0x10007 || !rd(v - 7, b, 7)) return false;
    if (b[2] == 0xE8) return true;                                          // call rel32
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return true;                 // call r/m (2 bytes)
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10) return true;                 // call [r+d8]
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return true;                 // call [r+d32] / [abs]
    if (b[0] == 0xFF && (b[1] & 0x38) == 0x10) return true;                 // call [r+r*s+d32]
    return false;
}

static uint32_t function_start(uint32_t a)              // first 16-aligned address after int3 padding
{
    for (uint32_t p = a & ~15u; p > IMG_LO && a - p < 0x10000; p -= 16) {
        uint8_t c;
        if (rd(p - 1, &c, 1) && c == 0xCC) return p;
    }
    return 0;
}

static void describe_code(uint32_t a, char *out, size_t cap)
{
    if (a >= IMG_LO && a < IMG_HI) {
        uint32_t fs = function_start(a);
        if (fs) _snprintf(out, cap, "game fn %06X+0x%X", fs, a - fs);
        else _snprintf(out, cap, "game");
    } else {
        HMODULE m = nullptr;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a, &m) && m) {
            GetModuleFileNameA(m, path, MAX_PATH);
            const char *base = strrchr(path, '\\');
            _snprintf(out, cap, "%s+0x%X", base ? base + 1 : path, a - (uint32_t)(uintptr_t)m);
        } else
            _snprintf(out, cap, "(no module)");
    }
    out[cap - 1] = 0;
}

static const char *exception_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    case 0xC0000374: return "heap corruption";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

// ------------------------------------------------------------------------------------------------ game context
static void game_state(Report &r)
{
    section(r, "game");
    bool ok;
    uint32_t st = rd32(0x38E45C, &ok);
    uint32_t id = st ? rd32(st + 8) : 0xFFFFFFFF;
    char name[64] = "?";
    for (int i = 0; i < 30; i++)                        // state-name table 0x37E380 {id, char *name, ...}
        if (rd32(0x37E380 + i * 16) == id) rd_str(rd32(0x37E384 + i * 16), name, sizeof name);
    put(r, "state         %u (%s)%s\n", id, name, id == 0x18 ? " - world map cutscene" : "");
    put(r, "requested     %u\n", rd32(0x38E43C));
    put(r, "online match  %s\n", con_online() ? "YES" : "no");
    if (g_start) put(r, "uptime        %u s, %u frames\n", (GetTickCount() - g_start) / 1000, g_frames);

    section(r, "dog scripts");
    uint32_t depth = rd32(0x4C90D4);
    uint32_t cur = rd32(0x483230);
    if (!cur) put(r, "no script running\n");
    if (depth > 10) depth = 10;
    put(r, "nesting depth %u (* = current)\n", depth);
    bool cur_listed = false;
    for (uint32_t i = 0; i <= depth; i++) {             // nested Run stack 0x4C90AC[]
        uint32_t s = rd32(0x4C90AC + 4 * i);
        if (!s) continue;
        char fn[160];
        uint32_t file = rd32(s + 0x80);
        rd_bstr(file ? rd32(file + 0x10) : 0, fn, sizeof fn);
        put(r, "%s [%u] %08X  %s  ip 0x%X\n", s == cur ? "*" : " ", i, s, fn[0] ? fn : "(no file)", rd32(s + 0x1E0));
        cur_listed |= s == cur;
    }
    if (cur && !cur_listed) {
        char fn[160];
        uint32_t file = rd32(cur + 0x80);
        rd_bstr(file ? rd32(file + 0x10) : 0, fn, sizeof fn);
        put(r, "* %08X  %s  ip 0x%X\n", cur, fn[0] ? fn : "(no file)", rd32(cur + 0x1E0));
    }

    section(r, "memory");
    put(r, "tracked bytes %u in %u blocks\n", rd32(0x418BD0), rd32(0x418BD4));
}

static void tail_line(const char *line, void *ctx) { put(*(Report *)ctx, "  %s\n", line); }

// ------------------------------------------------------------------------------------------------ report
void crash_report(EXCEPTION_POINTERS *ep, const char *path, const char *why)
{
    if (dt_release()) return;                           // release: no reports (gladiusport.dll asks for none either)
    static Report r;                                    // static: the crashing thread's stack may be nearly gone
    r.f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (r.f == INVALID_HANDLE_VALUE) return;
    r.n = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    put(r, "GladiusPort crash report - %04u-%02u-%02u %02u:%02u:%02u (%s, thread %u)\n", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, why, GetCurrentThreadId());
    char where[MAX_PATH + 40];
    if (ep) {
        EXCEPTION_RECORD *e = ep->ExceptionRecord;
        CONTEXT *c = ep->ContextRecord;
        describe_code(c->Eip, where, sizeof where);
        section(r, "exception");
        put(r, "%08X %s at %08X  [%s]\n", e->ExceptionCode, exception_name(e->ExceptionCode), c->Eip, where);
        if (e->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->NumberParameters >= 2)
            put(r, "%s of address %08X%s\n", e->ExceptionInformation[0] == 8 ? "execute" : e->ExceptionInformation[0] ? "write" : "read",
                (uint32_t)e->ExceptionInformation[1], e->ExceptionInformation[1] < 0x10000 ? " (NULL pointer + offset)" : "");
        put(r, "EAX %08X EBX %08X ECX %08X EDX %08X\nESI %08X EDI %08X EBP %08X ESP %08X\n", c->Eax, c->Ebx, c->Ecx,
            c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);

        section(r, "call stack (return addresses found on the stack, newest first)");
        uint32_t sp = c->Esp, top = (uint32_t)(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
        int shown = 0;
        for (uint32_t a = sp; a + 4 <= top && a < sp + 0x10000 && shown < 40; a += 4) {
            uint32_t v;
            if (!rd(a, &v, 4)) break;
            bool game = v >= IMG_LO && v < IMG_HI;
            if (!is_return_address(v)) continue;
            if (!game) {
                HMODULE m;
                if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCSTR)(uintptr_t)v, &m))
                    continue;
            }
            describe_code(v, where, sizeof where);
            put(r, "  [esp+%04X] %08X  %s\n", a - sp, v, where);
            shown++;
        }
    }
    __try { game_state(r); } __except (EXCEPTION_EXECUTE_HANDLER) { put(r, "(reading the game state failed)\n"); }
    section(r, "console (last 30 lines)");
    if (!con_tail(tail_line, &r, 30)) put(r, "  (busy)\n");
    section(r, "game output (last 40 lines)");
    if (!gamelog_tail(tail_line, &r, 40)) put(r, "  (busy)\n");
    section(r, "port.log (last 80 lines)");
    if (!log_tail(tail_line, &r, 80)) put(r, "  (busy)\n");
    flush(r);
    CloseHandle(r.f);
}
