// Bigger text buffers. Every on-screen string goes through three fixed buffers:
//   - 0x3ACCC8, 1 KB: the 8 text-draw functions (0x6F4xx-0x72xxx) vsprintf their format into it, unbounded;
//   - a 1 KB stack buffer in the two functions those hand it to (0x1C0590 draw item, 0x718B0 direct draw), which
//     vsprintf it again, with the stack cookie right behind it;
//   - 0x4B32A0, 4 KB: the frame's text area every queued string is copied into (cursor 0x380008, reset at 0x1C15D0;
//     a string that doesn't fit is dropped).
// The item inspect screen's class list (0x12E870) names every class that can use the item; with the AnyEquip debug
// cheat that is every class, over 1 KB, and the second vsprintf runs over the cookie -> _exit(3) -> KeBugCheck
// (Rags Mod, 136 classes; on GameCube too). Here the format buffer is 16 KB, the text area 64 KB, and the two
// stack-buffer functions are re-done with a 16 KB buffer. All of it is render-side; the cursor 0x380008 points into
// this DLL, which loads at a fixed base on every PC.
#include "port.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

enum { FMT_SIZE = 16384, AREA_SIZE = 65536 };
static char g_fmt[FMT_SIZE];                             // was 0x3ACCC8 (1 KB)
static char g_area[AREA_SIZE];                           // was 0x4B32A0 (4 KB)
static char g_line[FMT_SIZE];                            // the two re-done functions' format buffer (was on the stack)

static const DWORD k_old_fmt = 0x3ACCC8, k_old_area = 0x4B32A0, k_vsprintf = 0x274219;
static char **const g_cursor = (char **)0x380008;        // next free byte of the text area

static int fmt_into(char *buf, const char *fmt, va_list va)
{
    int n = _vsnprintf(buf, FMT_SIZE - 1, fmt, va);
    if (n < 0 || n > FMT_SIZE - 1) n = FMT_SIZE - 1;
    buf[n] = 0;
    return n;
}

// the 8 text-draw functions: vsprintf(0x3ACCC8, fmt, args), now bounded into g_fmt
static int __cdecl draw_vsprintf(char *, const char *fmt, va_list va) { return fmt_into(g_fmt, fmt, va); }

// 0x1C0590 (cdecl, a1..a6, fmt, ...): queue a draw item for the formatted text. a4 = flags (0 -> 0x300).
static void __cdecl text_item(DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = fmt_into(g_line, fmt, va);
    va_end(va);
    if (!n) return;
    char *p = *g_cursor;
    if ((intptr_t)p - (intptr_t)g_area + n + 1 >= AREA_SIZE) return;
    *g_cursor = p + n + 1;
    if (!p) return;
    strcpy(p, g_line);
    DWORD flags = a4 ? a4 : 0x300;
    BYTE **icur = (BYTE **)0x4B42A0, *item = *icur;     // draw item array: cursor, end 0x4B42A4, 0x30 bytes each
    if (item + 0x30 >= *(BYTE **)0x4B42A4) return;
    *icur = item + 0x30;
    if (!item) return;
    typedef BYTE *(__thiscall *ItemFn)(void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const char *, DWORD);
    BYTE *r = ((ItemFn)0x1BE4C0)(item, a1, a2, flags, a3, a5, a6, *(DWORD *)0x4B3180, p, *(DWORD *)0x4BC2A8);
    if (!r) return;
    BYTE **head = (BYTE **)0x4BC2B0, **tail = (BYTE **)0x4BC2B4;   // queued item list
    *(BYTE **)(r + 8) = *tail;
    *(DWORD *)(r + 4) = 0;
    if (!*head) *head = r;
    else *(BYTE **)(*tail + 4) = r;
    *tail = r;
}

// 0x718B0 (cdecl, a1..a6, fmt, ...): draw the formatted text now with the current font [0x4B3180]: scale a5 / a6,
// flags a4 (0 -> 0x300) into the font's +0x44 nibbles, a3 into +0x3C, draw at a1 / a2; scale and nibbles put back.
static void __cdecl text_draw(DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = fmt_into(g_line, fmt, va);
    va_end(va);
    if (!n) return;
    char *p = ((char *(__cdecl *)(int))0x1BDF40)(n);  // room in the text area
    if (!p) return;
    strcpy(p, g_line);
    DWORD flags = a4 ? a4 : 0x300;
    BYTE *font = *(BYTE **)0x4B3180;
    if (!font) return;
    typedef void(__thiscall *ScaleFn)(void *, DWORD, DWORD);
    typedef float(__thiscall *DrawFn)(void *, DWORD, DWORD, const char *);
    DWORD saved = *(DWORD *)(font + 0x44) & 0xFF;
    ((ScaleFn)0x1CFCE0)(font, a5, a6);
    font = *(BYTE **)0x4B3180;
    DWORD &f44 = *(DWORD *)(font + 0x44);
    f44 = (f44 & ~0xFFu) | (flags & 0xF) | ((flags >> 8 & 0xF) << 4);
    *(DWORD *)(font + 0x3C) = a3;
    ((DrawFn)0x1D1090)(font, a1, a2, p);
    ((ScaleFn)0x1CFCE0)(*(BYTE **)0x4B3180, 0x3F800000, 0x3F800000);
    DWORD &g44 = *(DWORD *)(*(BYTE **)0x4B3180 + 0x44);
    g44 = (g44 & ~0xFFu) | saved;
}

