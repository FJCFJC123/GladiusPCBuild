// Gameplay codes ([codes] in gladiusport.ini, the builder's Codes tab): ports of the GameCube Gecko codes, re-done
// against the Xbox code. Applied once at startup, before the game's C++ ctors run. The enabled set goes into
// port_mods_crc() (mods.cpp) so netplay only pairs PCs running the same codes.
#include "port.h"
#include <string.h>
#include <intrin.h>

static uint32_t g_codes;                                // bit per code that is on

enum {
    CODE_EXPANDED_CLASSES,      // 1  SkillUseClass table: CustomUnit1-32
    CODE_ROSTER_40,             // 2  40-gladiator school
    CODE_ANIM_POOL,             // 3  animation (DCT) pool 4000 -> 32000
    CODE_PRESERVE_JP,           // 4  recruits keep the JP their level earned
    CODE_INFINITE_JP,           // 5  recruits always have 100 JP
    CODE_INFINITE_MONEY,        // 6
    CODE_POISON_PERCENT,        // 7  poison ticks 10% of max HP
    CODE_BLEED_PERCENT,         // 8  bleeding ticks 10% of max HP
    CODE_UNLOCK_20,             // 9  all 20 roster slots open from the start
    CODE_COUNT
};

static const struct { const char *key; int excl; } k_codes[CODE_COUNT] = {
    {"ExpandedClasses", -1}, {"Roster40", CODE_UNLOCK_20}, {"AnimPool", -1},
    {"PreserveJP", CODE_INFINITE_JP}, {"InfiniteJP", CODE_PRESERVE_JP}, {"InfiniteMoney", -1},
    {"PoisonPercent", -1}, {"BleedPercent", -1}, {"Unlock20Slots", CODE_ROSTER_40},
};

// patch helpers: write only when the expected bytes are there
static bool patch_push(DWORD at, DWORD from, DWORD to)
{
    BYTE *p = (BYTE *)at;
    if (p[0] != 0x68 || *(DWORD *)(p + 1) != from) return false;
    *(DWORD *)(p + 1) = to;
    return true;
}

static bool patch_mov_edx(DWORD at, DWORD from, DWORD to)
{
    BYTE *p = (BYTE *)at;
    if (p[0] != 0xBA || *(DWORD *)(p + 1) != from) return false;
    *(DWORD *)(p + 1) = to;
    return true;
}

// 3: animation pool (the 'panlist' init at 0x1A6740): 4000 entries of 0x84 bytes -> 32000. GC: FUN_8005F01C.
static bool code_anim_pool()
{
    const DWORD n = 4000, N = 32000, elem = 0x84;
    BYTE *a = (BYTE *)0x1A6769, *c = (BYTE *)0x1A678D, *l = (BYTE *)0x1A67AD;
    if (a[0] != 0x68 || *(DWORD *)(a + 1) != n * elem || c[0] != 0x68 || *(DWORD *)(c + 1) != n || l[0] != 0xBA ||
        *(DWORD *)(l + 1) != n - 1)
        return false;
    return patch_push(0x1A6769, n * elem, N * elem) && patch_push(0x1A678D, n, N) &&
           patch_mov_edx(0x1A67AD, n - 1, N - 1);
}

// Per-frame codes run from the pad poll hook (gn_hooks.cpp), on the game thread at the same point of every frame, so
// netplay PCs stay identical. Player school = [0x3EE8B0] (the WEALTH debug command): +0x1C gold, +0x20 first gladiator;
// gladiator +0x10 next, +0x92 JP (u16, the debug JP command at 0xAF9D6).
static bool code_per_frame() { return true; }           // nothing to patch: port_codes_frame does the work

static const DWORD k_money = 999999;                   // GC set 0x51CE0000; a 6-digit total fits the shop/HUD
static const WORD k_jp = 100;                           // GC value

