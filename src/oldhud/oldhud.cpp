// oldhud.dll - the "OldHud" code: the battle HUD of the Feb 2003 PS2 playable demo (SLUS_123.45), drawn in place of
// the retail HUD2. Layout, textures and logic follow the demo (FUN_00161bc0 and its helpers); the data comes from the
// retail structures. The textures still ship in the retail gladius.bec. Not ported: the swing meter (retail keeps
// drawing its own), the demo's win/lose/pause texts and debug pages (retail screens replace them).
//
// Retail facts used here (all addresses retail VAs):
//   kero = KeroberusState: +0x24 flags (1 skill desc, 2 turn order, 4 inspect), +0x2C phase (3 orders, 4 decide,
//     6 skirmish), +0x4050 no-labels, +0x4110 active unit, +0x4124 HUD frame unit, +0x4128 skill scroll lock,
//     +0x412C unit under the cursor, +0x423A open menu (-1 none), +0x40FC/+0x4100/+0x4104 retail panel fades.
//   unit: +0x10 next, +0xB4 stats, +0xB8 classdef, +0xBC actor, +0xDC team, +0xDE controller (-1 AI), +0xE8 flags,
//     +0xEC status, +0xFC initiative, +0x102 HP, +0x105 SP, +0x106 pending delay, +0x109 weapon gauge max,
//     +0x114 weapon gauge, +0x118 shield affinity, +0x398 current skill entry.
//   stats: +0x24 combat block, +0x5C shield item, +0x6C max HP, +0x7A max SP, +0x82 weapon affinity, +0x84 shield
//     affinity, +0x85 initiative interval, +0x86 initiative per extra tile, +0x87 free move tiles, +0x88 move range.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "oldhud_api.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef int16_t s16;
typedef uint16_t u16;
typedef uint32_t u32;

static OldHudLog g_log;
static float g_text_scale = 1.6f;                       // demo scales were for the PS2 font; retail fonts are larger
static bool g_stack = true;                             // initiative stack (the demo's KeroDrawStack)

#define AT(T, p, off) (*(T *)((u8 *)(p) + (off)))

// ------------------------------------------------------------------------------------------------ retail API
typedef void(__cdecl *SpriteFn)(float, float, float, float, u32, void *, int);
typedef void(__cdecl *SpriteUvFn)(float, float, float, float, float, float, float, float, u32, void *);
typedef void(__cdecl *RectFn)(float, float, float, float, u32);
typedef void(__cdecl *TextFn)(float, float, float, int, u32, u32, int, const char *, ...);
typedef void(__cdecl *Float1Fn)(float);
typedef void(__cdecl *Float2Fn)(float, float);
typedef void(__cdecl *Float4Fn)(float, float, float, float);
typedef bool(__cdecl *ProjectFn)(float *out, const float *in, void *cam);

#define R_SPRITE ((SpriteFn)0x6FAC0)
#define R_SPRITE_UV ((SpriteUvFn)0x6FC10)
#define R_RECT ((RectFn)0x6F980)
#define R_TEXT ((TextFn)0x70240)                         // one drop shadow (shadow mode ignored)
#define R_ALPHA ((Float1Fn)0x6F210)
#define R_ORIGIN ((Float2Fn)0x6F1F0)
#define R_SCALE ((Float2Fn)0x6F170)
#define R_SCISSOR ((Float4Fn)0x701D0)                    // (0,0,0,0) = off
#define R_PROJECT ((ProjectFn)0x6F100)

static u32 *const g_font = (u32 *)0x4B3180;
static const u32 *const FONT1 = (u32 *)0x418484, *const FONT2 = (u32 *)0x41847C, *const FONT3 = (u32 *)0x418480;
static void *const CAM = (void *)0x39D580;              // battle camera
static float *const g_dt = (float *)0x49C218;
static float *const g_time = (float *)0x49C210;
static u8 **const g_units = (u8 **)0x415BA4;            // initiative list head, linked through unit+0x10

