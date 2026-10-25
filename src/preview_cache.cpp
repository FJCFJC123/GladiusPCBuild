// Gladiator preview cache + school preload.
//
// The school / recruit / shop model viewer (wrapper: show 0x12D2D0, update 0x12D1C0; cache at wrapper +0x14) keeps the
// gladiators it has loaded in a list: viewer +0x24 head, entries of 0x1C bytes (+0 next, +8 display character, +0xC
// visual, +0x10 state), +0x2C the shown entry, +0x30 the hover delay before its load starts. 0x12CAE0 (cache add,
// thiscall viewer, character) evicts the oldest once it holds 6 (`cmp ecx, 6` at 0x12CC37), so a 7th reload threw one
// out and walking 7+ reloaded every one (~1 s each). The cap is 40 here: a full Roster40 school; each cached gladiator
// is well under 1 MB of game memory.
//
// Preload (school and shop screens, k_screens): the grid already holds every gladiator's display character (count
// +0xAC, array +0xC0), so on the screen's own viewer update calls (esi = the screen) every gladiator not
// yet cached is added and its load started (0x118820) - all of them PRELOAD_FIRST calls after the screen opens, then any
// new one every PRELOAD_EVERY calls - and the shown entry and its delay are put back, so they load in the background
// without being drawn. Runs on the game thread from the screen's own code (the screen is alive) and is paced by call
// count, so netplay PCs do the same thing on the same frames.
#include "port.h"
#include <stdint.h>

static const DWORD k_cache_add = 0x12CAE0, k_load = 0x118820, k_update = 0x12D1C0;
enum { CACHE_CAP = 40, PRELOAD_FIRST = 10, PRELOAD_EVERY = 45 };   // viewer updates (frames)

static bool cached(BYTE *viewer, DWORD ch)
{
    for (BYTE *e = *(BYTE **)(viewer + 0x24); e; e = *(BYTE **)e)
        if (*(DWORD *)(e + 8) == ch) return true;
    return false;
}

// Add one character to the viewer's cache and start its load without making it the shown one.
static void preload_one(BYTE *viewer, DWORD ch)
{
    DWORD cur = *(DWORD *)(viewer + 0x2C), delay = *(DWORD *)(viewer + 0x30);
    ((void *(__thiscall *)(void *, DWORD))k_cache_add)(viewer, ch);
    BYTE *e = *(BYTE **)(viewer + 0x2C);
    if (e && *(DWORD *)(e + 8) == ch && !*(DWORD *)(e + 0xC)) {
        *(float *)(viewer + 0x30) = 0.0f;               // the load (0x118820) waits for the hover delay to run out
        ((void(__thiscall *)(void *))k_load)(viewer);
    }
    *(DWORD *)(viewer + 0x2C) = cur;
    *(DWORD *)(viewer + 0x30) = delay;
}

// The screens with a gladiator grid: the grid and viewer wrapper offsets in the screen, the wrapper's class (the shop's
// is a subclass), how the grid holds its gladiators and its viewer update calls (all run with esi = the screen).
//   GRID_CHARS   (school, shop): count +0xAC, display characters at +0xC0 ([0] the shown one, [1..count] the list)
//   GRID_LIST    (battle select roster): count +0xAC, display characters at +0xC0 ([0..count-1])
//   GRID_BUTTONS (recruit): count +0xAC, buttons of 0x18 bytes at +0xC4, each +0x10 -> a recruit entry whose
//                +0xC -> the display character
enum { K_CHAR_VT = 0x341B54, K_BUTTON_VT = 0x351398 };     // display character / recruit list button classes
enum { GRID_CHARS, GRID_LIST, GRID_BUTTONS, MAX_LIST = 200 };
struct GridScreen { const char *name; DWORD grid_off, wrapper_off, wrapper_vt; int kind; DWORD sites[2]; };
static const GridScreen k_screens[] = {
    {"school", 0x4C, 0x50, 0x350A40, GRID_CHARS, {0x1436B4, 0x146EAA}},
    {"shop", 0x48, 0x4C, 0x3526B0, GRID_CHARS, {0x15D7EC, 0x15ACD7}},
    {"recruit", 0x58, 0x5C, 0x350A40, GRID_BUTTONS, {0x13B622, 0}},
    {"battle select", 0x64, 0x6C, 0x350A40, GRID_LIST, {0x115045, 0x1163C4}},
};