void port_codes_frame()
{
    if (!(g_codes & (1u << CODE_INFINITE_MONEY | 1u << CODE_INFINITE_JP))) return;
    BYTE *school = *(BYTE **)0x3EE8B0;
    if (!school) return;
    if (g_codes & (1u << CODE_INFINITE_MONEY))
        *(DWORD *)(school + 0x1C) = k_money;
    if (g_codes & (1u << CODE_INFINITE_JP))
        for (BYTE *u = *(BYTE **)(school + 0x20); u; u = *(BYTE **)(u + 0x10))
            *(WORD *)(u + 0x92) = k_jp;
}

// 7 / 8: status damage per turn (0xD87xx, GC FUN_800470C4). Bleeding (effect type 2) and poison (type 1) sum their
// effects into xmm0, then `cvttss2si edi, xmm0; mov ecx, edi` (edi = damage, printed; ecx = -damage after neg) just
// before the HP change 0xD6C40 on unit esi. That pair becomes a call: poison = 10% of max HP, bleeding = the larger
// of its own total and 10%. Max HP = word [[unit+0xB4]+0x6C] (0xD6D34). Integer math, same on every PC.
static __declspec(naked) void poison_stub()
{
    __asm {
        push eax
        push edx
        mov eax, [esi + 0xB4]
        movsx eax, word ptr [eax + 0x6C]
        cdq
        mov ecx, 10
        idiv ecx
        mov edi, eax
        mov ecx, edi
        pop edx
        pop eax
        ret
    }
}

static __declspec(naked) void bleed_stub()
{
    __asm {
        push eax
        push edx
        mov eax, [esi + 0xB4]
        movsx eax, word ptr [eax + 0x6C]
        cdq
        mov ecx, 10
        idiv ecx
        cvttss2si edi, xmm0
        cmp edi, eax
        jge keep
        mov edi, eax
    keep:
        mov ecx, edi
        pop edx
        pop eax
        ret
    }
}

static bool hook_tick(DWORD at, void *stub)
{
    BYTE *p = (BYTE *)at;
    static const BYTE orig[6] = {0xF3, 0x0F, 0x2C, 0xF8, 0x8B, 0xCF};     // cvttss2si edi, xmm0; mov ecx, edi
    if (memcmp(p, orig, 6)) return false;
    p[0] = 0xE8;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)stub - (at + 5));
    p[5] = 0x90;
    return true;
}

static bool code_poison() { return hook_tick(0xD88AB, poison_stub); }
static bool code_bleed() { return hook_tick(0xD87EB, bleed_stub); }

// 9: roster room check (0xADF90, GC FUN_800F7B00): count of [school+0x20] < cap, cap by tier [0x394E20]: 1 -> 8,
// 2 -> 12, 3 -> 19, else 20. `mov eax, [0x394E20]` -> `mov eax, 0` always takes the 20 cap (the GC code ends the same).
static bool code_unlock20()
{
    BYTE *p = (BYTE *)0xADF90;
    if (p[0] != 0xA1 || *(DWORD *)(p + 1) != 0x394E20) return false;
    p[0] = 0xB8;
    *(DWORD *)(p + 1) = 0;
    return true;
}

// 4: recruits arrive unskilled with the JP their level earned (5 at level 1, +10 at 2 and 3, +15 per level after).
// The recruit office loader (0x9EA50, GC FUN_8013DD04) builds each gladiator for sale through the unit-create wrapper
// 0xE6CB0 (cdecl: unit, name, level, flag; GC FUN_800DF4E8). Only that call site is redirected; starting schools,
// scripted / quest units, battle summons and previews are stock. Like the GC hook (0x800DFA78), the first skill the
// unit-create skill loop (0xE68F0, GC FUN_800DF5A0) adds sets the JP and zeroes the loop's level, so the level-range
// test drops the rest of the level's skills.
typedef int(__cdecl *UnitCreateFn)(void *unit, const char *name, int level, int flag);
typedef int(__cdecl *SkillAddFn)(void *list, void *skill);

static bool g_recruiting;                               // game thread: inside recruit_create

static WORD earned_jp(int level)
{
    if (level <= 1) return 5;
    if (level == 2) return 15;
    return (WORD)(25 + 15 * (level - 3));
}

