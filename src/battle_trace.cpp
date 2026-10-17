// Battle skill tracer: logs each skirmish (skill in progress) stage change of Skirmish::Update (0x97920) and warns
// when a stage takes long. Read only.
#include "port.h"
#include <stdint.h>
#include <string.h>

static const char *const STAGE[] = {"NONE", "LOADING", "PRESWING_MOVEA", "PRESWING_MOVEB", "PRESWING_MOVEC",
                                    "TARGET_SETUP", "METER_WAITFORSTART", "SWINGMETER_RUN", "PROXYSCENEA",
                                    "PROXYSCENEB", "ANIM_TAGWAIT", "ATTACK_REPEATED", "PROJECTILES", "ATTACK_CHAIN",
                                    "DISTANCEDELAY", "HITS_ARE_DONE", "COST", "SCRIPT", "DONE"};
static const char *stage_name(uint8_t s) { return s < sizeof STAGE / sizeof STAGE[0] ? STAGE[s] : "?"; }

// True when n bytes at p are committed, accessible memory.
static bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

// Skirmish summary for the log: unit, skill, skill id and flags.
static void describe(const BYTE *sk, char *out, size_t n)
{
    const BYTE *skill = *(const BYTE *const *)(sk + 0xFC), *unit = *(const BYTE *const *)(sk + 0x28);
    unsigned id = 0, flags = 0;
    if (readable(skill, 0x10))
        id = *(const uint16_t *)(skill + 8), flags = *(const uint32_t *)(skill + 0xC);
    _snprintf(out, n, "skirmish %p unit %p skill %p id %04X flags %08X", (void *)sk, (void *)unit, (void *)skill, id,
              flags);
    out[n - 1] = 0;
}

typedef void(__cdecl *UpdateFn)(void *kero, BYTE *sk);
// Wraps Skirmish::Update: logs stage changes and stages that take 5 / 10 / 20 / 40 / 80 s.
static void __cdecl skirmish_update(void *kero, BYTE *sk)
{
    static const BYTE *last_sk;
    static int warned;                                  // "still in" reports given for this stage
    uint8_t before = sk[0x1C];
    ((UpdateFn)0x97920)(kero, sk);
    uint8_t after = sk[0x1C];
    float t = *(const float *)(sk + 0x20);
    char d[160];
    if (sk != last_sk || after != before) {
        describe(sk, d, sizeof d);
        port_log("battle: %s: %s -> %s\n", d, sk != last_sk ? "(new)" : stage_name(before), stage_name(after));
        last_sk = sk;
        warned = 0;
        return;
    }
    static const float marks[] = {5, 10, 20, 40, 80};
    if (warned < 5 && t >= marks[warned]) {             // the stage is taking long: say so (stalled skills)
        describe(sk, d, sizeof d);
        port_log("battle: %s: still in %s after %.0f s\n", d, stage_name(after), t);
        warned++;
    }
}

// Async package load race: 0x1E63C0 starts a load, then marks its slot "loading" (1E63C8). If the load already
// finished on an I/O thread, that write reset "done" (2) back to 1 and skills waiting on the model hung forever.
// mark_loading leaves a finished load alone.
static __declspec(naked) void mark_loading()
{
    __asm {
        cmp word ptr [esi + 0x12], 2
        je done
        mov word ptr [esi + 0x12], 1
    done:
        ret
    }
}

// Patch the "loading" write at 0x1E63C8 into a call to mark_loading.
static void install_load_race_fix()
{
    BYTE *p = (BYTE *)0x1E63C8;
    static const BYTE expect[6] = {0x66, 0xC7, 0x46, 0x12, 0x01, 0x00};
    if (memcmp(p, expect, 6)) {
        port_log("fix: async load state write at 1E63C8 unexpected - not patched\n");
        return;
    }
    p[0] = 0xE8;                                        // call mark_loading; nop
    *(int32_t *)(p + 1) = (int32_t)((DWORD)mark_loading - (0x1E63C8 + 5));
    p[5] = 0x90;
    port_log("fix: async package load no longer overwrites a completed load (1E63C8)\n");
}

// Install the load race fix and the skirmish tracer (call 0x99C51).
void port_battle_trace_init()
{
    install_load_race_fix();
    BYTE *p = (BYTE *)0x99C51;
    if (p[0] != 0xE8 || 0x99C51 + 5 + *(int32_t *)(p + 1) != 0x97920) {
        port_log("battle: skirmish call at 099C51 unexpected - tracer off\n");
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)skirmish_update - (0x99C51 + 5));
}
