// Save / load screen paging ([game] SavePages=1): up to 27 saves, 3 per page, LB / RB (Q / W, PageUp / PageDown)
// flip pages. A save's slot number is a "[slot][version]" tag in its name; every call that hands the save system a
// slot gets page * 3 + widget, the device scan keeps every slot, and the other "3 most recent" limits go to 27.
#include "port.h"
#include <stdint.h>
#include <string.h>
#include <limits.h>

enum { PER_PAGE = 3, MAX_PAGES = 9, MAX_SAVES = PER_PAGE * MAX_PAGES };
static int g_page;
static void *g_screen;
static int g_dev = -2;
static bool g_screen_seen, g_screen_live;               // save_update ran since the last / previous frame hook

int port_player1_page_pressed();                        // xapi_input.cpp: -1 LB / white, +1 RB / black, 0

typedef int(__cdecl *SlotFn)(int, void *, int, void *, void *);
// Slot calls from the save screen, offset by the current page.
static int __cdecl slot_get(int idx, void *rec, int dev, void *name, void *x)
{
    return ((SlotFn)0x1D7C70)(idx + g_page * PER_PAGE, rec, dev, name, x);
}

typedef int(__cdecl *VerFn)(int, int, void *, int);
static int __cdecl slot_version(int slot, int dev, void *out, int f)
{
    return ((VerFn)0x1D9970)(slot + g_page * PER_PAGE, dev, out, f);
}
typedef int(__cdecl *Slot2Fn)(int, int);
static int __cdecl slot_action(int slot, int dev) { return ((Slot2Fn)0x36060)(slot + g_page * PER_PAGE, dev); }
static int __cdecl slot_save(int slot, int dev) { return ((Slot2Fn)0x3A830)(slot + g_page * PER_PAGE, dev); }

// True when the screen's sub-panel at `off` is open.
static bool panel_open(BYTE *self, int off)
{
    BYTE *p = *(BYTE **)(self + off);
    return p && p[0xD];
}

typedef void(__thiscall *UpdateFn)(void *, int);
typedef void(__thiscall *RefreshFn)(void *);
// Save screen update (vtable 0x33F32C): flip pages on LB / RB, then run the retail update.
static void __fastcall save_update(BYTE *self, void *edx, int arg)
{
    (void)edx;
    int dev = *(int *)0x37C5EC;                         // selected storage device, -1 = none
    g_screen_seen = true;
    if (self != g_screen || dev != g_dev)               // screen opened / other device: first page
        g_screen = self, g_dev = dev, g_page = 0;
    int dir = port_player1_page_pressed();
    if (dir && dev != -1 && !self[0x14C] && !self[0x14D] && !panel_open(self, 0x1B0) && !panel_open(self, 0x1B4) &&
        !panel_open(self, 0x1B8)) {
        int count = ((int(__cdecl *)(int))0x1D8270)(dev);
        int pages = count / PER_PAGE + 1;               // every save + a free slot for a new one
        if (pages > MAX_PAGES) pages = MAX_PAGES;
        int np = g_page + dir;
        np = np < 0 ? 0 : np >= pages ? pages - 1 : np;
        if (np != g_page) {
            g_page = np;
            port_log("saves: page %d of %d (%d saves on device %d)\n", g_page + 1, pages, count, dev);
            (*(RefreshFn *)(*(BYTE **)self + 0x10))(self);   // the screen's refresh: refetch the 3 slots
        }
    }
    ((UpdateFn)0x3BB20)(self, arg);
}

// Device scan (replaces FUN_001D8DA0): same calls and status codes as retail, but MAX_SAVES slots and multi-digit
// slot tags. Device record: +8 device, +0xC status, +0x18 / +0x1C save count, +0x20 name.
struct SaveEntry {
    DWORD a, b;
    char *name, *other;
    short slot, state;
    DWORD pad;
};
static_assert(sizeof(SaveEntry) == 0x18, "save entry layout");

static void *vfn(void *o, int off) { return (*(void ***)o)[off / 4]; }
typedef char(__cdecl *DevTest)(int);

// First "[number]" at or after *pos: its value (and *pos moves past it); -1 if none.
static long tag_after(const char *s, int *pos)
{
    for (int i = *pos; s[i]; i++) {
        if (s[i] != '[')
            continue;
        int j = i + 1;
        long v = 0;
        while (s[j] >= '0' && s[j] <= '9' && j - i <= 9)
            v = v * 10 + (s[j++] - '0');
        if (j > i + 1 && s[j] == ']') {
            *pos = j + 1;
            return v;
        }
    }
    return -1;
}

