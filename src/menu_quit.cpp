// Main menu "Quit Game" (keyboard Esc quits too, d3d_core.cpp). The main menu is a sub-state of the boot / menu
// state: its update (0xFE250, thiscall, called at 0x101BBE) reads the selected item's name on A
// (thiscall list->FUN_00036E20(&name) + string compare 0x1891F0) - "NewGame", "LoadGame", "Options", "VersuS" - and requests
// that state. Its list ([this+0x38], the list-menu API at 0x1255xx: find item 0x1256D0, add item 0x125660) gets a
// "Quit" item the first time the menu runs; picking it ends the game.
#include "port.h"
#include <stdint.h>
#include <string.h>

static void *g_list_done;

static bool selected_is(void *list, const char *name)
{
    struct Str { BYTE *p; } s = {nullptr};
    ((void(__thiscall *)(void *, void *))0x36E20)(list, &s);   // the selected item's name (refcounted string)
    bool eq = s.p && ((char(__thiscall *)(void *, const char *))0x1891F0)(&s, name) != 0;
    if (s.p && --*(short *)(s.p + 8) == 0) {
        void *h = ((void *(__cdecl *)(void *))0x188EE0)(s.p);
        ((void(__thiscall *)(void *, void *))0x176360)(h, s.p);
    }
    return eq;
}

static void __fastcall main_menu_update(BYTE *self)
{
    void *list = *(void **)(self + 0x38);
    if (list && list != g_list_done) {                  // add the item once per menu list
        g_list_done = list;
        if (!((void *(__thiscall *)(void *, const char *))0x1256D0)(list, "Quit")) {
            ((void *(__thiscall *)(void *, const char *, const char *, const char *))0x125660)(list, "Quit",
                                                                                              "Quit Game", "");
            port_log("menu: Quit Game added to the main menu\n");
        }
    }
    if (((int(__thiscall *)(void *, int, DWORD, int))0x74A40)((void *)0x3AD658, 0, 0x2020, 0) && list && selected_is(list, "Quit")) {
        port_log("quit (main menu)\n");
        ExitProcess(0);
    }
    ((void(__thiscall *)(void *))0xFE250)(self);
}

void port_menu_quit_init()
{
    BYTE *call = (BYTE *)0x101BBE;
    if (call[0] != 0xE8 || 0x101BBE + 5 + *(int32_t *)(call + 1) != 0xFE250) {
        port_log("menu: main menu code unexpected - Quit Game not added\n");
        return;
    }
    *(int32_t *)(call + 1) = (int32_t)((DWORD)main_menu_update - (0x101BBE + 5));
}