static int __cdecl recruit_create(void *unit, const char *name, int level, int flag)
{
    g_recruiting = true;
    int r = ((UnitCreateFn)0xE6CB0)(unit, name, level, flag);
    g_recruiting = false;
    return r;
}

// 0xE6C69: call 0xB8100(unit + 0x2C, skill) in the unit-create skill loop (unit in edi, level at [esp+0x78])
static int __cdecl recruit_skill_add(void *list, void *skill)
{
    int r = ((SkillAddFn)0xB8100)(list, skill);
    BYTE *ret = (BYTE *)_AddressOfReturnAddress();
    if (g_recruiting && *(DWORD *)ret == 0xE6C6E) {
        BYTE *unit = (BYTE *)list - 0x2C;
        if (!*(WORD *)(unit + 0x92)) *(WORD *)(unit + 0x92) = earned_jp(*(short *)(unit + 0x90));
        *(int *)(ret + 4 + 8 + 0x78) = 0;               // caller esp = past the return address and 2 args
    }
    return r;
}

static bool code_preserve_jp()
{
    const DWORD site = 0x9EF7E, skill = 0xE6C69;
    BYTE *p = (BYTE *)site, *s = (BYTE *)skill;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != 0xE6CB0) return false;
    if (s[0] != 0xE8 || skill + 5 + *(int32_t *)(s + 1) != 0xB8100 || *(DWORD *)0xE6C40 != 0x78245C8B) return false;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)recruit_create - (site + 5));
    *(int32_t *)(s + 1) = (int32_t)((DWORD)recruit_skill_add - (skill + 5));
    return true;
}

// 2: 40-gladiator school (GC "40 Recruit School Inc"). Tier caps 8 / 12 / 20 -> 37 / 39 / 40; tier 3 stays 19 (a
// late-game scripted unit joins the school there). The gladiator grid widget holds 20 pointers at +0xC0 (count +0xAC)
// followed by +0x110 team and +0x114 school, so 40 entries need the tail fields moved (+0x180 / +0x184) and bigger
// allocations, as on GC. Every access was matched to the GC list (5 x +0x110, 4 x +0x114).
struct BytePatch { DWORD at; BYTE len; BYTE from[5]; BYTE to[5]; };

static bool apply_patches(const BytePatch *p, int n)
{
    for (int i = 0; i < n; i++)
        if (memcmp((void *)p[i].at, p[i].from, p[i].len)) {
            port_log("codes: unexpected bytes at %06X\n", p[i].at);
            return false;
        }
    for (int i = 0; i < n; i++) memcpy((void *)p[i].at, p[i].to, p[i].len);
    return true;
}

// rewrite a disp32 of 0x110 / 0x114 inside the instruction at `at`
static bool move_disp(DWORD at, DWORD from, DWORD to)
{
    for (int k = 2; k <= 4; k++)
        if (*(DWORD *)(at + k) == from) {
            *(DWORD *)(at + k) = to;
            return true;
        }
    return false;
}

