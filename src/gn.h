// Deterministic-mode / netplay shared declarations - GladiusPort build (Xbox default.xbe addresses). Used by
// gladiusport.dll (gn_core.cpp, gn_hooks.cpp) and gladiusnet.dll. Cloned from ProtiusBuildMerged\gladiusnet.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#ifndef PORT_RELEASE                 // release build: see port.h
#define PORT_RELEASE 0
#endif

struct GnCtx {                       // filled by gn_hooks.cpp
    uint32_t version;
    uint8_t *padState;               // 4 pad slots: +0 DWORD connected, +4 Xbox XINPUT_GAMEPAD (20 bytes)
    uint32_t padBytes;               // PAD_BYTES
    uint32_t *randseed;              // _randseed
    int32_t *timeOverrideEnabled;
    float *timeDeltaOverride;
    float *timeClock;
    uint32_t *detFlag;               // exe-side deterministic-mode gate
    uint32_t *seedMix;               // XORed into the battle Srand seed
    int (__cdecl *getStateId)(void); // GameState::GetCurrentGameStateID
    void **randTraceFn;              // exe calls it on every Rand(): (return address, caller EBP)
    uint8_t *inputSys;               // Input::sInputSys (+0x18 = keyboard->joypad mapping on)
    uint32_t *padMap;                // versus G_PADMAP[4]: logical slot -> pad read by joypad_Read
    uint32_t *curTeam;               // versus G_CUR_TEAM (team whose save is being picked)
    uint32_t *joinPad;               // versus G_JOIN_PAD[4] (pad / network slot of player N)
    void **pcManager;                // MemCard::PCManager::sPCManager
    uint32_t *curMemCard;            // gCurrentMemCard (active port)
};

static const uint32_t CTX_VERSION = 8;   // 5 popup, 6 io wait, 7 fixed RealDelta, 8 save exchange
static const float FIXED_DT = 1.0f / 60.0f;
static const uint32_t PAD_SLOT = 0x18;      // padState stride
static const int NSLOTS = 4;
static const uint32_t PAD_BYTES = PAD_SLOT * NSLOTS;

extern GP_DATA char g_dir[MAX_PATH];
extern GP_DATA uint32_t g_frame;                    // session frame (trace label)

void path_in_dir(char *out, const char *name);
void logf(const char *fmt, ...);
bool hotkey(int vk, bool &latch, bool shift = false);
void det_enter(GnCtx *c);
void det_leave(GnCtx *c);
void pace();
void trace_open(GnCtx *c, const char *name);
void trace_close(GnCtx *c);
void dtrace(const char *fmt, ...);     // session trace line (no-op outside a session)
extern GP_DATA volatile uint32_t g_dtraceFrame;
void io_wait();                     // all DataStream async reads finished (deterministic mode)

// x87 control word: every machine must run the simulation with identical precision/rounding
// (D3D sets single precision at CreateDevice/Reset; a resized window may differ).
extern GP_DATA uint16_t g_sessionCw;        // enforced control word: host's value at session start
static inline uint16_t fpu_get()
{
    uint16_t cw;
    __asm { fnstcw cw }
    return cw;
}
static inline void fpu_set(uint16_t cw)
{
    __asm { fldcw cw }
}
void fpu_enforce(uint32_t frame);  // log a mismatch once per value, then force g_sessionCw