// Assign each slot its newest save, fill empty slots with the rest, and load them into the device's slot cache.
static void scan_slots(BYTE *rec, int dev, void *obj)
{
    int best[MAX_SAVES], ver[MAX_SAVES];
    for (int s = 0; s < MAX_SAVES; s++)
        best[s] = -1, ver[s] = 0;
    int n = *(int *)(rec + 0x1C);
    if (n > 0) {
        typedef void *(__cdecl * AllocFn)(UINT);
        typedef void(__cdecl * FreeFn)(void *);
        AllocFn alloc = *(AllocFn *)0x5937FC;
        FreeFn freef = *(FreeFn *)0x593800;
        BYTE *blk = alloc ? (BYTE *)alloc(n * sizeof(SaveEntry) + 4) : nullptr;
        SaveEntry *e = nullptr;
        if (blk) {
            *(int *)blk = n;
            e = (SaveEntry *)(blk + 4);
            for (int i = 0; i < n; i++)
                ((void(__thiscall *)(void *))0x1D6F10)(&e[i]);
        }
        int *vers = ((int *(__cdecl *)(int, int))0x176BC0)(4, n * 4);
        if (((char(__thiscall *)(void *, void *, int))0x271290)(obj, e, n) && e && vers) {
            for (int i = 0; i < n; i++) {               // "... hrs [slot][version]"
                vers[i] = 0;
                if (!e[i].name)
                    continue;
                int pos = 0;
                long slot = tag_after(e[i].name, &pos);
                if (slot < 0)
                    continue;
                e[i].slot = (short)(slot < 0x7FFF ? slot : -1);
                long v = tag_after(e[i].name, &pos);
                vers[i] = v < 0 ? 0 : v;
            }
            for (int s = 0; s < MAX_SAVES; s++) {       // each slot: its newest save
                int pick = -1, top = INT_MIN;
                for (int i = 0; i < n; i++)
                    if (e[i].state == 1 && e[i].slot == s && vers[i] > top)
                        pick = i, top = vers[i];
                if (pick >= 0)
                    e[pick].state = 2, best[s] = pick, ver[s] = vers[pick];
            }
            for (int s = 0; s < MAX_SAVES; s++) {       // untagged / older copies fill the empty slots
                if (best[s] >= 0)
                    continue;
                int pick = -1, top = INT_MIN;
                for (int i = 0; i < n; i++)
                    if (e[i].state == 1 && vers[i] > top)
                        pick = i, top = vers[i];
                if (pick >= 0)
                    e[pick].state = 2, best[s] = pick, ver[s] = vers[pick];
            }
        }
        if (blk) {
            for (int i = 0; i < n; i++)
                ((void(__thiscall *)(void *))0x1D6F30)(&e[i]);
            if (freef)
                freef(blk);
        }
        if (vers)
            ((void(__cdecl *)(void *))0x176CA0)(vers);
    }
    for (int s = 0; s < MAX_SAVES; s++) {               // load each slot (index -1 clears it)
        ((int(__cdecl *)(int, int, int))0x1D82D0)(s, best[s], dev);
        BYTE *entry = ((BYTE * (__thiscall *)(void *, int))0x1D7B30)(rec, s);
        if (entry)
            *(int *)(entry + 0x1C) = ver[s];
    }
}

// Device scan entry: status checks, then scan_slots.
static void __fastcall scan_device(BYTE *rec)
{
    if (BYTE *s = *(BYTE **)(rec + 0x20)) {             // drop the record's name string (refcounted)
        if (--*(short *)(s + 8) == 0) {
            void *h = ((void *(__cdecl *)(void *))0x188EE0)(s);
            ((void(__thiscall *)(void *, void *))0x176360)(h, s);
        }
        *(BYTE **)(rec + 0x20) = nullptr;
    }
    int &st = *(int *)(rec + 0xC);
    st = -1;
    int dev = *(int *)(rec + 8);
    void *mgr = *(void **)0x4CAEB4;
    if (!mgr || !((char(__thiscall *)(void *, int))vfn(mgr, 4))(mgr, dev)) {
        st = 1;
        return;
    }
    auto devobj = [&]() { return dev < 0 ? nullptr : ((void *(__thiscall *)(void *, int))vfn(mgr, 8))(mgr, dev); };
    void *d = devobj();
    if (!d || !((char(__thiscall *)(void *))vfn(d, 0x24))(d)) {
        st = 4;
        return;
    }
    d = devobj();
    if (!d || ((char(__thiscall *)(void *))vfn(d, 0x28))(d)) {
        st = 2;
        return;
    }
    d = devobj();
    if (d && !((char(__thiscall *)(void *))vfn(d, 0x1C))(d)) {
        st = 3;
        return;
    }
    if (!((DevTest)0x1D78C0)(dev)) { st = 5; return; }
    if (!((DevTest)0x1D7830)(dev)) { st = 6; return; }
    if (!((DevTest)0x1D77D0)(dev)) { st = 7; return; }
    void *obj = ((void *(__cdecl *)(int))0x1D7340)(dev);
    if (!obj)
        return;
    *(int *)(rec + 0x18) = ((int(__thiscall *)(void *, int, int))vfn(obj, 4))(obj, 0, 0);
    *(int *)(rec + 0x1C) = -1;
    int r = (unsigned char)((int(__cdecl *)(int))0x1D9530)(dev);
    if (r == 3) {
        st = 0;
        scan_slots(rec, dev, obj);
    } else if (r == 0) {
        st = ((DevTest)0x1D9760)(dev) ? 8 : 10;
    } else if (r == 1 || r == 2) {
        st = 9;
    }
}