static const char *unit_name(void *u) { return ((const char *(__thiscall *)(void *))0xCCBD0)(u); }
static const char *skill_name(void *def) { return ((const char *(__thiscall *)(void *))0xB2E10)(def); }
static bool skill_usable(void *u, void *e) { return ((bool(__thiscall *)(void *, void *))0xD3FD0)(u, e); }
static int skill_index(void *u, void *e, u32 mask) { return ((int(__thiscall *)(void *, void *, u32))0xD6F60)(u, e, mask); }
static void *skill_at(void *u, int i, u32 mask) { return ((void *(__thiscall *)(void *, int, u32))0xD6EF0)(u, i, mask); }
static u32 tab_mask() { return ((u32(__cdecl *)())0x912E0)(); }
static u32 skill_category(void *e) { return ((u32(__cdecl *)(void *))0xB4860)(e); }
static int sp_cost(void *u, void *def) { return ((int(__cdecl *)(void *, void *))0xB4C40)(u, def); }
static int damage(void *combat, void *def) { return ((int(__cdecl *)(void *, void *))0x4EAB0)(combat, def); }
static bool has_shield(void *combat) { return ((bool(__cdecl *)(void *))0x4E940)(combat); }
static void pending_delay(void *kero) { ((void(__cdecl *)(void *))0x7DD90)(kero); }
// attack preview: total predicted damage on a unit from the game's prediction list (0 when none is running)
static int predicted_damage(void *u) { return ((int(__cdecl *)(void *))0x96780)(u); }
static void camera_apply(void *cam) { ((void(__thiscall *)(void *))0x1AB170)(cam); }

// ------------------------------------------------------------------------------------------------ textures
// A retail Texture is a 0x14-byte object: ctor thiscall 0x17A440(this, path, 2, 0, 0), dtor thiscall 0x17A710;
// +0x10 = the bitmap (0 when the file is missing).
enum Tex {
    T_ST_BACK, T_ST_BARBLUE, T_ST_BARRED, T_ST_TARGET, T_SKILLSSCROLL, T_GRAD_UP, T_GRAD_DOWN, T_STATBAR_GREY,
    T_SKILLBACK_L, T_SKILLBACK_R, T_ICON_WEAPON, T_ICON_AFFINITY, T_ICON_COMBO, T_ICON_MOVE, T_STUN, T_MRINC, T_MRDEC,
    T_ACCINC, T_ACCDEC, T_AFF_MASK, T_FRAME_LL, T_FRAME_LR, T_FRAME_RL, T_FRAME_RR, T_BIGSP_BLUE, T_BIGSP_RED,
    T_LITSP_BLUE, T_LITSP_RED, T_BAR_BLUE, T_BAR_RED, T_BAR_GREEN, T_BAR_PURPLE, T_CROWDMASK,
    T_FLAG_BLUE1, T_FLAG_BLUE2, T_FLAG_BLUE3, T_FLAG_BLUE4, T_FLAG_RED1, T_FLAG_RED2, T_FLAG_RED3, T_FLAG_RED4,
    T_AFFW = T_FLAG_RED4 + 1,                           // weapon affinity [1..6] (index 0 unused)
    T_AFFS = T_AFFW + 7,                                // shield affinity [1..6]
    T_COUNT = T_AFFS + 7
};

