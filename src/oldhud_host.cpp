// "OldHud" code ([codes] OldHud=1): loads oldhud.dll (src/oldhud) and lets it draw the battle HUD instead of the
// retail HUD2. Three calls in the battle state are repointed:
//   0x7D2CB DrawState -> HUD2 draw 0x8E940(kero)          -> oldhud draw (HUD2 not drawn)
//   0x7FE8B InitState -> HUD2 texture load 0x8EC90(kero)  -> HUD2 load, then oldhud's textures
//   0x7EDA0 CleanupState -> HUD2 texture free 0x8F8C0     -> oldhud's textures freed, then HUD2's
// Purely visual: the draw writes the same game state HUD2 does (kero+0x4128 zeroed, turn-order call), so it is not
// part of the netplay mod check.
#include "port.h"
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "oldhud/oldhud_api.h"

static const DWORD HUD2_DRAW = 0x8E940, HUD2_LOAD = 0x8EC90, HUD2_FREE = 0x8F8C0;
static const DWORD DRAW_CALL = 0x7D2CB, LOAD_CALL = 0x7FE8B, FREE_CALL = 0x7EDA0;
static const OldHudApi *g_hud;

typedef void(__cdecl *KeroFn)(void *kero);

static void __cdecl hud_draw(void *kero) { g_hud->draw(kero); }
static void __cdecl hud_load(void *kero) { ((KeroFn)HUD2_LOAD)(kero); g_hud->load(); }
static void __cdecl hud_free(void *kero) { g_hud->unload(); ((KeroFn)HUD2_FREE)(kero); }

static bool call_to(DWORD site, DWORD expect)
{
    const BYTE *p = (const BYTE *)site;
    return p[0] == 0xE8 && site + 5 + *(const int32_t *)(p + 1) == expect;
}

static void repoint(DWORD site, void *fn) { *(int32_t *)(site + 1) = (int32_t)((DWORD)fn - (site + 5)); }

static void __cdecl hud_log(const char *fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    port_log("%s", buf);
}

void port_oldhud_init(const char *ini)
{
    if (!GetPrivateProfileIntA("codes", "OldHud", 0, ini)) return;
    if (!call_to(DRAW_CALL, HUD2_DRAW) || !call_to(LOAD_CALL, HUD2_LOAD) || !call_to(FREE_CALL, HUD2_FREE)) {
        port_log("oldhud: battle HUD calls not as expected - OldHud off\n");
        return;
    }
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "oldhud.dll");
    HMODULE m = LoadLibraryA(path);
    OldHudEntry entry = m ? (OldHudEntry)GetProcAddress(m, "oldhud_api") : NULL;
    g_hud = entry ? entry(OLDHUD_ABI, hud_log, ini) : NULL;
    if (!g_hud) {
        port_log("oldhud: oldhud.dll %s - OldHud off\n", m ? "is for another version" : "not found");
        if (m) FreeLibrary(m);
        return;
    }
    repoint(DRAW_CALL, (void *)hud_draw);
    repoint(LOAD_CALL, (void *)hud_load);
    repoint(FREE_CALL, (void *)hud_free);
    port_log("oldhud: on (PS2 demo battle HUD)\n");
}
