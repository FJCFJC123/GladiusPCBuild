// Battle restart (debug menu "restart", and the editor's sandbox reload).
//
// Cross-referenced with protius.exe (PC, symbols): KeroberusFinishMenu "restart" sets battle-state flag 0x200 ->
// KeroberusState::HandleState -> KeroberusResetBattle(state, -1) (full reset: units back to their starts, scene,
// crowd, camera, intro track + music). The Xbox code is all there, nothing stripped: menu handler 0x7CEC0 sets
// state +0x24 |= 0x2000, HandleState 0x82310 clears it, calls 0x96990 (state cleanup) and ResetBattle 0x7D730(-1).
//
// What breaks: the Xbox ResetBattle starts with LoadingScreen(1) (0x1935F0, PC has no such call). The loading screen
// is hidden by the battle update 0x55C70 (from KeroberusUpdate 0x817A1) only once byte 0x37CA08 is set - by battle
// init (0x5902E) and Battle::ResetForNewWave (0x58E10, the "wave" path ResetBattle(1)). The restart path (-1) sets
// neither, so the loading screen never goes away (port: intro replays, then an endless loading screen). Fix: the
// restart's ResetBattle call (0x8237D) goes through restart_reset, which arms 0x37CA08 afterwards.
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

// Battle reload (editor Ctrl+R: battle -> pre-battle state 11 -> battle, editor.cpp). The encounter loader 0x69B20
// takes the battle's prize record from the prize pool (head 0x395210, 0x38 bytes; prizes.tok fills all but ~1 of
// it) into 0x3951E8, freed by 0x555E0 when the battle exits - except when the next state is 11 or 13 (battle
// ExitState 0x7EE14; 11 = the PC series path, never taken on the Xbox). So each reload leaked one: the next load
// got no prize (0x3951E8 = 0) and the results screen crashed reading it (0x10AE81, 2026-10-13). State 11's load
// call (0x114053) now frees a prize still held first.
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