static const char *const k_tex[T_COUNT] = {
    "$t\\initiativestack\\st_back.tga", "$t\\initiativestack\\st_barblue.tga", "$t\\initiativestack\\st_barred.tga",
    "$t\\initiativestack\\st_target.tga", "$t\\hud\\hud_skillsscroll.tga", "$t\\hud\\alphagradientup.tga",
    "$t\\hud\\alphagradientdown.tga", "$t\\hud\\statbar_grey.tga", "$t\\hud\\hud_skillbackL.tga",
    "$t\\hud\\hud_skillbackR.tga", "$t\\hud\\hud_iconskill_weapon.tga", "$t\\hud\\hud_iconskill_affinity.tga",
    "$t\\hud\\hud_iconskill_combo.tga", "$t\\hud\\hud_iconskill_MoveToAttack.tga", "$t\\hud\\hud_iconstatus_stun.tga",
    "$t\\hud\\hud_iconstatus_MRInc.tga", "$t\\hud\\hud_iconstatus_MRDec.tga", "$t\\hud\\hud_iconstatus_AccInc.tga",
    "$t\\hud\\hud_iconstatus_AccDec.tga", "$t\\hud\\hud_affinitymask.tga", "$t\\hud\\hud_frameleftL.tga",
    "$t\\hud\\hud_frameleftR.tga", "$t\\hud\\hud_framerightL.tga", "$t\\hud\\hud_framerightR.tga",
    "$t\\hud\\bigspdot_blue.tga", "$t\\hud\\bigspdot_red.tga", "$t\\hud\\littlespdot_blue.tga",
    "$t\\hud\\littlespdot_red.tga", "$t\\hud\\statbar_blue.tga", "$t\\hud\\statbar_red.tga",
    "$t\\hud\\statbar_green.tga", "$t\\hud\\statbar_purple.tga", "$t\\hud\\hud_crowdmask.tga",
    "$t\\hud\\hud_cm_flag_blue1.tga", "$t\\hud\\hud_cm_flag_blue2.tga", "$t\\hud\\hud_cm_flag_blue3.tga",
    "$t\\hud\\hud_cm_flag_blue4.tga", "$t\\hud\\hud_cm_flag_red1.tga", "$t\\hud\\hud_cm_flag_red2.tga",
    "$t\\hud\\hud_cm_flag_red3.tga", "$t\\hud\\hud_cm_flag_red4.tga",
    NULL, "$t\\hud\\hud_affinityweapon_earth.tga", "$t\\hud\\hud_affinityweapon_water.tga",
    "$t\\hud\\hud_affinityweapon_death.tga", "$t\\hud\\hud_affinityweapon_air.tga",
    "$t\\hud\\hud_affinityweapon_fire.tga", "$t\\hud\\hud_affinityweapon_life.tga",
    NULL, "$t\\hud\\hud_affinityshield_earth.tga", "$t\\hud\\hud_affinityshield_water.tga",
    "$t\\hud\\hud_affinityshield_death.tga", "$t\\hud\\hud_affinityshield_air.tga",
    "$t\\hud\\hud_affinityshield_fire.tga", "$t\\hud\\hud_affinityshield_life.tga",
};

static __declspec(align(16)) u8 g_tex[T_COUNT][0x14];
static bool g_tex_on[T_COUNT];
static bool g_loaded;

static void __cdecl hud_load()
{
    if (g_loaded) return;
    int missing = 0;
    for (int i = 0; i < T_COUNT; i++) {
        if (!k_tex[i]) continue;
        memset(g_tex[i], 0, sizeof g_tex[i]);
        ((void *(__thiscall *)(void *, const char *, int, int, int))0x17A440)(g_tex[i], k_tex[i], 2, 0, 0);
        g_tex_on[i] = true;
        if (!AT(u32, g_tex[i], 0x10)) missing++;
    }
    g_loaded = true;
    g_log("oldhud: textures loaded (%d missing)\n", missing);
}

static void __cdecl hud_unload()
{
    if (!g_loaded) return;
    for (int i = 0; i < T_COUNT; i++)
        if (g_tex_on[i]) {
            ((void(__thiscall *)(void *))0x17A710)(g_tex[i]);
            g_tex_on[i] = false;
        }
    g_loaded = false;
}

static void *tex(int i) { return g_tex_on[i] && AT(u32, g_tex[i], 0x10) ? g_tex[i] : NULL; }

// ------------------------------------------------------------------------------------------------ draw helpers
static void spr(int t, float x, float y, float w, float h, u32 c = 0xFFFFFFFF)
{
    if (void *p = tex(t)) R_SPRITE(x, y, w, h, c, p, 0);
}

static void spr_tex(void *p, float x, float y, float w, float h, u32 c = 0xFFFFFFFF)
{
    if (p && AT(u32, p, 0x10)) R_SPRITE(x, y, w, h, c, p, 0);
}

// demo text call (x, y, scale, colour, align) with a black drop shadow; demo scale x g_text_scale
static void text(float x, float y, float scale, u32 c, int align, const char *fmt, ...)
{
    char buf[256];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    R_TEXT(x, y, scale * g_text_scale, 0, c, 0xFF000000, align, "%s", buf);
}

static u32 team_colour(int team)
{
    if (team < 0) return 0xFFDDDDDD;
    return ((u32 *)0x37D0BC)[team > 4 ? 4 : team];
}

static bool blink_on(float phase) { float t = *g_time + phase; return t - (float)(int)t > 0.5f; }