// Once per game frame: is the save screen open this frame?
void port_save_frame()
{
    g_screen_live = g_screen_seen;
    g_screen_seen = false;
}

// Rescan every storage device after the files under U: changed (netplay save exchange) and refresh an open save
// screen. Changes game memory: callers run it at the same frame on every netplay machine.
void port_save_rescan()
{
    int n = 0;
    for (BYTE *rec = *(BYTE **)0x4CAEC0; rec; rec = *(BYTE **)rec, n++) {
        ((void(__fastcall *)(void *))0x1D7C40)(rec);
        ((void(__thiscall *)(void *))0x1D8DA0)(rec);   // scan_device when SavePages is on
    }
    BYTE *self = (BYTE *)g_screen;
    bool refresh = g_screen_live && self && !self[0x14C] && !self[0x14D] && !panel_open(self, 0x1B0) &&
                   !panel_open(self, 0x1B4) && !panel_open(self, 0x1B8);
    if (refresh)
        (*(RefreshFn *)(*(BYTE **)self + 0x10))(self);
    port_log("saves: rescanned %d device(s)%s\n", n, refresh ? ", save screen refreshed" : "");
}

// Install the save screen paging when [game] SavePages is on.
void port_save_pages_init(const char *ini)
{
    if (!GetPrivateProfileIntA("game", "SavePages", 1, ini))
        return;
    struct { DWORD site, target; void *fn; } calls[] = {{0x3B20A, 0x1D7C70, (void *)slot_get},
                                                       {0x36647, 0x1D9970, (void *)slot_version},
                                                       {0x380F0, 0x36060, (void *)slot_action},
                                                       {0x3B6F4, 0x3A830, (void *)slot_save}};
    bool ok = true;
    for (auto &c : calls)
        ok = ok && *(BYTE *)c.site == 0xE8 && c.site + 5 + *(int32_t *)(c.site + 1) == c.target;
    static const BYTE scan_prologue[] = {0x6A, 0xFF, 0x68, 0xAB, 0x85, 0x29, 0x00};   // push -1; push 0x2985AB
    ok = ok && !memcmp((void *)0x1D8DA0, scan_prologue, sizeof scan_prologue);
    DWORD *vt = (DWORD *)0x33F32C;
    if (!ok || *vt != 0x3BB20 ||
        *(BYTE *)0x3B57A != 3 || *(BYTE *)0xFD582 != 3 || *(DWORD *)0xFD584 != 3 || *(BYTE *)0x36BB2 != 3) {
        port_log("saves: save screen code unexpected - paging not installed\n");
        return;
    }
    DWORD old;
    VirtualProtect(vt, 4, PAGE_EXECUTE_READWRITE, &old);
    *vt = (DWORD)save_update;
    VirtualProtect(vt, 4, old, &old);
    for (auto &c : calls)
        *(int32_t *)(c.site + 1) = (int32_t)((DWORD)c.fn - (c.site + 5));
    *(BYTE *)0x1D8DA0 = 0xE9;                           // device scan: every slot (scan_device)
    *(int32_t *)0x1D8DA1 = (int32_t)((DWORD)scan_device - (0x1D8DA0 + 5));
    *(BYTE *)0x3B57A = MAX_SAVES;                       // "only the most recent 3" warning: only past 27
    *(BYTE *)0xFD582 = MAX_SAVES, *(DWORD *)0xFD584 = MAX_SAVES;   // versus unlocks: every save
    *(BYTE *)0x36BB2 = MAX_SAVES;                       // "any save present": every save
    port_log("saves: %d per page, LB / RB (Q / W, PageUp / PageDown) flip pages - up to %d saves\n", PER_PAGE, MAX_SAVES);
}