static bool code_roster40()
{
    static const BytePatch caps[] = {
        {0xADFB1, 3, {0x83, 0xFA, 0x14}, {0x83, 0xFA, 0x28}},             // room check: default 20 -> 40
        {0xADFE2, 3, {0x83, 0xFA, 0x0C}, {0x83, 0xFA, 0x27}},             //   tier 2: 12 -> 39
        {0xADFFA, 3, {0x83, 0xFA, 0x08}, {0x83, 0xFA, 0x25}},             //   tier 1: 8 -> 37
        {0x11D163, 5, {0xBD, 0x14, 0, 0, 0}, {0xBD, 0x28, 0, 0, 0}},      // grid empty slots (GC 0x80170038)
        {0x11D17C, 5, {0xBD, 0x0C, 0, 0, 0}, {0xBD, 0x27, 0, 0, 0}},
        {0x11D195, 5, {0xBD, 0x08, 0, 0, 0}, {0xBD, 0x25, 0, 0, 0}},
        {0x115CB5, 3, {0x83, 0xF8, 0x14}, {0x83, 0xF8, 0x28}},             // grid fill stops at 40 (GC 0x80165854)
        {0x113D24, 5, {0x68, 0x18, 0x01, 0, 0}, {0x68, 0x00, 0x04, 0, 0}}, // battle setup grid 0x118 -> 0x400
        {0x153E3A, 5, {0x68, 0x14, 0x01, 0, 0}, {0x68, 0x56, 0x03, 0, 0}}, // school screen grids 0x114 -> 0x356
        {0x15E6EB, 5, {0x68, 0x14, 0x01, 0, 0}, {0x68, 0x56, 0x03, 0, 0}},
    };
    static const DWORD team[] = {0x10DF93, 0x111FA6, 0x1140F1, 0x115C43, 0x1113D6};   // +0x110
    static const DWORD school[] = {0x10DFA3, 0x111EE7, 0x115C6A, 0x115C7D};            // +0x114
    for (DWORD a : team)
        if (!(*(DWORD *)(a + 2) == 0x110 || *(DWORD *)(a + 3) == 0x110 || *(DWORD *)(a + 4) == 0x110)) return false;
    for (DWORD a : school)
        if (!(*(DWORD *)(a + 2) == 0x114 || *(DWORD *)(a + 3) == 0x114 || *(DWORD *)(a + 4) == 0x114)) return false;
    if (!apply_patches(caps, sizeof caps / sizeof caps[0])) return false;
    for (DWORD a : team) move_disp(a, 0x110, 0x180);
    for (DWORD a : school) move_disp(a, 0x114, 0x184);
    return true;
}

// 1: expanded SkillUseClass table. The class-name pool (internal 0x60310, display 0x60560; GC FUN_800FB9F4 /
// FUN_800FB724) has 0x38 entries and skill / ClassDef membership is a 64-bit BaseClassSet. New names CustomUnit1-32
// take indices 0x38-0x57 (as on GC); bits 64-95 live in a side table keyed by the set's address (GC kept them in MEM2).
// Every set operation is a real function on Xbox, so those are replaced whole: Clear 0x60CD0, Or 0x60CF0, AndNot
// 0x60D10, Intersects 0x60D40, IsEmpty 0x60D70, SetSingle 0x60D90. The only inline copies are the three
// "can this character use this skill" gates (0x5D2xx, 0x5E5xx, 0x5E6xx: classdef +0x14 copied to the stack before
// Intersects); their Intersects calls get the real classdef set instead. Name scans: caps 0x38 -> 0x58 at 0x6093D
// (name -> index), 0x60DED (name -> set), 0x10FF74 (class list).
static const int k_custom_first = 0x38, k_custom_count = 32;
static char g_custom_names[k_custom_count][16];

// side table: set address -> bits 64..95 (open addressing; only sets that ever held a custom class get an entry)
static const DWORD k_side_size = 1 << 15;
static DWORD g_side_key[k_side_size], g_side_val[k_side_size];
static volatile LONG g_side_used;
static SRWLOCK g_side_lock = SRWLOCK_INIT;

static DWORD side_get(const void *set)
{
    if (!g_side_used) return 0;
    DWORD k = (DWORD)(uintptr_t)set, h = (k >> 2) * 2654435761u >> 17, v = 0;
    AcquireSRWLockShared(&g_side_lock);
    for (DWORD i = 0; i < k_side_size; i++, h = (h + 1) & (k_side_size - 1)) {
        if (g_side_key[h] == k) { v = g_side_val[h]; break; }
        if (!g_side_key[h]) break;
    }
    ReleaseSRWLockShared(&g_side_lock);
    return v;
}