// ------------------------------------------------------------------------------------------------ 3.3 bars
// HP bar (demo FUN_0015fb80): y = bar centre
static void hp_bar(float x, float y, float w, float h, u8 *u, int team, bool show_text)
{
    u8 *st = AT(u8 *, u, 0xB4);
    int hp = AT(s16, u, 0x102), max = AT(s16, st, 0x6C);
    float fill = max > 0 ? w * hp / max : 0;
    if (fill < 0) fill = 0;
    int bar = team < 0 ? T_STATBAR_GREY : T_BAR_BLUE + (team > 3 ? 3 : team);
    if (has_shield(st + 0x24)) {
        spr(bar, x, y - h / 2, fill, h * 2 / 3);
        u8 *item = AT(u8 *, st, 0x5C);
        float smax = AT(float, AT(u8 *, item, 0xC), 0x3C), sdmg = AT(float, item, 0x10);
        if (smax > 0) spr(T_STATBAR_GREY, x, y + h / 6, w * (smax - sdmg) / smax, h / 3);
    } else
        spr(bar, x, y - h / 2, fill, h);
    if (show_text) text(x + w + 6, y, 0.6f, team_colour(team), 0x200, "%d/%d", hp, max);
}

// SP dots (demo FUN_0015fe60): one per 10 SP
static void sp_dots(float x, float y, float size, float step, u8 *u, int team)
{
    u8 *st = AT(u8 *, u, 0xB4);
    int sp = AT(s8, u, 0x105), max = AT(u8, st, 0x7A);
    int t = team == 1 || team == 2 ? T_BIGSP_RED : T_BIGSP_BLUE;
    for (int i = 10; i <= max; i += 10, x += step)
        if (i <= sp) spr(t, x, y - size / 2, size, size);
}

// ------------------------------------------------------------------------------------------------ 3.1 left frame
static void left_frame(u8 *u)
{
    int team = AT(s16, u, 0xDC);
    spr(T_FRAME_LL, 0, 294, 256, 134);
    spr(T_FRAME_LR, 256, 294, 93, 134);
    if (u8 *cls = AT(u8 *, u, 0xB8)) spr_tex(AT(void *, cls, 0x2C), 99, 352, 24, 24);
    u32 prev = *g_font;
    *g_font = *FONT3;
    text(127, 364, 0.7f, 0xFFDDDDDD, 0x200, "%s", unit_name(u));
    *g_font = prev;
    hp_bar(101, 391, 137, 19, u, team, true);
    sp_dots(98, 415, 14, 16, u, team);
    u32 status = AT(u32, u, 0xEC);
    float x = 290;
    if (status & 0x100) { spr(T_STUN, x, 402, 22, 22); x -= 24; }
    if (status & 0x4000000) { spr(T_ACCINC, x, 402, 22, 22); x -= 24; }
    if (status & 0x8000000) { spr(T_ACCDEC, x, 402, 22, 22); x -= 24; }
}

// ------------------------------------------------------------------------------------------------ 3.2 affinity
static void affinity(u8 *u, bool animate)
{
    u8 *st = AT(u8 *, u, 0xB4);
    int satype = AT(s8, st, 0x84);
    float r = AT(float, u, 0x118) / 100.0f;
    if (satype >= 1 && satype <= 6 && r > 0) {
        if (r < 1)
            spr(T_AFFS + satype, 53 - 37 * r, 398 - 37 * r, 74 * r, 74 * r);
        else if (!animate || blink_on(0.2f))
            spr(T_AFFS + satype, 16, 361, 74, 74);
    }
    spr(T_AFF_MASK, 2, 347, 39, 81);                    // under the weapon gauge: its glow shows through (demo order)
    int watype = AT(s8, st, 0x82);
    if (watype == -1) {                                 // "any" affinity: cycle like retail (0x8C4B0)
        static const int cyc[4] = {5, 2, 1, 4};
        watype = cyc[(int)*g_time & 3];
    }
    int wmax = AT(s8, u, 0x109);
    float w = wmax > 0 ? AT(float, u, 0x114) / wmax : 0;
    if (watype >= 1 && watype <= 6 && w > 0) {
        if (void *p = tex(T_AFFW + watype)) {
            if (w < 1) {
                float h = 130 * w + 4;
                R_SPRITE_UV(18, 433 - h, 24, h, 0, 1 - w, 1, 1, 0xFFFFFFFF, p);
            } else if (!animate || blink_on(0.1f))
                R_SPRITE(18, 295, 24, 138, 0xFFFFFFFF, p, 0);
        }
    }
}

