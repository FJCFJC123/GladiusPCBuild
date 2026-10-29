// Interface between gladiusport.dll (oldhud_host.cpp) and oldhud.dll (the "OldHud" code: the Feb 2003 PS2 demo's
// battle HUD drawn in place of the retail HUD2). Plain C across the boundary; bump OLDHUD_ABI on any change.
#pragma once
#include <stdint.h>

#define OLDHUD_ABI 1

struct OldHudApi {
    uint32_t abi;
    void(__cdecl *load)();                  // battle InitState, after HUD2's textures (0x8EC90)
    void(__cdecl *unload)();                // battle CleanupState, before HUD2's textures are freed (0x8F8C0)
    void(__cdecl *draw)(void *kero);        // replaces the HUD2 draw (0x8E940) in DrawState
};

typedef void(__cdecl *OldHudLog)(const char *fmt, ...);
// exported by oldhud.dll; ini = gladiusport.ini ([oldhud] settings)
typedef const OldHudApi *(__cdecl *OldHudEntry)(uint32_t abi, OldHudLog log, const char *ini);
