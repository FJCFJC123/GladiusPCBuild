// Output for the retail console commands. Retail kept the commands (their walks and side effects still run) but
// compiled out what they printed. Before / after the retail handler runs, the port prints what the command shows:
// lists read from the same game data (the May 2003 PC build's formats where it had the command), and feedback for
// commands that change things. Runs on the game thread inside the console's command execution; every data walk is
// guarded (a bad pointer prints an error instead of taking the game down).
// Addresses: see the research notes (list_commands_map.md); all retail VAs.
#include "devtools.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#define AT(T, p, off) (*(T *)((uint8_t *)(p) + (off)))

static void out(const char *fmt, ...)
{
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    con_print(OUT_TEXT, "%s", buf);
}

static void err(const char *fmt, ...)
{
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    con_print(OUT_ERROR, "%s", buf);
}

static bool ok_ptr(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v < 0x7FFF0000 && v != 0xDEADBEEF;
}

static const char *bstr(const void *rep)                // retail BString rep: text at +0xC, NULL = ""
{
    return ok_ptr(rep) ? (const char *)rep + 0xC : "";
}

static const char *safe_str(const char *s) { return ok_ptr(s) ? s : ""; }

static const char *file_name(const char *path)
{
    path = safe_str(path);
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    return b;
}

static bool icontains(const char *hay, const std::string &needle)
{
    if (needle.empty()) return true;
    std::string h = safe_str(hay), n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

static std::string arg(const std::vector<std::string> &a, size_t i) { return i < a.size() ? a[i] : std::string(); }
static bool is(const std::vector<std::string> &a, size_t i, const char *w) { return i < a.size() && !_stricmp(a[i].c_str(), w); }

// ------------------------------------------------------------------------------------------------ memory
static void cmd_mem(const std::vector<std::string> &a)
{
    if (!(AT(uint32_t, 0x41E638, 0) & 1)) { out("memory tracking list not started"); return; }
    struct T { const char *name; uint32_t mask; uint64_t bytes; uint32_t nodes; } types[40];
    int nt = 0;
    for (int i = 0; i < 39; i++) {
        const uint8_t *e = (const uint8_t *)0x37EFF0 + i * 0x10;
        if (!AT(uint32_t, e, 4)) continue;
        types[nt++] = {AT(const char *, e, 0), AT(uint32_t, e, 4), 0, 0};
    }
    bool list = is(a, 0, "list");
    std::string filt = list ? arg(a, 1) : std::string();
    int shown = 0, n = 0;
    for (uint8_t *h = *(uint8_t **)0x41E428; ok_ptr(h) && n < 2000000; h = *(uint8_t **)h, n++) {
        uint32_t type = AT(uint32_t, h, 8), size = AT(uint32_t, h, 0xC);
        for (int i = 0; i < nt; i++)
            if (type & types[i].mask) types[i].bytes += size, types[i].nodes++;
        if (list && shown < 300) {
            const char *tn = "?";
            for (int i = 0; i < nt; i++)
                if ((type & 0x7FFFFFFF) == types[i].mask) tn = safe_str(types[i].name);
            if (icontains(tn, filt)) {
                uint32_t over = (type & 0x80000000) ? AT(uint32_t, h, -8) - size : 0x20;
                out("%08X %-12s %-9u + %u", (unsigned)(uintptr_t)(h + 0x20), tn, size, over);
                shown++;
            }
        }
    }
    if (list) {
        out("%d blocks listed%s (filter: mem list <type>)", shown, shown >= 300 ? " (first 300)" : "");
        return;
    }
    for (int i = 0; i < nt; i++)
        if (types[i].nodes)
            out("%16s : %10llu (%6lluk) (nodes = %u)", safe_str(types[i].name), types[i].bytes, (types[i].bytes + 1023) >> 10,
                types[i].nodes);
    out("tracked: %u bytes in %u blocks", AT(uint32_t, 0x418BD0, 0), AT(uint32_t, 0x418BD4, 0));
}

static void cmd_heap_list(const std::vector<std::string> &a)
{
    std::string filt = arg(a, 1);
    uint64_t tl = 0, tw = 0;
    int n = 0;
    for (uint8_t *h = *(uint8_t **)0x41E40C; ok_ptr(h) && n < 4096; h = *(uint8_t **)h, n++) {
        const char *name = safe_str(AT(const char *, h, 0x70));
        uint32_t len = AT(uint32_t, h, 0x6C) * 4, used = AT(uint32_t, h, 0x74), high = AT(uint32_t, h, 0x78);
        if (!len || !icontains(name, filt)) continue;
        out("%7u:%5uk, %7u:%3u%% used, %7u:%3u%% high, %4uk waste  %s", len, (len + 1023) >> 10, used,
            (unsigned)(used * 100ull / len), high, (unsigned)(high * 100ull / len), (len - high + 1023) >> 10, name);
        tl += len, tw += len - high;
    }
    out("Total use: %lluk, waste: %lluk", (tl + 1023) >> 10, (tw + 1023) >> 10);
}

static void cmd_heap_history()
{
    int idx = AT(int, 0x418BE0, 0);
    int n = 0;
    for (uint8_t *h = *(uint8_t **)0x41E40C; ok_ptr(h) && n < 4096; h = *(uint8_t **)h, n++) {
        char line[256];
        int o = _snprintf(line, sizeof line, "%-24s", safe_str(AT(const char *, h, 0x70)));
        for (int k = 0; k < 6 && o > 0; k++)
            o += _snprintf(line + o, sizeof line - o, " %8u", AT(uint32_t, h, 0x4C + ((idx + k) % 6) * 4));
        out("%s", line);
    }
    out("(snapshots oldest -> newest; 'heap snapshot' takes one)");
}

// ------------------------------------------------------------------------------------------------ loaded data
static void cmd_panlist()
{
    int n = 0;
    for (uint8_t *p = *(uint8_t **)0x4B1F00; ok_ptr(p) && n < 100000; p = *(uint8_t **)p, n++)
        out("%-48s %d", file_name(AT(const char *, p, 0x7C)), AT(int, p, 0x78));
    out("%d animation sets loaded", n);
}

static int g_pax_n;
static bool __cdecl pax_cb(void *pax)
{
    out("%-48s %u", file_name(AT(const char *, pax, 0xC)), AT(uint16_t, pax, 0x8A));
    g_pax_n++;
    return true;
}

static void cmd_paxlist()
{
    g_pax_n = 0;
    ((unsigned(__thiscall *)(void *, bool(__cdecl *)(void *)))0x1DB100)((void *)0x4CAFF8, pax_cb);
    out("%d PAX files loaded", g_pax_n);
}

static void cmd_fx()
{
    static const struct { const char *title; DWORD head; } k[] = {
        {"Emitters", 0x4B2D28}, {"Sprites", 0x4E10E4}, {"Rings", 0x50ABC0}, {"Particles", 0x5095C4}};
    for (auto &t : k) {
        out("**** %s ****", t.title);
        int i = 0;
        for (uint8_t *p = *(uint8_t **)t.head; ok_ptr(p) && i < 20000; p = AT(uint8_t *, p, 4), i++)
            out("%d\t(%3d) %s", i, AT(int, p, 0x14), safe_str(AT(const char *, p, 0x18)));
    }
}

// ------------------------------------------------------------------------------------------------ events / story
static void cmd_event_handlers(const std::vector<std::string> &a)
{
    std::string filt = arg(a, 0);
    int n = 0, shown = 0;
    for (uint8_t *h = *(uint8_t **)0x38E7AC; ok_ptr(h) && n < 100000; h = AT(uint8_t *, h, 4), n++) {
        const char *ev = bstr(AT(void *, h, 0xC));
        if (!icontains(ev, filt)) continue;
        shown++;
        if (AT(int, h, 0x24)) { out("%32s : (Internal)", ev); continue; }
        const char *file = "";
        uint8_t *obj = AT(uint8_t *, h, 0x10);
        uint8_t *s = ok_ptr(obj) ? AT(uint8_t *, obj, 8) : nullptr;
        if (ok_ptr(s)) {
            uint8_t *f = AT(int16_t, s, 0x1BE) > 0 && ok_ptr(AT(void *, s, 0xF4)) ? AT(uint8_t *, s, 0xF4) : AT(uint8_t *, s, 0x80);
            if (ok_ptr(f)) file = file_name(bstr(AT(void *, f, 0x10)));
        }
        out("%32s : (Script) %s %s", ev, file, bstr(AT(void *, h, 0x14)));
    }
    out("%d event handlers%s", shown, filt.empty() ? "" : " matching");
}

static int count_handlers(const std::string &event)
{
    int n = 0, c = 0;
    for (uint8_t *h = *(uint8_t **)0x38E7AC; ok_ptr(h) && n < 100000; h = AT(uint8_t *, h, 4), n++)
        if (!_stricmp(bstr(AT(void *, h, 0xC)), event.c_str())) c++;
    return c;
}

static std::string gvar_value(const uint8_t *gv, const char **type)
{
    char b[64];
    switch (AT(int, gv, 8)) {
    case 1: *type = "Int"; sprintf(b, "%d", AT(int, gv, 0xC)); return b;
    case 2: *type = "Float"; sprintf(b, "%g", AT(float, gv, 0xC)); return b;
    case 3: *type = "String"; return safe_str(AT(const char *, gv, 0xC));
    default: *type = "Unknown"; return "?";
    }
}

static void cmd_gvar_list(const std::string &filt)
{
    std::vector<std::pair<std::string, std::string>> rows;
    for (int b = 0; b < 64; b++) {
        int n = 0;
        for (uint8_t *nd = *(uint8_t **)(0x38EA88 + b * 8); ok_ptr(nd) && n < 100000; nd = *(uint8_t **)nd, n++) {
            const char *name = safe_str(AT(const char *, nd, 8));
            uint8_t *gv = AT(uint8_t *, nd, 0x10);
            if (!ok_ptr(gv) || !icontains(name, filt)) continue;
            const char *type;
            std::string v = gvar_value(gv, &type);
            rows.push_back({name, v + "\t(" + type + ")"});
        }
    }
    std::sort(rows.begin(), rows.end(), [](auto &x, auto &y) { return _stricmp(x.first.c_str(), y.first.c_str()) < 0; });
    for (auto &r : rows) out("%-32s %s", r.first.c_str(), r.second.c_str());
    out("%d story variables%s", (int)rows.size(), filt.empty() ? "" : " matching");
}

static const uint8_t *gvar_find(const std::string &name)
{
    return ((const uint8_t *(__thiscall *)(void *, const char *))0x25870)((void *)0x38EA88, name.c_str());
}

static void cmd_protiushelp_list()
{
    uint8_t *items = *(uint8_t **)0x394064;
    uint32_t cnt = *(uint32_t *)0x394068;
    if (!ok_ptr(items)) { out("help not loaded"); return; }
    for (uint32_t i = 0; i < cnt && i < 10000; i++) {
        uint8_t *it = items + i * 0xC;
        char key[32];
        sprintf(key, "Help_%u", i);
        const uint8_t *gv = gvar_find(key);
        bool seen = ok_ptr(gv) && AT(int, gv, 8) == 1 && AT(int, gv, 0xC) == 1;
        out("%3u %-28s %s", i, bstr(AT(void *, it, 0)), seen ? "(seen)" : "");
    }
    out("protiushelp <number|name> shows one (marks it seen in the save)");
}

// ------------------------------------------------------------------------------------------------ world map
static const char *region_name()
{
    static const char *const k[] = {"Expanse", "Imperia", "Nordagh", "Steppes"};
    int r = AT(int, 0x394DD8, 0x50);
    return r >= 0 && r < 4 ? k[r] : "Error";
}

static uint8_t *world_map() { return *(uint8_t **)0x417D6C; }

static uint8_t *find_town(const std::string &name)
{
    int n = 0;
    for (uint8_t *t = *(uint8_t **)0x410DA0; ok_ptr(t) && n < 10000; t = *(uint8_t **)t, n++)
        if (!_stricmp(bstr(AT(void *, t, 0x20)), name.c_str())) return t;
    return nullptr;
}

static uint8_t *find_prop(const std::string &name, int *index)
{
    uint8_t *wm = world_map();
    int i = 1;
    for (uint8_t *p = ok_ptr(wm) ? AT(uint8_t *, wm, 0xAC) : nullptr; ok_ptr(p) && i < 100000; p = AT(uint8_t *, p, 0x10), i++) {
        uint8_t *d = AT(uint8_t *, p, 0x1C);
        if (ok_ptr(d) && !_stricmp(bstr(AT(void *, d, 0x14)), name.c_str())) { if (index) *index = i; return p; }
    }
    return nullptr;
}

// Retail wmWarp warps to the world map prop named exactly like the town (0xF6DC0 -> FindProp 0xF4010) and silently
// does nothing without one. Some towns have none: Imperia Shore is reached through the beach props shore01..03 that run
// Script\wmImperiaShore. For those the port warps to a prop it can find under a near name instead.
static std::string squash(const std::string &s)
{
    std::string r;
    for (char c : s) if (c != ' ' && c != '_') r += (char)tolower((unsigned char)c);
    return r;
}

static std::string town_prop(const std::string &town)
{
    if (find_prop(town, nullptr)) return town;
    static const char *const k_alias[][2] = {{"imperiashore", "shore03"}};
    std::string sq = squash(town);
    for (auto &a : k_alias)
        if (sq == a[0] && find_prop(a[1], nullptr)) return a[1];
    for (const char *suffix : {"", "arena", "town"})
        if (find_prop(sq + suffix, nullptr)) return sq + suffix;
    return std::string();
}

static void cmd_wmwarp_list()
{
    const char *cur = region_name();
    int n = 0, shown = 0;
    for (uint8_t *t = *(uint8_t **)0x410DA0; ok_ptr(t) && n < 10000; t = *(uint8_t **)t, n++)
        if (!_stricmp(bstr(AT(void *, t, 0x10)), cur)) {
            std::string name = bstr(AT(void *, t, 0x20));
            std::string prop = world_map() ? town_prop(name) : name;
            if (prop.empty()) out("%-24s (no world map prop - can't warp)", name.c_str());
            else if (_stricmp(prop.c_str(), name.c_str())) out("%-24s (via prop %s)", name.c_str(), prop.c_str());
            else out("%s", name.c_str());
            shown++;
        }
    out("%d towns in %s%s", shown, cur, world_map() ? "" : " (warping needs the world map)");
}

// returns false when the retail handler must not run (it would crash or do nothing)
static bool pre_wmwarp(const std::vector<std::string> &a)
{
    uint8_t *t = find_town(a[0]);
    if (!t) { err("Couldn't find town %s (wmWarp list shows them)", a[0].c_str()); return false; }
    if (_stricmp(bstr(AT(void *, t, 0x10)), region_name())) { err("That town is not in %s.", region_name()); return false; }
    if (!world_map()) { err("Not on the world map."); return false; }
    std::string town = bstr(AT(void *, t, 0x20));
    std::string prop = town_prop(town);
    if (prop.empty()) { err("%s has no world map prop to warp to (try wmProp list)", town.c_str()); return false; }
    out("Ok. Warping to %s", town.c_str());
    if (!_stricmp(prop.c_str(), town.c_str())) return true;
    // no prop under the town's own name: do what the retail handler does (0xF6F66..0xF6F8C) with the stand-in prop
    float pos[4] = {};
    ((void(__thiscall *)(void *, float *))0x4DF30)(find_prop(prop, nullptr), pos);
    ((void(__thiscall *)(void *, float *))0x1B010)(AT(void *, world_map(), 0xA4), pos);
    return false;
}

static void cmd_wmprop_list(const std::string &filt)
{
    uint8_t *wm = world_map();
    if (!wm) { out("Not on the world map."); return; }
    int i = 1, shown = 0;
    for (uint8_t *p = AT(uint8_t *, wm, 0xAC); ok_ptr(p) && i < 100000; p = AT(uint8_t *, p, 0x10), i++) {
        uint8_t *d = AT(uint8_t *, p, 0x1C);
        const char *name = ok_ptr(d) ? bstr(AT(void *, d, 0x14)) : "";
        if (!icontains(name, filt)) continue;
        const float *pos = ok_ptr(d) ? (const float *)(d + 0x20) : nullptr;
        if (pos) out("%4d: %-32s (%.1f, %.1f, %.1f)", i, name, pos[0], pos[1], pos[2]);
        else out("%4d: %s", i, name);
        shown++;
    }
    out("%d props (wmProp warpnum, <number> / wmProp warp, <name>)", shown);
}

static void cmd_wmentity_list(const std::string &filt)
{
    int n = 0, shown = 0;
    for (uint8_t *nd = *(uint8_t **)0x38C5A4; ok_ptr(nd) && n < 100000; nd = *(uint8_t **)nd, n++) {
        uint8_t *e = AT(uint8_t *, nd, 8);
        if (!ok_ptr(e) || !ok_ptr(*(void **)e)) continue;
        uint8_t *node = (*(uint8_t *(__thiscall **)(void *))(*(uint8_t **)e + 0x24))(e);
        if (!ok_ptr(node)) continue;
        const char *name = safe_str(AT(const char *, node, 0x20));
        if (!*name || !icontains(name, filt)) continue;
        const float *pos = (const float *)(node + 0x4C);
        out("%-32s (%.1f, %.1f, %.1f)", name, pos[0], pos[1], pos[2]);
        shown++;
    }
    out("%d entities", shown);
}

// ------------------------------------------------------------------------------------------------ school
static uint8_t *school() { return *(uint8_t **)0x3EE8B0; }

static int roster_count()
{
    uint8_t *s = school();
    int n = 0;
    for (uint8_t *c = ok_ptr(s) ? AT(uint8_t *, s, 0x20) : nullptr; ok_ptr(c) && n < 1000; c = AT(uint8_t *, c, 0x10)) n++;
    return n;
}

static bool roster_has(const std::string &name)
{
    uint8_t *s = school();
    int n = 0;
    for (uint8_t *c = ok_ptr(s) ? AT(uint8_t *, s, 0x20) : nullptr; ok_ptr(c) && n++ < 1000; c = AT(uint8_t *, c, 0x10))
        if (!_stricmp(safe_str(AT(const char *, c, 0x18)), name.c_str())) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------ dispatch
struct PreState { int roster; };

static bool pre(const std::string &cmd, const std::vector<std::string> &a, PreState &st)
{
    const char *c = cmd.c_str();
    bool q = is(a, 0, "?");
    if (q) return true;                                 // usage: retail prints it through the hooked ShowUsage
    // handlers the release build emptied out (stubs, or their effect is never read)
    static const char *const k_stub[] = {"scene", "camConfig", "LogLeague", "LogEncounter", "ScriptCalls", "sys",
                                         "dump", "font", "files", "UnitInfo", "exit", "history"};
    for (const char *s : k_stub)
        if (!_stricmp(c, s)) { err("%s does nothing in the release build (its code was stripped)", c); return false; }
    if (!_stricmp(c, "mem")) { if (a.empty() || is(a, 0, "list") || is(a, 0, "used")) { cmd_mem(a); return false; } }
    else if (!_stricmp(c, "mempool")) { out("no memory pools registered"); return false; }
    else if (!_stricmp(c, "heap")) {
        if (is(a, 0, "list")) { cmd_heap_list(a); return false; }
        if (is(a, 0, "history")) { cmd_heap_history(); return false; }
    } else if (!_stricmp(c, "panlist")) { if (a.empty()) { cmd_panlist(); return false; } }
    else if (!_stricmp(c, "paxlist")) { if (a.empty()) { cmd_paxlist(); return false; } }
    else if (!_stricmp(c, "fx")) { cmd_fx(); return false; }
    else if (!_stricmp(c, "EventHandlers")) { cmd_event_handlers(a); return false; }
    else if (!_stricmp(c, "EventFire")) {
        if (!a.empty()) {
            int n = count_handlers(a[0]);
            if (n) out("firing %s%s%s -> %d handler%s", a[0].c_str(), a.size() > 1 ? " " : "", arg(a, 1).c_str(), n, n == 1 ? "" : "s");
            else err("no handler for event %s (EventHandlers lists them)", a[0].c_str());
        }
    } else if (!_stricmp(c, "gvar")) {
        if (is(a, 0, "list")) { cmd_gvar_list(arg(a, 1)); return false; }
        if (!a.empty() && !is(a, 0, "set") && !is(a, 0, "add")) { cmd_gvar_list(a[0]); return false; }   // retail reads argv[1] here
        if (is(a, 0, "set") && a.size() == 3 && !gvar_find(a[1])) { err("can't find %s", a[1].c_str()); return false; }
    } else if (!_stricmp(c, "protiushelp")) { if (is(a, 0, "list")) { cmd_protiushelp_list(); return false; } }
    else if (!_stricmp(c, "wmWarp")) {
        if (is(a, 0, "list")) { cmd_wmwarp_list(); return false; }
        if (a.size() == 1) return pre_wmwarp(a);
    } else if (!_stricmp(c, "wmProp")) {
        if (is(a, 0, "list")) { cmd_wmprop_list(arg(a, 1)); return false; }
        if (!world_map()) { err("Not on the world map."); return false; }
        if (is(a, 0, "warp") && a.size() == 2) {
            if (!find_prop(a[1], nullptr)) { err("Couldn't find prop %s", a[1].c_str()); return false; }
            out("Ok. Warping to %s", a[1].c_str());
        }
        if (is(a, 0, "warpnum") && a.size() == 2) out("Ok. Warping to prop %s", a[1].c_str());
    } else if (!_stricmp(c, "wmEntity")) {
        if (is(a, 0, "list")) { cmd_wmentity_list(arg(a, 1)); return false; }
    } else if (!_stricmp(c, "CutSceneParse")) {
        err("CutSceneParse is disabled: it writes generated .cut files into the game data folder");
        return false;
    } else if (!_stricmp(c, "ForceTime") || !_stricmp(c, "ForceWeather")) {
        static const char *const times[] = {"noon", "night", "duskdawn", "none"};
        static const char *const weathers[] = {"clear", "cloudheavy", "cloudmed", "cloudlight", "none"};
        bool time = !_stricmp(c, "ForceTime");
        if (a.size() == 1) {
            bool valid = false;
            for (const char *v : time ? std::vector<const char *>(times, times + 4) : std::vector<const char *>(weathers, weathers + 5))
                if (!_stricmp(v, a[0].c_str())) valid = true;
            if (!valid) { err("%s: unknown value '%s' (the scene would just load without it)", c, a[0].c_str()); return false; }
        }
    } else if (!_stricmp(c, "StoryEvent")) {
        if (is(a, 0, "set") && a.size() < 2) { err("StoryEvent set,<name>"); return false; }   // retail reads argv[1] unchecked
        if (is(a, 0, "list")) { err("StoryEvent list: the listing was compiled out of the release build"); return false; }
    } else if (!_stricmp(c, "shake")) {
        if (a.size() < 2) { err("shake <amplitude>,<duration>"); return false; }
        if (!AT(int, 0x4B2814, 0)) out("note: shake only moves the god camera (godcamera=1 to see it)");
    } else if (!_stricmp(c, "camDarkGodTarget")) {
        if (a.empty()) { err("camDarkGodTarget <unit>"); return false; }
    } else if (!_stricmp(c, "key")) {
        if (a.size() >= 2) out("note: the release build never checks key binds, so this bind won't fire");
    } else if (!_stricmp(c, "exec")) {
        out("note: exec runs each line through a printf-style call - lines containing %% can crash");
    } else if (!_stricmp(c, "school")) {
        if (!school()) { err("no school loaded"); return false; }
        st.roster = roster_count();
        if ((is(a, 0, "GIVEXP") || is(a, 0, "GIVEJP")) && a.size() >= 3 && !roster_has(a[2])) {
            err("no gladiator named %s in the school", a[2].c_str());
            return false;
        }
    } else if (!_stricmp(c, "CursorRadius")) {
        uint8_t *k = *(uint8_t **)0x3D2970;
        uint8_t *u = ok_ptr(k) ? AT(uint8_t *, k, 0x412C) : nullptr;
        if (!ok_ptr(u)) { err("CursorRadius: put the battle cursor on a unit first"); return false; }
        if (a.empty()) { out("Current Cursor Radius = %f", AT(float, u, 0x428)); return false; }
    }
    return true;
}

static void post(const std::string &cmd, const std::vector<std::string> &a, const PreState &st)
{
    const char *c = cmd.c_str();
    if (is(a, 0, "?")) return;
    if (!_stricmp(c, "heap")) {
        if (is(a, 0, "track")) out("Tracking switched %s (lines in Log > Game output)", AT(uint8_t, 0x418BC4, 0) ? "on" : "off");
        else if (is(a, 0, "snapshot")) out("Snapshot %d taken ('heap history' shows them)", (AT(int, 0x418BE0, 0) + 5) % 6);
        else if (is(a, 0, "debug")) {
            uint8_t *h = *(uint8_t **)0x418BC8;
            if (a.size() == 2) {
                if (ok_ptr(h)) out("Dumping heap %s", safe_str(AT(const char *, h, 0x70)));
                else err("Can't find heap %s", a[1].c_str());
            } else out("Switched off debug dump");
        }
    } else if (!_stricmp(c, "gvar")) {
        if ((is(a, 0, "set") && a.size() == 3) || (is(a, 0, "add") && a.size() == 2)) {
            const uint8_t *gv = gvar_find(a[1]);
            const char *type = "";
            if (gv) {
                std::string v = gvar_value(gv, &type);
                out("%s = %s (%s)", a[1].c_str(), v.c_str(), type);
            }
        }
    } else if (!_stricmp(c, "input")) {
        if (is(a, 0, "enable") || is(a, 0, "disable"))
            out("pad input %s%s", AT(uint32_t, 0x3AD670, 0) ? "enabled" : "DISABLED",
                AT(uint32_t, 0x3AD670, 0) ? "" : " - 'input enable' turns it back on");
        else if (is(a, 0, "show")) out("pad monitor %s (it prints to the Game output log every frame)",AT(uint8_t, 0x3AD758, 0) ? "on" : "off");
    } else if (!_stricmp(c, "ForceTime") || !_stricmp(c, "ForceWeather")) {
        const void *rep = *(void **)(!_stricmp(c, "ForceTime") ? 0x394DFC : 0x394E00);
        // read by the scene-name builder 0x433C0 (battle loads and story scenes only); TheGame::Serialize 0x42F50
        // copies gvar wmCurrentWeather over the forced weather on every save/load
        out("%s = %s (applies at the next battle or story scene, not towns or the world map%s)", c, *bstr(rep) ? bstr(rep) : "none",
            !_stricmp(c, "ForceWeather") ? "; saving or loading resets it" : "");
    } else if (!_stricmp(c, "school") && school()) {
        uint8_t *s = school();
        if (is(a, 0, "WEALTH")) out("dinars now %d", AT(int, s, 0x1C));
        else if (is(a, 0, "ALLITEMS")) out(a.size() >= 3 ? "items for %s / %s added" : "all items added", arg(a, 1).c_str(), arg(a, 2).c_str());
        else if (is(a, 0, "NOITEMS")) out("all items removed");
        else if (is(a, 0, "ADDUNIT")) out(roster_count() > st.roster ? "unit added to roster" : "unit NOT added (check the class name)");
        else if (is(a, 0, "GIVEXP") || is(a, 0, "GIVEJP"))
            out("%s %s given to %s", arg(a, 1).c_str(), is(a, 0, "GIVEXP") ? "xp" : "jp", a.size() >= 3 ? a[2].c_str() : "every gladiator");
        else if (is(a, 0, "CHUMP") || is(a, 0, "MUNIO")) out("school level override now %d", AT(int, 0x418458, 0));
    } else if (!_stricmp(c, "calc")) {
        for (size_t i = 0; i < a.size() && i < 16; i++) out("%s = %g", a[i].c_str(), AT(float, 0x480C18, i * 4));   // fargs
    } else if (!_stricmp(c, "echo")) {
        std::string s;
        for (auto &x : a) s += (s.empty() ? "" : " ") + x;
        out("%s", s.c_str());
    } else if (!_stricmp(c, "activateconsole")) {
        out("console flag %s (retail: 0 = active, which pauses the world map script)", AT(uint8_t, 0x481CE0, 0) ? "active" : "off");
    } else if (!_stricmp(c, "CursorRadius")) {
        if (is(a, 0, "save")) out("not saved: release stubbed pax_Config::SaveAll (0x1E4E00); the radius lasts this session");
        else {
            uint8_t *k = *(uint8_t **)0x3D2970;
            uint8_t *u = ok_ptr(k) ? AT(uint8_t *, k, 0x412C) : nullptr;
            if (ok_ptr(u)) out("Cursor Radius = %f", AT(float, u, 0x428));
        }
    }
}

// ------------------------------------------------------------------------------------------------ guarded entry
struct Call { const std::string *cmd; const std::vector<std::string> *args; PreState *st; bool result; };

static void do_pre(Call *c) { c->result = pre(*c->cmd, *c->args, *c->st); }
static void do_post(Call *c) { post(*c->cmd, *c->args, *c->st); }

static bool guarded(void (*fn)(Call *), Call *c)
{
    __try {
        fn(c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static PreState g_state;

// Before the retail handler: false = do not run it (the port printed everything / it would fail)
bool cmdout_pre(const std::string &cmd, const std::vector<std::string> &args)
{
    Call c = {&cmd, &args, &g_state, true};
    g_state = PreState();
    if (!guarded(do_pre, &c)) {
        con_print(OUT_ERROR, "%s: reading the game data failed (not loaded here?)", cmd.c_str());
        return false;
    }
    return c.result;
}

void cmdout_post(const std::string &cmd, const std::vector<std::string> &args)
{
    Call c = {&cmd, &args, &g_state, true};
    if (!guarded(do_post, &c)) con_print(OUT_ERROR, "%s: reading the result failed", cmd.c_str());
}