// The viewer and the grid's gladiators (at most CACHE_CAP - 1, so the shown one keeps its place), if `screen` really
// is one of k_screens (checked by class, so a call site shared with another screen does nothing).
static int grid_of(BYTE *screen, const GridScreen &k, BYTE **viewer, DWORD *chars, int *n)
{
    __try {
        BYTE *g = *(BYTE **)(screen + k.grid_off), *w = *(BYTE **)(screen + k.wrapper_off);
        if (!g || !w || *(DWORD *)w != k.wrapper_vt) return 0;
        int c = *(int *)(g + 0xAC), got = 0;
        if (c < 1 || c > (k.kind == GRID_CHARS ? CACHE_CAP : MAX_LIST)) return 0;
        for (int i = 0; i < c + (k.kind == GRID_CHARS); i++) {
            DWORD ch;
            if (k.kind != GRID_BUTTONS) {
                ch = ((DWORD *)(g + 0xC0))[i];
            } else {
                BYTE *b = g + 0xC4 + i * 0x18;
                if (*(DWORD *)b != K_BUTTON_VT) return 0;
                BYTE *entry = *(BYTE **)(b + 0x10);
                DWORD *pch = entry ? *(DWORD **)(entry + 0xC) : nullptr;
                ch = pch ? *pch : 0;
            }
            if (!ch) continue;
            if (*(DWORD *)ch != K_CHAR_VT) return 0;
            if (got < CACHE_CAP - 1) chars[got++] = ch;
        }
        *viewer = w + 0x14, *n = c;
        return got;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Each call: a screen's viewer update (once a frame). PRELOAD_FIRST calls after the screen opens every gladiator in
// its grid that isn't cached is added at once (their loads run side by side); every PRELOAD_EVERY calls after that
// any new one (a gladiator joining while the screen is open) is. A screen reopened at the same address is told apart
// by its cache: leaving a screen frees it, so the new one starts smaller than the last one was (the cap is never
// reached, so nothing is evicted).
static int cache_size(BYTE *viewer)
{
    int c = 0;
    for (BYTE *e = *(BYTE **)(viewer + 0x24); e; e = *(BYTE **)e) c++;
    return c;
}

static void __cdecl preload_step(BYTE *screen, const GridScreen *k)
{
    enum { SLOTS = 8 };
    static struct { BYTE *screen; const GridScreen *k; unsigned calls; int size; } st[SLOTS];
    static unsigned logged, next_slot;
    int s = 0;
    while (s < SLOTS && !(st[s].screen == screen && st[s].k == k)) s++;
    if (s == SLOTS) {
        s = next_slot++ % SLOTS;
        st[s].screen = screen, st[s].k = k, st[s].calls = 0, st[s].size = 0;
    }
    BYTE *viewer;
    DWORD chars[CACHE_CAP];
    int n, got = grid_of(screen, *k, &viewer, chars, &n), added = 0;
    if (!got) return;
    int size = cache_size(viewer);
    if (size < st[s].size) st[s].calls = 0;               // closed and reopened
    st[s].size = size;
    unsigned calls = ++st[s].calls;
    if (calls != PRELOAD_FIRST && (calls < PRELOAD_FIRST || (calls - PRELOAD_FIRST) % PRELOAD_EVERY)) return;
    for (int i = 0; i < got; i++) {
        if (cached(viewer, chars[i])) continue;
        preload_one(viewer, chars[i]);
        added++;
    }
    st[s].size = cache_size(viewer);
    if (added && logged++ < 20)
        port_log("preview: preloaded %d of %d %s gladiators (viewer update %u)\n", added, n, k->name, calls);
}

#define UPDATE_STUB(name, idx)                                                                                         \
    static __declspec(naked) void name()                                                                               \
    {                                                                                                                  \
        __asm pushad                                                                                                   \
        __asm push offset k_screens + idx * TYPE GridScreen                                                            \
        __asm push esi                                                                                                 \
        __asm call preload_step                                                                                        \
        __asm add esp, 8                                                                                               \
        __asm popad                                                                                                    \
        __asm mov eax, 0x12D1C0                                                                                        \
        __asm jmp eax                                                                                                  \
    }
UPDATE_STUB(school_update_stub, 0)
UPDATE_STUB(shop_update_stub, 1)
UPDATE_STUB(recruit_update_stub, 2)
UPDATE_STUB(battle_update_stub, 3)
static void (*const k_stubs[])() = {school_update_stub, shop_update_stub, recruit_update_stub, battle_update_stub};

void port_preview_cache_init()
{
    BYTE *p = (BYTE *)0x12CC37;
    if (p[0] == 0x83 && p[1] == 0xF9 && p[2] == 6 && p[3] == 0x0F && p[4] == 0x8C) {   // cmp ecx, 6; jl
        p[2] = CACHE_CAP;
        port_log("game: gladiator preview cache 6 -> %d\n", CACHE_CAP);
    } else {
        port_log("game: preview cache - unexpected bytes at 12CC37, left at 6\n");
        return;                                          // the preload relies on the bigger cache
    }
    for (int i = 0; i < (int)(sizeof k_screens / sizeof k_screens[0]); i++) {
        int hooked = 0, want = 0;
        for (DWORD s : k_screens[i].sites) {
            if (!s) continue;
            want++;
            BYTE *c = (BYTE *)s;
            if (c[0] == 0xE8 && s + 5 + *(int32_t *)(c + 1) == k_update) {
                *(int32_t *)(c + 1) = (int32_t)((DWORD)k_stubs[i] - (s + 5));
                hooked++;
            }
        }
        port_log("game: %s gladiator preload %s\n", k_screens[i].name, hooked == want ? "on" : "not installed");
    }
}