static bool g_bad;

static void put32(DWORD at, DWORD expect, DWORD value)
{
    if (*(DWORD *)at != expect) {
        port_log("text: unexpected bytes at %06X (%08X), not patched\n", at, *(DWORD *)at);
        g_bad = true;
        return;
    }
    *(DWORD *)at = value;
}

static void redirect_call(DWORD at, DWORD target, void *fn)
{
    BYTE *p = (BYTE *)at;
    if (p[0] != 0xE8 || at + 5 + *(int32_t *)(p + 1) != target) {
        port_log("text: unexpected call at %06X, not patched\n", at);
        g_bad = true;
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)fn - (at + 5));
}

static void jump_to(DWORD at, void *fn)
{
    BYTE *p = (BYTE *)at;
    static const BYTE sub404[6] = {0x81, 0xEC, 0x04, 0x04, 0x00, 0x00};   // sub esp, 0x404
    if (memcmp(p, sub404, 6)) {
        port_log("text: unexpected bytes at %06X, not replaced\n", at);
        g_bad = true;
        return;
    }
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)fn - (at + 5));
}

void port_text_buffers_init()
{
    // check everything first, then patch: a partial set would mix the old and new buffers
    static const DWORD fmt_refs[20] = {0x6F52E, 0x6F669, 0x6F69E, 0x6F7E3, 0x7024E, 0x703B1, 0x70438, 0x7046E,
                                       0x70589, 0x7062C, 0x71A1E, 0x71B59, 0x71B8E, 0x71CF3, 0x71D81, 0x71DAE,
                                       0x71EF3, 0x71F2E, 0x72093, 0x7212E};
    static const DWORD fmt_calls[8] = {0x6F532, 0x6F6A2, 0x70252, 0x70472, 0x71A22, 0x71B92, 0x71DB2, 0x71F32};
    static const DWORD area_refs[12] = {0x1BDF51, 0x1BEE4F, 0x1C04D1, 0x1C05D1, 0x1C0725, 0x1C08B6, 0x1C0B71,
                                        0x1C0C81, 0x1C0DDD, 0x1C0F04, 0x1C0FEE, 0x1C15D6};
    DWORD limits[12] = {};                               // "cmp reg, 0x1000" after each area check
    for (DWORD r : fmt_refs) g_bad |= *(DWORD *)r != k_old_fmt;
    for (DWORD c : fmt_calls) {
        BYTE *p = (BYTE *)c;
        g_bad |= p[0] != 0xE8 || c + 5 + *(int32_t *)(p + 1) != k_vsprintf;
    }
    for (int i = 0; i < 12; i++) {
        g_bad |= *(DWORD *)area_refs[i] != k_old_area;
        if (i == 11) break;                              // the reset has no limit
        for (DWORD a = area_refs[i] + 4; a < area_refs[i] + 24; a++) {
            const BYTE *p = (const BYTE *)a;
            if (p[0] == 0x81 && (p[1] & 0xF8) == 0xF8 && *(const DWORD *)(p + 2) == 0x1000) {
                limits[i] = a + 2;
                break;
            }
        }
        g_bad |= !limits[i];
    }
    if (g_bad) {
        port_log("text: game code not as expected - text buffers left at 1 KB / 4 KB\n");
        return;
    }
    for (DWORD r : fmt_refs) put32(r, k_old_fmt, (DWORD)g_fmt);
    for (DWORD c : fmt_calls) redirect_call(c, k_vsprintf, (void *)draw_vsprintf);
    for (int i = 0; i < 12; i++) put32(area_refs[i], k_old_area, (DWORD)g_area);
    for (int i = 0; i < 11; i++) put32(limits[i], 0x1000, AREA_SIZE);
    jump_to(0x1C0590, (void *)text_item);
    jump_to(0x718B0, (void *)text_draw);
    if (*g_cursor == (char *)k_old_area) *g_cursor = g_area;   // until the first frame's reset
    port_log("text: format buffer %u KB (was 1), frame text area %u KB (was 4)\n", FMT_SIZE / 1024, AREA_SIZE / 1024);
}