// ------------------------------------------------------------------------------------------------ 3.4 skill list
static void skill_row(u8 *u, void *entry, float x, float y, int team)
{
    spr(T_SKILLBACK_L, x, y, 256, 27);
    spr(T_SKILLBACK_R, x + 256, y, 33, 27);
    if (!entry) return;
    u8 *def = AT(u8 *, entry, 0xC);
    if (!def) return;
    u32 c = skill_usable(u, entry) ? 0xFFDDDDDD : 0xFF606060;
    text(x + 9, y + 12, 0.6f, c, 0x200, "%s", skill_name(def));
    float cx = x + 268;
    if (skill_category(entry) & 9) {                    // attack / combo: predicted damage
        text(cx, y + 12, 0.6f, c, 0x201, "%d", damage(AT(u8 *, u, 0xB4) + 0x24, def));
        cx = x + 243;
    }
    u32 flags = AT(u32, def, 0xC);
    bool combo = AT(s8, def, 0x11) >= 1;
    if ((flags & 0x1000) && !combo) { spr(T_ICON_WEAPON, cx - 20, y + 3, 18, 18, c); cx -= 20; }
    if (flags & 0x40000) { spr(T_ICON_MOVE, cx - 20, y + 3, 18, 18, c); cx -= 20; }
    if (flags & 0x80000) { spr(T_ICON_AFFINITY, cx - 20, y + 3, 18, 18, c); cx -= 20; }
    if (combo) { spr(T_ICON_COMBO, cx - 20, y + 3, 18, 18, c); cx -= 20; }
    int cost = sp_cost(u, def);
    int dot = team == 1 || team == 2 ? T_LITSP_RED : T_LITSP_BLUE;
    for (int k = 0; k * 10 + 10 <= cost; k++) spr(dot, cx - 15 - 11 * k, y + 12 - 4.5f, 9, 9, c);
}

static float g_slide;                                   // rows still to slide (demo bs+0x4094)

static void skill_list(u8 *kero, u8 *u)
{
    // retail input scrolls only while kero+0x4128 == 0 and sets it to +-1 (HUD2 zeroes it every frame): take the
    // scroll for the animation and zero it like HUD2, so the game state is the same with either HUD
    float &lock = AT(float, kero, 0x4128);
    if (lock != 0) {
        g_slide += lock;
        lock = 0;
    }
    if (g_slide > 0) { g_slide -= *g_dt * 6; if (g_slide < 0) g_slide = 0; }
    else if (g_slide < 0) { g_slide += *g_dt * 6; if (g_slide > 0) g_slide = 0; }
    if (g_slide > 2) g_slide = 2;
    if (g_slide < -2) g_slide = -2;

    int team = AT(s16, u, 0xDC);
    void *cur = AT(void *, u, 0x398);
    u32 mask = tab_mask();
    int idx = cur ? skill_index(u, cur, mask) : -1;
    auto row = [&](int d) -> void * { return idx >= 0 && idx + d >= 0 ? skill_at(u, idx + d, mask) : NULL; };
    R_SCISSOR(319, 347, 300, 81);
    float base = 347 + (float)(int)(g_slide * 27);
    if (g_slide > 0) skill_row(u, row(-2), 319, base - 27, team);
    skill_row(u, row(-1), 319, base, team);
    skill_row(u, cur, 319, base + 27, team);
    skill_row(u, row(1), 319, base + 54, team);
    if (g_slide < 0) skill_row(u, row(2), 319, base + 81, team);
    spr(T_GRAD_UP, 319, 347, 285, 27, 0xFF000000);
    spr(T_GRAD_DOWN, 319, 401, 285, 27, 0xFF000000);
    R_SCISSOR(0, 0, 0, 0);
    // 3.5 scroll widget: retail scrolls with up / down
    spr(T_SKILLSSCROLL, 602, 349, 33, 81);
    text(612, 371, 0.55f, 0xFFF0C765, 0x202, "Up");
    text(612, 408, 0.55f, 0xFFF0C765, 0x202, "Dn");
}