static void side_set(const void *set, DWORD v)
{
    if (!v && !g_side_used) return;
    DWORD k = (DWORD)(uintptr_t)set, h = (k >> 2) * 2654435761u >> 17;
    AcquireSRWLockExclusive(&g_side_lock);
    for (DWORD i = 0; i < k_side_size; i++, h = (h + 1) & (k_side_size - 1)) {
        if (g_side_key[h] == k) { g_side_val[h] = v; break; }
        if (!g_side_key[h]) {
            if (v) {                                    // a zero needs no entry
                g_side_key[h] = k;
                g_side_val[h] = v;
                InterlockedIncrement(&g_side_used);
                if (g_side_used == (LONG)(k_side_size * 3 / 4))
                    port_log("codes: ExpandedClasses side table 3/4 full\n");
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_side_lock);
}

static void __cdecl set_clear(DWORD *s) { s[0] = s[1] = 0; side_set(s, 0); }
static void __cdecl set_or(DWORD *a, const DWORD *b) { a[0] |= b[0]; a[1] |= b[1]; side_set(a, side_get(a) | side_get(b)); }
static void __cdecl set_andnot(DWORD *a, const DWORD *b) { a[0] &= ~b[0]; a[1] &= ~b[1]; side_set(a, side_get(a) & ~side_get(b)); }
static bool __cdecl set_intersects(const DWORD *a, const DWORD *b)
{
    return (a[0] & b[0]) || (a[1] & b[1]) || (side_get(a) & side_get(b));
}
static bool __cdecl set_isempty(const DWORD *s) { return !s[0] && !s[1] && !side_get(s); }
static void __cdecl set_single(DWORD *s, int idx)
{
    s[0] = s[1] = 0;
    DWORD hi = 0;
    if (idx >= 64 && idx < 96) hi = 1u << (idx - 64);
    else if (idx >= 32 && idx < 64) s[1] = 1u << (idx - 32);
    else if (idx < 64) s[0] = 1u << (idx & 31);         // < 32 (negatives as the original: shl by idx & 31)
    side_set(s, hi);
}

// the gates: replace the stack copy (first argument) with the unit's classdef set, unit in ebx / esi / ebp
static __declspec(naked) void gate_ebx()
{
    __asm {
        mov eax, [ebx + 0x20]
        add eax, 0x14
        mov [esp + 4], eax
        jmp set_intersects
    }
}
static __declspec(naked) void gate_esi()
{
    __asm {
        mov eax, [esi + 0x20]
        add eax, 0x14
        mov [esp + 4], eax
        jmp set_intersects
    }
}
static __declspec(naked) void gate_ebp()
{
    __asm {
        mov eax, [ebp + 0x20]
        add eax, 0x14
        mov [esp + 4], eax
        jmp set_intersects
    }
}

// name pool out-of-range branches: [esp+4] = index; default = "unknown" (internal) / localised (display)
static const char *__cdecl custom_name(int idx)
{
    return idx >= k_custom_first && idx < k_custom_first + k_custom_count ? g_custom_names[idx - k_custom_first]
                                                                           : nullptr;
}
static __declspec(naked) void int_name_default()
{
    __asm {
        push dword ptr [esp + 4]
        call custom_name
        add esp, 4
        test eax, eax
        jz orig
        ret
    orig:
        mov eax, 0x341FA0
        ret
    }
}
static __declspec(naked) void disp_name_default()
{
    __asm {
        push dword ptr [esp + 4]
        call custom_name
        add esp, 4
        test eax, eax
        jz orig
        ret
    orig:
        mov dword ptr [esp + 4], 0x33C644
        mov eax, 0xA0500
        jmp eax
    }
}

static bool hook_entry(DWORD fn, void *to, const BYTE *expect, int n)
{
    if (memcmp((void *)fn, expect, n)) return false;
    BYTE *p = (BYTE *)fn;
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)to - (fn + 5));
    return true;
}

static bool redirect_call(DWORD site, DWORD old_target, void *to)
{
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != old_target) return false;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    return true;
}

static bool redirect_ja(DWORD site, DWORD old_target, void *to)    // 0F 87 rel32
{
    BYTE *p = (BYTE *)site;
    if (p[0] != 0x0F || p[1] != 0x87 || site + 6 + *(int32_t *)(p + 2) != old_target) return false;
    *(int32_t *)(p + 2) = (int32_t)((DWORD)to - (site + 6));
    return true;
}

