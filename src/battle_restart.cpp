// Battle restart (debug menu "restart" and the editor's battle reload).
// The Xbox ResetBattle (0x7D730) shows a loading screen that only hides once byte 0x37CA08 is set, and the
// restart path (-1) never sets it. restart_reset wraps the restart's ResetBattle call (0x8237D) and sets it.
#include "port.h"
#include <stdint.h>

static const DWORD RESET_BATTLE = 0x7D730;       // cdecl (KeroberusState *, int mode): -1 restart, 1 next wave
static const DWORD RESTART_CALL = 0x8237D;       // HandleState: flag 0x2000 -> ResetBattle(state, -1)
static BYTE *const g_hide_loading = (BYTE *)0x37CA08;
static DWORD *const g_battle_state = (DWORD *)0x3D2970;

typedef void(__cdecl *ResetBattleFn)(void *state, int mode);

static void __cdecl restart_reset(void *state, int mode)
{
    port_log("battle: restart (ResetBattle %d)\n", mode);
    ((ResetBattleFn)RESET_BATTLE)(state, mode);
    *g_hide_loading = 1;                          // battle update hides the loading screen next frame
}

// Battle reload (editor Ctrl+R) goes through pre-battle state 11, which skips freeing the battle's prize record
// (0x3951E8), so the next load gets no prize. Free a held prize before state 11's encounter load (0x114053).
static const DWORD ENC_LOAD = 0x69B20;           // cdecl (const char *enc, int, int) -> 0 ok
static const DWORD ENC_LOAD_CALL = 0x114053;     // pre-battle InitState
static const DWORD PRIZE_FREE = 0x555E0;
static DWORD *const g_prize = (DWORD *)0x3951E8;

typedef int(__cdecl *EncLoadFn)(const char *enc, int a, int b);

static int __cdecl prebattle_load(const char *enc, int a, int b)
{
    if (*g_prize) {
        port_log("battle: reload - prize record of the previous load freed\n");
        ((void(__cdecl *)())PRIZE_FREE)();
    }
    return ((EncLoadFn)ENC_LOAD)(enc, a, b);
}

// Repoint the E8 call at `site` (expected to call `expect`) to fn.
static bool redirect(DWORD site, DWORD expect, void *fn, const char *what)
{
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != expect) {
        port_log("battle: %s call unexpected at %06X - fix NOT installed\n", what, site);
        return false;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)fn - (site + 5));
    return true;
}

void port_battle_restart_init()
{
    redirect(RESTART_CALL, RESET_BATTLE, (void *)restart_reset, "restart");
    redirect(ENC_LOAD_CALL, ENC_LOAD, (void *)prebattle_load, "pre-battle encounter load");
}

// Ask the running battle to restart (same as the debug menu entry); false when no battle is running.
bool port_battle_restart()
{
    DWORD st = *g_battle_state;
    if (!st) return false;
    *(DWORD *)(st + 0x24) |= 0x2000;
    return true;
}