// ------------------------------------------------------------------------------------------------ 3.6 crowd meter
static float crowd_fill(float c)
{
    if (c < 10) return 2 * c - 3;
    if (c < 30) return (c - 10) + 17;
    if (c < 60) return (c - 30) * 2 / 3 + 37;
    if (c < 100) return (c - 60) / 2 + 57;
    return 77;
}

static void crowd_meter()
{
    R_ORIGIN(331, 351);
    const float *crowd = (const float *)0x3D2BE8;       // displayed values (animated toward 0x3D2C08)
    static const u32 fillc[2] = {0x802563BE, 0x80A0521F};
    static const float colx[2] = {20, 141}, colw[2] = {117, 115};
    for (int t = 0; t < 2; t++) {
        float f = crowd_fill(crowd[t]);
        if (f > 0) R_RECT(colx[t], 77 - f, colw[t], f, fillc[t]);
        static const float ticks[3] = {19, 38, 58};
        for (float y : ticks) R_RECT(colx[t], y, colw[t], 2, 0xFFDDDDDD);
    }
    spr(T_CROWDMASK, 0, 0, 256, 77);
    static const float lim[4] = {10, 30, 60, 100}, fy[4] = {58, 38, 18, -2}, ty[4] = {68, 49, 28, 10};
    static const char *const roman[4] = {"I", "II", "III", "IV"};
    for (int t = 0; t < 2; t++)
        for (int l = 0; l < 4; l++) {
            float c = crowd[t];
            if (l < 3 ? c <= lim[l] : c < lim[l]) continue;
            int tx = (t ? T_FLAG_RED1 : T_FLAG_BLUE1) + l;
            if (tex(tx)) spr(tx, t ? 260.0f : -10.0f, fy[l], 32, 31);
            else text(t ? 271.0f : 10.0f, ty[l], 0.5f, 0xFFDDDDDD, 0x202, "%s", roman[l]);
        }
    R_ORIGIN(0, 0);
}

// ------------------------------------------------------------------------------------------------ 3.7 initiative stack
static void initiative_stack(u8 *kero)
{
    u8 *units[64];
    int work[64], n = 0;
    for (u8 *u = *g_units; u && n < 64; u = AT(u8 *, u, 0x10)) {
        units[n] = u;
        work[n++] = AT(u16, u, 0xFC);
    }
    if (!n) return;
    u8 *active = AT(u8 *, kero, 0x4110);
    u8 state = AT(u8, kero, 0x2C);
    int pending = 0;
    int marker = -1;
    if (active && (state == 3 || state == 4)) {
        pending_delay(kero);                            // retail's update calls it every orders frame too
        pending = AT(s16, active, 0x106);
        u8 *st = AT(u8 *, active, 0xB4);
        u8 *e = AT(u8 *, active, 0x398);
        u8 *def = e ? AT(u8 *, e, 0xC) : NULL;
        int path = *(int *)0x3D3778, tiles = *(s8 *)0x3D3AC8;
        if (state == 3 && def && (AT(u32, def, 0xC) & 0x40000) && !AT(void *, kero, 0x412C) &&
            path <= AT(u8, st, 0x88)) {
            int extra = tiles - AT(u8, st, 0x87) - 1;
            marker = (extra > 0 ? extra : 0) * AT(u8, st, 0x86);
        }
    }
    R_ORIGIN(530, 28);
    spr(T_ST_BACK, 0, 0, 120, 202.5f);
    float y = 10;
    int active_seen = 0;
    for (int slot = 0; slot < 12; slot++) {
        int best = 0;
        for (int i = 1; i < n; i++)
            if (work[i] < work[best]) best = i;
        u8 *u = units[best];
        u32 status = AT(u32, u, 0xEC);
        if ((status & 0x800001C2) || (AT(u32, u, 0xE8) & 0x2000)) {
            work[best] += 10000;
            continue;
        }
        if (marker >= 0 && slot > 0 && marker <= work[best]) {
            spr(T_ST_TARGET, 0, y - 6, 8, 12, 0xDDFFFFFF);
            marker = -1;
        }
        spr(AT(s16, u, 0xDC) == 0 ? T_ST_BARBLUE : T_ST_BARRED, 10, y, 93, 19.5f, 0xDDFFFFFF);
        if (u == active && ++active_seen == 2) {
            spr(T_ST_TARGET, -8, y + 1, 16, 23, 0xDDFFFFFF);
            work[best] += pending;
        }
        if (status & 0x800001C1) {
            text(15, y + 9.75f, 0.4f, 0xFFDDDDDD, 0x200, "XXXXXXXXXX");
            work[best] += 100000;
        } else {
            text(16, y + 9.75f, 0.4f, 0xFFDDDDDD, 0x200, "%s", unit_name(u));
            text(77.5f, y + 9.75f, 0.35f, 0xFFDDDDDD, 0x200, "%d", work[best]);
            work[best] += AT(u8, AT(u8 *, u, 0xB4), 0x85);
        }
        y += 19.5f;
    }
    R_ORIGIN(0, 0);
}