static bool code_expanded_classes()
{
    static const BytePatch caps[] = {
        {0x6093D, 3, {0x83, 0xFE, 0x38}, {0x83, 0xFE, 0x58}},
        {0x60DED, 3, {0x83, 0xFE, 0x38}, {0x83, 0xFE, 0x58}},
        {0x10FF74, 3, {0x83, 0xFE, 0x38}, {0x83, 0xFE, 0x58}},
    };
    static const struct { DWORD fn; void *to; BYTE head[5]; } fns[] = {
        {0x60CD0, (void *)set_clear, {0x8B, 0x44, 0x24, 0x04, 0xC7}},
        {0x60CF0, (void *)set_or, {0x8B, 0x44, 0x24, 0x04, 0x8B}},
        {0x60D10, (void *)set_andnot, {0x8B, 0x4C, 0x24, 0x08, 0x8B}},
        {0x60D40, (void *)set_intersects, {0x8B, 0x44, 0x24, 0x08, 0x8B}},
        {0x60D70, (void *)set_isempty, {0x8B, 0x4C, 0x24, 0x04, 0x33}},
        {0x60D90, (void *)set_single, {0x8B, 0x4C, 0x24, 0x08, 0x56}},
    };
    for (const auto &f : fns)
        if (memcmp((void *)f.fn, f.head, 5)) {
            port_log("codes: ExpandedClasses - unexpected set function at %06X\n", f.fn);
            return false;
        }
    const BYTE *p;
    static const DWORD gates[] = {0x5D2EE, 0x5E557, 0x5E64E};
    for (DWORD site : gates) {
        p = (const BYTE *)site;
        if (p[0] != 0xE8 || site + 5 + *(const int32_t *)(p + 1) != 0x60D40) return false;
    }
    p = (const BYTE *)0x60318;
    if (p[0] != 0x0F || p[1] != 0x87 || 0x60318 + 6 + *(const int32_t *)(p + 2) != 0x6046F) return false;
    p = (const BYTE *)0x60568;
    if (p[0] != 0x0F || p[1] != 0x87 || 0x60568 + 6 + *(const int32_t *)(p + 2) != 0x6082C) return false;
    if (!apply_patches(caps, sizeof caps / sizeof caps[0])) return false;

    for (int i = 0; i < k_custom_count; i++) sprintf(g_custom_names[i], "CustomUnit%d", i + 1);
    for (const auto &f : fns) hook_entry(f.fn, f.to, f.head, 5);
    redirect_call(0x5D2EE, 0x60D40, (void *)gate_ebx);
    redirect_call(0x5E557, 0x60D40, (void *)gate_esi);
    redirect_call(0x5E64E, 0x60D40, (void *)gate_ebp);
    redirect_ja(0x60318, 0x6046F, (void *)int_name_default);
    redirect_ja(0x60568, 0x6082C, (void *)disp_name_default);
    return true;
}

typedef bool (*CodeFn)();
static const CodeFn k_apply[CODE_COUNT] = {
    code_expanded_classes, code_roster40, code_anim_pool, code_preserve_jp, code_per_frame, code_per_frame, code_poison, code_bleed,
    code_unlock20,
};

extern "C" void port_codes_init(const char *ini)   // declared inside port_main.cpp's extern "C" port_start
{
    uint32_t want = 0;
    for (int i = 0; i < CODE_COUNT; i++)
        if (GetPrivateProfileIntA("codes", k_codes[i].key, 0, ini)) want |= 1u << i;
    for (int i = 0; i < CODE_COUNT; i++) {
        if (!(want & (1u << i))) continue;
        int x = k_codes[i].excl;
        if (x >= 0 && (g_codes & (1u << x))) {
            port_log("codes: %s off - can't be used with %s\n", k_codes[i].key, k_codes[x].key);
            continue;
        }
        if (!k_apply[i]) {
            port_log("codes: %s is not available in this build yet\n", k_codes[i].key);
            continue;
        }
        if (k_apply[i]()) {
            g_codes |= 1u << i;
            port_log("codes: %s on\n", k_codes[i].key);
        } else {
            port_log("codes: %s - unexpected game bytes, not applied\n", k_codes[i].key);
        }
    }
}

uint32_t port_codes_mask() { return g_codes; }