// ------------------------------------------------------------------------------------------------ 3.8 overhead label
static void overhead_label(u8 *u)
{
    u32 status = AT(u32, u, 0xEC);
    if (status & 0x40000001) return;
    u8 *actor = AT(u8 *, u, 0xBC);
    if (!actor) return;
    float in[3] = {AT(float, actor, 0x40), AT(float, actor, 0x44), AT(float, actor, 0x48) + 2.2f}, out[3];
    camera_apply(CAM);
    bool on = R_PROJECT(out, in, CAM);
    if (on) {
        float sy = out[1] - 10;
        if (sy < 45) sy = 45;
        R_ORIGIN(out[0], sy);
        float t = -14, b = 0;                           // flags 1 (name) + 4 (HP bar)
        u32 fl = AT(u32, u, 0xE8);
        if (fl & 0x300000)
            text(0, t - 12, 0.5f, 0xFFDDDDDD, 0x202, "%s%s", fl & 0x100000 ? "H+" : "", fl & 0x200000 ? "H-" : "");
        struct { u32 bit; const char *s; float dy, scale; u32 c; } k_words[] = {
            {0x20, "?", -8, 2.0f, 0xFFFF0000}, {0x40, "X", -8, 2.0f, 0xFFFF0000}, {0x80, "brr", -8, 2.0f, 0xFF0000FF},
            {0x100, "STUN", 8, 0.8f, 0xFFFF0000}, {0x200, "BERSERK", 8, 0.8f, 0xFFFF0000},
            {0x10, "!", -8, 2.0f, 0xFFFF0000}, {0x2000, "blind", -8, 0.8f, 0xFFFF0000},
            {0x4000, "supercharge", -8, 0.8f, 0xFF00FF00}, {0x8000, "immortal", -8, 0.8f, 0xFF00FF00},
            {0x10000, "rooted", -8, 0.8f, 0xFFFF0000},
        };
        for (auto &w : k_words)
            if (status & w.bit) text(0, t + w.dy, w.scale, w.c, 0x202, "%s", w.s);
        if (status & 0x20000)                           // doom: the countdown field is not known in retail yet
            R_TEXT(0, t - 8, 0.8f * g_text_scale, 0, 0xFF000000, 0xFFDDDDDD, 0x202, "%s", "DOOM");
        text(-18, t - 4, 0.5f, 0xFFDDDDDD, 0x200, "%s", unit_name(u));
        R_RECT(-20, b - 6, 58, 12, 0xFF000000);
        hp_bar(-18, b, 54, 8, u, AT(s16, u, 0xDC), false);
        // retail attack preview (HUD2's marker 0x8C830 -> HP bar 0x88860): predicted damage of the chosen skill
        // on this unit, after its defences; the HP it would lose blinks
        int dmg = predicted_damage(u);
        if (dmg > 0) {
            u8 *st = AT(u8 *, u, 0xB4);
            int hp = AT(s16, u, 0x102), max = AT(s16, st, 0x6C);
            if (max > 0 && hp > 0) {
                int left = hp > dmg ? hp - dmg : 0;
                float h = has_shield(st + 0x24) ? 8 * 2 / 3.0f : 8;
                if (blink_on(0))
                    R_RECT(-18 + 54.0f * left / max, b - 4, 54.0f * (hp - left) / max, h, 0xA0FFFFFF);
            }
            text(40, b, 0.5f, 0xFFFF4040, 0x200, "-%d", dmg);
        }
        R_ORIGIN(0, 0);
    }
    camera_apply(CAM);
}

// ------------------------------------------------------------------------------------------------ retail panels
// HUD2's user-opened panels (inspect, skill description, LT turn order) with HUD2's own fades (0x8E940)
static void panel(u8 *kero, int fade_off, bool on, void (*draw)(u8 *kero))
{
    float &f = AT(float, kero, fade_off);
    float step = *g_dt * 2;
    if (on) f = f + step > 1 ? 1 : f + step;
    else {
        if (f <= 0) return;
        f = f - step < 0 ? 0 : f - step;
    }
    R_ALPHA(f);
    draw(kero);
    R_ALPHA(1);
}

static void draw_turn_order(u8 *kero) { ((void(__cdecl *)(void *))0x8E5F0)(kero); }
static void draw_inspect(u8 *kero) { ((void(__cdecl *)(void *, void *))0x8DE20)(kero, AT(void *, kero, 0x412C)); }
static void draw_skill_desc(u8 *kero) { ((void(__cdecl *)(void *))0x8E1C0)(kero); }

// ------------------------------------------------------------------------------------------------ entry
static void __cdecl hud_draw(void *k)
{
    u8 *kero = (u8 *)k;
    if (!g_loaded) hud_load();
    *g_font = *FONT2;
    R_ALPHA(1);
    u8 state = AT(u8, kero, 0x2C);
    u8 *active = AT(u8 *, kero, 0x4110);
    u8 *frame = AT(u8 *, kero, 0x4124);
    u8 *cursor = AT(u8 *, kero, 0x412C);
    bool human = active && ((AT(s16, active, 0xDE) != -1 && state == 3) || state == 4);
    bool animate = AT(s16, kero, 0x423A) == -1;          // no battle menu open

    if (!AT(u8, kero, 0x4050) && human && cursor) overhead_label(cursor);
    if (g_stack && state != 6) initiative_stack(kero);
    spr(T_FRAME_RL, 319, 334, 256, 94);
    spr(T_FRAME_RR, 575, 334, 65, 94);
    if (human && frame) skill_list(kero, frame);
    else {
        AT(float, kero, 0x4128) = 0;                    // as HUD2
        g_slide = 0;
        crowd_meter();
    }
    if (frame) {
        left_frame(frame);
        affinity(frame, animate);
    }
    R_RECT(0, 428, 640, 36, 0xFF000000);

    panel(kero, 0x4104, (AT(u32, kero, 0x24) & 2) != 0, draw_turn_order);
    if (AT(void *, kero, 0x412C)) panel(kero, 0x40FC, (AT(u32, kero, 0x24) & 4) != 0, draw_inspect);
    else AT(float, kero, 0x40FC) = 0;
    panel(kero, 0x4100, (AT(u32, kero, 0x24) & 1) != 0, draw_skill_desc);

    // HUD2 epilogue: screen-space floaters (damage numbers, "Crowd Favor"), 2D state back to defaults
    *g_font = *FONT1;
    ((void(__cdecl *)(int))0x6CA40)(0);
    ((void(__cdecl *)(void *, int))0x6BFE0)(0, 0);
    R_SCALE(1, 1);
    R_ORIGIN(0, 0);
}

static const OldHudApi k_api = {OLDHUD_ABI, hud_load, hud_unload, hud_draw};

extern "C" __declspec(dllexport) const OldHudApi *__cdecl oldhud_api(uint32_t abi, OldHudLog log, const char *ini)
{
    if (abi != OLDHUD_ABI) return NULL;
    g_log = log;
    char v[32];
    if (ini && GetPrivateProfileStringA("oldhud", "TextScale", "", v, sizeof v, ini) && v[0]) {
        float f = (float)atof(v);
        if (f > 0.2f && f < 4) g_text_scale = f;
    }
    if (ini) g_stack = GetPrivateProfileIntA("oldhud", "InitiativeStack", 1, ini) != 0;
    g_log("oldhud: ready (text scale %.2f, initiative stack %s)\n", g_text_scale, g_stack ? "on" : "off");
    return &k_api;
}
