// Collision painter (gladius_editor.exe, Level page): edit the scene's floor collision (.pms) live - raise / lower /
// flatten / smooth it with a brush, switch floor triangles off and on, paint their surface category - and save the
// level's loose .pms.
//
// The engine (loader 0x1D2680) keeps the .pms file buffer whole: the scene's +0xC points at its 0x44-byte header (grid
// size +8 / +0xC, bounds +0x28, 1 / cell size +0x40 - rewritten by the loader) and globals point into the rest:
// vertices 0x380670 (count 0x380668), normals 0x38067C (count 0x380674), triangles 0x4CAB8C, cell records 0x4CAB90,
// cell lists 0x4CAB94, cell table 0x4CAB98. The height check (0x1D38D0) and the other queries read only those, so an
// edit goes live by rebuilding the file into our own buffer, copying its header over the engine's and pointing the
// globals at our arrays. The unload (0x1D1CD0) frees the globals only when 0x4CAB9C is set (never, for a file load),
// so our buffer stays ours; it is rewritten in place (poly pointers handed out by the height check stay readable).
// The loader also gives category-less triangles every category (flags | 0xF000); the live copy does the same.
// File layout: tools/pms.py.
#include "port.h"
#include <string>
#include <vector>
#include <map>
#include <array>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include "overlay.h"
#include "editor.h"

bool port_read_game_file(const std::string &key, std::string &out);   // bec_read.cpp
void port_mods_rescan();                                               // mods.cpp

enum { T_RAISE, T_LOWER, T_FLATTEN, T_SMOOTH, T_FLOOR_OFF, T_FLOOR_ON, T_CATEGORY, NTOOLS };
static const char *const k_tools[NTOOLS] = {"Raise", "Lower", "Flatten (to the clicked height)", "Smooth",
                                            "Floor off (not walkable)", "Floor on (walkable)", "Surface category"};
static const char *const k_guide_heads[NTOOLS] = {"Raise", "Lower", "Flatten", "Smooth", "Floor off (not walkable)",
                                                  "Floor on (walkable)", "Surface category"};
// the category is 4 flag bits (the world maps use 1 / 2 / 4 / 8 and a few mixes; arenas 1). Meanings as observed on
// the world map (user): 1 morning search / loot strip, 2 road, 4 off-road ground, 8 unknown, 4+2 NPC spawn areas
static const char *const k_cat_names[16] = {"0  every category", "1  search spot (loot)", "2  road",
    "3  road + search", "4  off-road ground", "5  off-road + search", "6  NPC spawn (road + off-road)",
    "7  road + off-road + search", "8  unknown (darker blue)", "9  8 + search", "10  8 + road", "11  8 + road + search",
    "12  8 + off-road", "13  8 + off-road + search", "14  8 + road + off-road", "15  all four"};
static int g_pick_tri = -1;                             // the triangle the last pick hit
static const float k_radii[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
static const uint32_t k_cat_colors[16] = {0xFFB0B0B0, 0xFF40FF40, 0xFF40C0FF, 0xFFFFC040, 0xFFFF60FF, 0xFF60FFFF,
                                          0xFFFFFF60, 0xFFC08040, 0xFF8080FF, 0xFFFF8080, 0xFF80FF80, 0xFF4080C0,
                                          0xFFC0C040, 0xFFC040C0, 0xFF40C0C0, 0xFFFFFFFF};

static volatile bool g_cpaint;                          // Paint collision on
static volatile int g_tool, g_radius = 2, g_cat = 1;    // tool, radius index, category 0..15
static volatile bool g_undo_req, g_revert_req;
static HWND g_cstatus;
static CRITICAL_SECTION g_ccs;                          // the model (game thread edits, editor thread saves)

struct CTri {
    uint16_t v[4];                                      // v0, v1, v2, v2 alt (flags bit 0: either winding)
    uint16_t flags;
    float nsign;                                        // the file normal's z sign (kept through rebuilds)
};
static struct {
    bool loaded, dirty, live_dirty;
    std::string key;                                    // "data/levels/.../x_pms.pms"
    uint8_t *header;                                    // the engine's (scene +0xC)
    float cell;
    std::vector<std::array<float, 3>> V;
    std::vector<CTri> T;
    std::vector<std::vector<int>> adj;                  // vertex neighbours (smooth)
    std::string tail;                                   // the names after the cell table
    float target;                                       // flatten height
} C;
static std::vector<std::pair<std::vector<std::array<float, 3>>, std::vector<uint16_t>>> g_undo;
static uint8_t *g_live;                                 // our live buffer
static size_t g_live_cap;
static DWORD g_live_at;

static struct {                                         // input (game window thread -> game thread)
    bool press, down, valid;
    float nx, ny;
    bool alt;                                           // Ctrl: the opposite tool
} g_cin;

static void cstatus(const char *fmt, ...)
{
    char b[300];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    b[sizeof b - 1] = 0;
    port_log("coll: %s\n", b);
    static char ring[8][300];                           // posted, not sent: the editor thread may be waiting on us
    static LONG slot;
    char *dst = ring[InterlockedIncrement(&slot) & 7];
    memcpy(dst, b, sizeof b);
    if (g_cstatus) PostMessageA(g_cstatus, WM_SETTEXT, 0, (LPARAM)dst);
}

static int game_state()
{
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    return state ? *(const int *)(state + 8) : -1;
}

static uint8_t *scene_ptr()
{
    int id = game_state();
    return id == 7 ? *(uint8_t **)0x417D64 : id == 12 ? *(uint8_t **)0x39D320 : nullptr;
}

static std::string lower_key(std::string p)            // "$l\x\y" / "x\y" -> "x/y" lower case, no doubled slashes
{
    for (auto &c : p) c = c == (char)0x5C ? '/' : (char)tolower((unsigned char)c);
    std::string out;
    for (char c : p) if (!(c == '/' && !out.empty() && out.back() == '/')) out += c;
    return out;
}

static std::string field(const std::string &text, const char *name)   // "NAME: value" -> value (quotes stripped)
{
    size_t p = text.find(name);
    if (p == std::string::npos) return "";
    p += strlen(name);
    size_t e = text.find_first_of("\r\n", p);
    std::string v = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
    size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
    return a == std::string::npos ? "" : v.substr(a, b - a + 1);
}

// the scene file -> its PHYSICS line -> the .pms beside it
static std::string find_pms_key()
{
    std::string scn;
    if (game_state() == 7) {                            // four world maps: the one whose .pms matches the live header
        uint8_t *scene = scene_ptr();
        const uint8_t *hdr = scene ? *(uint8_t **)(scene + 0xC) : nullptr;
        for (const char *m : {"imperia", "nordagh", "southern", "steppes"}) {
            std::string key = std::string("data/levels/worldmap/") + m + "_worldmap_pms.pms", d;
            if (hdr && port_read_game_file(key, d) && d.size() >= 0x44 && !memcmp(&d[0x1C], hdr + 0x1C, 4) &&
                !memcmp(&d[0x14], hdr + 0x14, 4))
                return key;
        }
        return "";
    } else {
        std::string enc = ed_current_enc(), text;
        if (enc.empty() || !port_read_game_file(enc, text)) return "";
        std::string s = field(text, "SCENE:");
        if (s.empty()) return "";
        scn = "data/levels/" + lower_key(s);
    }
    std::string text;
    if (!port_read_game_file(scn, text)) return "";
    std::string phys = field(text, "PHYSICS:");
    if (phys.empty()) return "";
    return scn.substr(0, scn.find_last_of('/') + 1) + lower_key(phys) + ".pms";
}

static bool load_pms()                                  // game thread
{
    C.loaded = false;
    C.V.clear(), C.T.clear(), C.adj.clear(), g_undo.clear();
    uint8_t *scene = scene_ptr();
    uint8_t *hdr = scene ? *(uint8_t **)(scene + 0xC) : nullptr;
    if (!hdr) { cstatus("no collision in this scene - the world map or a battle"); return false; }
    std::string key = find_pms_key(), d;
    if (key.empty() || !port_read_game_file(key, d) || d.size() < 0x44 || memcmp(d.data(), "PMS2", 4)) {
        cstatus("could not find this scene's .pms (%s)", key.empty() ? "no PHYSICS line" : key.c_str());
        return false;
    }
    uint32_t h[10];
    memcpy(h, d.data(), 40);
    uint32_t nx = h[2], ny = h[3], nv = h[5], nn = h[6], nt = h[7], nc = h[8], nl = h[9];
    size_t o = 0x44, need = o + 12 * nv + 12 * nn + 16 * nt + 32 * nc + 2 * nl + 2 * nx * ny;
    if (need > d.size()) { cstatus("%s is truncated", key.c_str()); return false; }
    memcpy(&C.cell, &d[0x10], 4);
    C.V.resize(nv);
    for (uint32_t i = 0; i < nv; i++) memcpy(C.V[i].data(), &d[o + 12 * i], 12);
    const float *N = (const float *)&d[o + 12 * nv];
    const uint8_t *tp = (const uint8_t *)&d[o + 12 * nv + 12 * nn];
    C.T.resize(nt);
    for (uint32_t i = 0; i < nt; i++) {
        uint16_t r[8];
        memcpy(r, tp + 16 * i, 16);
        CTri &t = C.T[i];
        memcpy(t.v, r, 8);
        t.flags = r[7];
        t.nsign = r[4] < nn && N[3 * r[4] + 2] < 0 ? -1.0f : 1.0f;
    }
    C.tail = d.substr(need);
    C.adj.assign(nv, {});
    for (auto &t : C.T)
        for (int k = 0; k < 3; k++) {
            int a = t.v[k], b = t.v[(k + 1) % 3];
            if (a < (int)nv && b < (int)nv) C.adj[a].push_back(b), C.adj[b].push_back(a);
        }
    C.key = key;
    C.header = hdr;
    C.dirty = C.live_dirty = false;
    C.loaded = true;
    cstatus("%s: %u triangles - click / drag in the game window (Ctrl = the opposite tool)", key.c_str(), nt);
    return true;
}

// the model -> PMS2 bytes (the file; live = the loader's category fix-up applied)
static bool build(std::string &out, bool live)
{
    std::vector<std::array<float, 3>> V;
    std::vector<int> remap(C.V.size(), -1);
    std::map<std::array<uint32_t, 3>, int> extra;       // bounds vertices by position
    auto bits = [](const std::array<float, 3> &p) {
        std::array<uint32_t, 3> k;
        memcpy(k.data(), p.data(), 12);
        return k;
    };
    auto use = [&](int i) {
        if (remap[i] < 0) remap[i] = (int)V.size(), V.push_back(C.V[i]);
        return remap[i];
    };
    auto vert = [&](const std::array<float, 3> &p) {
        auto k = bits(p);
        auto it = extra.find(k);
        if (it != extra.end()) return it->second;
        int i = (int)V.size();
        V.push_back(p);
        extra[k] = i;
        return i;
    };
    struct R { uint16_t r[8]; float lo[3], hi[3]; };
    std::vector<R> recs;
    std::vector<std::array<float, 3>> Nrm;
    std::map<std::array<int, 3>, int> nidx;
    for (auto &t : C.T) {
        bool alt = (t.flags & 1) != 0;                  // v2 alt only used with flag bit 0 (retail: 0xDEAD otherwise)
        int nc4 = alt ? 4 : 3;
        bool bad = false;
        for (int k = 0; k < nc4; k++) bad |= t.v[k] >= C.V.size();
        if (bad) continue;
        R r{};
        for (int k = 0; k < nc4; k++) r.r[k] = (uint16_t)use(t.v[k]);
        if (!alt) r.r[3] = t.v[3];
        const float *a = C.V[t.v[0]].data(), *b = C.V[t.v[1]].data(), *c = C.V[t.v[2]].data();
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float ln = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (ln < 1e-9f) n[0] = n[1] = 0, n[2] = 1, ln = 1;
        float s = ((n[2] < 0) != (t.nsign < 0)) ? -1.0f : 1.0f;   // keep the file's facing
        for (int k = 0; k < 3; k++) n[k] = n[k] / ln * s;
        std::array<int, 3> nk = {(int)lrintf(n[0] * 1e6f), (int)lrintf(n[1] * 1e6f), (int)lrintf(n[2] * 1e6f)};
        auto it = nidx.find(nk);
        if (it == nidx.end()) it = nidx.emplace(nk, (int)Nrm.size()).first, Nrm.push_back({n[0], n[1], n[2]});
        r.r[4] = (uint16_t)it->second;
        for (int q = 0; q < 3; q++) {
            r.lo[q] = r.hi[q] = a[q];
            for (int k = 1; k < nc4; k++) {
                float z = C.V[t.v[k]][q];
                r.lo[q] = (std::min)(r.lo[q], z), r.hi[q] = (std::max)(r.hi[q], z);
            }
        }
        uint16_t f = t.flags;
        if (live && !(f & 0xF000)) f |= 0xF000;
        r.r[7] = f;
        recs.push_back(r);
    }
    for (auto &r : recs) {
        r.r[5] = (uint16_t)vert({r.lo[0], r.lo[1], r.lo[2]});
        r.r[6] = (uint16_t)vert({r.hi[0], r.hi[1], r.hi[2]});
    }
    if (recs.empty()) return false;
    float bmin[3], bmax[3];
    for (int q = 0; q < 3; q++) {
        bmin[q] = 1e30f, bmax[q] = -1e30f;
        for (auto &v : V) bmin[q] = (std::min)(bmin[q], v[q]), bmax[q] = (std::max)(bmax[q], v[q]);
    }
    float cell = C.cell > 0 ? C.cell : 4.0f;
    int nx = (std::max)(1, (int)ceilf((bmax[0] - bmin[0]) / cell)), ny = (std::max)(1, (int)ceilf((bmax[1] - bmin[1]) / cell));
    std::vector<std::vector<uint16_t>> lists((size_t)nx * ny);
    for (size_t t = 0; t < recs.size(); t++) {
        int x0 = (std::max)(0, (int)floorf((recs[t].lo[0] - bmin[0]) / cell)), x1 = (std::min)(nx - 1, (int)floorf((recs[t].hi[0] - bmin[0]) / cell));
        int y0 = (std::max)(0, (int)floorf((recs[t].lo[1] - bmin[1]) / cell)), y1 = (std::min)(ny - 1, (int)floorf((recs[t].hi[1] - bmin[1]) / cell));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) lists[(size_t)y * nx + x].push_back((uint16_t)t);
    }
    struct Cell { uint16_t first, count, vlo, vhi; float c[3], r2; };
    std::vector<Cell> cells;
    std::vector<uint16_t> flat, G((size_t)nx * ny, 0);
    for (size_t k = 0; k < lists.size(); k++) {
        auto &l = lists[k];
        if (l.empty()) continue;
        float lo[3], hi[3];
        for (int q = 0; q < 3; q++) {
            lo[q] = 1e30f, hi[q] = -1e30f;
            for (auto t : l) lo[q] = (std::min)(lo[q], recs[t].lo[q]), hi[q] = (std::max)(hi[q], recs[t].hi[q]);
        }
        Cell c{};
        c.first = (uint16_t)flat.size(), c.count = (uint16_t)l.size();
        c.vlo = (uint16_t)vert({lo[0], lo[1], lo[2]}), c.vhi = (uint16_t)vert({hi[0], hi[1], hi[2]});
        float r2 = 0;
        for (int q = 0; q < 3; q++) c.c[q] = (lo[q] + hi[q]) / 2, r2 += ((hi[q] - lo[q]) / 2) * ((hi[q] - lo[q]) / 2);
        c.r2 = r2;
        if (flat.size() + l.size() > 0xFFFF) return false;
        flat.insert(flat.end(), l.begin(), l.end());
        cells.push_back(c);
        G[k] = (uint16_t)cells.size();
    }
    if (V.size() > 0xFFFF || Nrm.size() > 0xFFFF || recs.size() > 0xFFFF || cells.size() > 0xFFFF) return false;
    size_t size = 0x44 + 12 * V.size() + 12 * Nrm.size() + 16 * recs.size() + 32 * cells.size() + 2 * flat.size() +
                  2 * G.size() + C.tail.size();
    out.assign(size, 0);
    uint8_t *p = (uint8_t *)&out[0];
    uint32_t hd[10] = {0, (uint32_t)size, (uint32_t)nx, (uint32_t)ny, 0, (uint32_t)V.size(), (uint32_t)Nrm.size(),
                       (uint32_t)recs.size(), (uint32_t)cells.size(), (uint32_t)flat.size()};
    memcpy(hd, "PMS2", 4);
    memcpy(&hd[4], &cell, 4);
    memcpy(p, hd, 40);
    memcpy(p + 0x28, bmin, 12), memcpy(p + 0x34, bmax, 12);
    float inv = 1.0f / cell;
    memcpy(p + 0x40, &inv, 4);
    p += 0x44;
    for (auto &v : V) memcpy(p, v.data(), 12), p += 12;
    for (auto &n : Nrm) memcpy(p, n.data(), 12), p += 12;
    for (auto &r : recs) memcpy(p, r.r, 16), p += 16;
    for (auto &c : cells) memcpy(p, &c, 24), p += 32;
    memcpy(p, flat.data(), 2 * flat.size()), p += 2 * flat.size();
    memcpy(p, G.data(), 2 * G.size()), p += 2 * G.size();
    memcpy(p, C.tail.data(), C.tail.size());
    return true;
}

static void go_live()                                   // game thread: the model into the engine
{
    std::string b;
    if (!build(b, true)) { cstatus("collision too large for 16-bit indices - not applied"); return; }
    if (b.size() > g_live_cap) {                        // grow (the old buffer is kept: poly pointers may linger)
        g_live_cap = b.size() + b.size() / 2;
        g_live = (uint8_t *)malloc(g_live_cap);
        if (!g_live) { g_live_cap = 0; return; }
    }
    memcpy(g_live, b.data(), b.size());
    uint32_t h[10];
    memcpy(h, g_live, 40);
    uint32_t nv = h[5], nn = h[6], nt = h[7], nc = h[8], nl = h[9];
    memcpy(C.header, g_live, 0x44);                     // grid size, bounds, counts, 1 / cell size
    uint8_t *v = g_live + 0x44, *n = v + 12 * nv, *t = n + 12 * nn, *c = t + 16 * nt, *l = c + 32 * nc, *g = l + 2 * nl;
    *(uint8_t **)0x380670 = v;
    *(uint32_t *)0x380668 = nv;
    *(uint8_t **)0x38067C = n;
    *(uint32_t *)0x380674 = nn;
    *(uint8_t **)0x4CAB8C = t;
    *(uint8_t **)0x4CAB90 = c;
    *(uint8_t **)0x4CAB94 = l;
    *(uint8_t **)0x4CAB98 = g;
    C.live_dirty = false;
    g_live_at = GetTickCount();
}

// the camera ray under a screen point against the model -> hit point
static bool pick(float nx, float ny, float hit[3])
{
    CamFrame f;
    if (!ed_cam_frame(f)) return false;
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = -f.bk[k] * f.n + f.rt[k] * nx * f.hw + f.up[k] * ny * f.hh;
    float best = 1e30f;
    int besti = -1;
    for (auto &t : C.T) {
        if (t.v[0] >= C.V.size() || t.v[1] >= C.V.size() || t.v[2] >= C.V.size()) continue;
        const float *a = C.V[t.v[0]].data(), *b = C.V[t.v[1]].data(), *c = C.V[t.v[2]].data();
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (fabsf(det) < 1e-12f) continue;
        float inv = 1.0f / det, s[3] = {f.c[0] - a[0], f.c[1] - a[1], f.c[2] - a[2]};
        float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0 || u > 1) continue;
        float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        float w = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (w < 0 || u + w > 1) continue;
        float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (tt > 0 && tt < best) best = tt, besti = (int)(&t - C.T.data());
    }
    if (best >= 1e30f) return false;
    g_pick_tri = besti;
    for (int k = 0; k < 3; k++) hit[k] = f.c[k] + d[k] * best;
    return true;
}

static void snapshot()
{
    std::vector<uint16_t> fl(C.T.size());
    for (size_t i = 0; i < C.T.size(); i++) fl[i] = C.T[i].flags;
    g_undo.emplace_back(C.V, fl);
    if (g_undo.size() > 30) g_undo.erase(g_undo.begin());
}

static void apply_brush(const float hit[3], bool alt)  // game thread, once per frame while held
{
    int tool = g_tool;
    if (alt) tool = tool == T_RAISE ? T_LOWER : tool == T_LOWER ? T_RAISE : tool == T_FLOOR_OFF ? T_FLOOR_ON
                  : tool == T_FLOOR_ON ? T_FLOOR_OFF : tool;
    float r = k_radii[g_radius], r2 = r * r;
    bool changed = false;
    if (tool <= T_SMOOTH) {
        std::vector<float> nz;
        if (tool == T_SMOOTH) {
            nz.resize(C.V.size());
            for (size_t i = 0; i < C.V.size(); i++) nz[i] = C.V[i][2];
        }
        for (size_t i = 0; i < C.V.size(); i++) {
            auto &v = C.V[i];
            float dx = v[0] - hit[0], dy = v[1] - hit[1], dd = dx * dx + dy * dy;
            if (dd > r2 || C.adj[i].empty()) continue;   // bounds-only vertices are rebuilt anyway
            float w = 1.0f - dd / r2;
            w *= w;
            float z = v[2];
            if (tool == T_RAISE) z += 0.04f * w;
            else if (tool == T_LOWER) z -= 0.04f * w;
            else if (tool == T_FLATTEN) z += (C.target - z) * 0.25f * w;
            else {
                float s = 0;
                for (int j : C.adj[i]) s += C.V[j][2];
                z += (s / C.adj[i].size() - z) * 0.3f * w;
            }
            if (tool == T_SMOOTH) nz[i] = z;
            else if (z != v[2]) v[2] = z, changed = true;
        }
        if (tool == T_SMOOTH)
            for (size_t i = 0; i < C.V.size(); i++)
                if (nz[i] != C.V[i][2]) C.V[i][2] = nz[i], changed = true;
    } else {
        for (auto &t : C.T) {
            if (t.v[0] >= C.V.size() || t.v[1] >= C.V.size() || t.v[2] >= C.V.size()) continue;
            float cx = (C.V[t.v[0]][0] + C.V[t.v[1]][0] + C.V[t.v[2]][0]) / 3 - hit[0];
            float cy = (C.V[t.v[0]][1] + C.V[t.v[1]][1] + C.V[t.v[2]][1]) / 3 - hit[1];
            if (cx * cx + cy * cy > r2) continue;
            uint16_t f = t.flags;
            if (tool == T_FLOOR_OFF) f &= ~4;
            else if (tool == T_FLOOR_ON) f |= 4;
            else f = (uint16_t)((f & 0x0FFF) | (g_cat << 12));
            if (f != t.flags) t.flags = f, changed = true;
        }
    }
    if (changed) C.dirty = C.live_dirty = true;
}

// ------------------------------------------------------------------------------------------------ game thread
void coll_frame()
{
    if (!g_cpaint) return;
    int st = game_state();
    uint8_t *scene = scene_ptr();
    uint8_t *hdr = scene ? *(uint8_t **)(scene + 0xC) : nullptr;
    EnterCriticalSection(&g_ccs);
    if (C.loaded && (st != 7 && st != 12 || hdr != C.header)) C.loaded = false;   // left the scene
    if (!C.loaded && (st == 7 || st == 12) && hdr) {
        static DWORD tried;
        if (GetTickCount() - tried > 2000) {
            tried = GetTickCount();
            load_pms();
        }
    }
    if (!C.loaded) { LeaveCriticalSection(&g_ccs); return; }
    if (g_revert_req) {
        g_revert_req = false;
        std::string key = C.key;
        if (load_pms()) {
            go_live();
            cstatus("%s reloaded from the file - edits since the last save dropped", key.c_str());
        }
    }
    if (g_undo_req) {
        g_undo_req = false;
        if (g_undo.empty()) cstatus("nothing to undo");
        else {
            C.V = g_undo.back().first;
            auto &fl = g_undo.back().second;
            for (size_t i = 0; i < C.T.size() && i < fl.size(); i++) C.T[i].flags = fl[i];
            g_undo.pop_back();
            C.dirty = C.live_dirty = true;
            cstatus("undone - %d more", (int)g_undo.size());
        }
    }
    auto in = g_cin;
    g_cin.press = false;
    if (in.press || in.down) {
        float hit[3];
        if (pick(in.nx, in.ny, hit)) {
            if (in.press) {
                snapshot();
                C.target = hit[2];
            }
            apply_brush(hit, in.alt);
        }
    }
    if (C.live_dirty && (!in.down || GetTickCount() - g_live_at > 200)) go_live();
    LeaveCriticalSection(&g_ccs);
}

void coll_draw(const CamFrame &cf)                      // game thread, overlay frame open
{
    if (!g_cpaint) return;
    EnterCriticalSection(&g_ccs);
    if (!C.loaded) { LeaveCriticalSection(&g_ccs); return; }
    float hit[3];
    bool have = g_cin.valid && pick(g_cin.nx, g_cin.ny, hit);
    // the area shown grows with the camera's distance (zoomed out = a wider patch); centred on the cursor, else on
    // the middle of the screen; the nearest triangles first when there are too many to draw
    float r = k_radii[g_radius], ctr3[3];
    bool cen = have;
    if (have) memcpy(ctr3, hit, sizeof ctr3);
    else cen = pick(0.0f, 0.0f, ctr3);
    if (!cen) { LeaveCriticalSection(&g_ccs); return; }
    float dist = sqrtf((ctr3[0] - cf.c[0]) * (ctr3[0] - cf.c[0]) + (ctr3[1] - cf.c[1]) * (ctr3[1] - cf.c[1]) +
                       (ctr3[2] - cf.c[2]) * (ctr3[2] - cf.c[2]));
    float view = (std::max)((std::max)(10.0f, r * 3), dist * 0.9f);
    static std::vector<std::pair<float, int>> cand;
    cand.clear();
    for (size_t i = 0; i < C.T.size(); i++) {
        const CTri &t = C.T[i];
        if (t.v[0] >= C.V.size() || t.v[1] >= C.V.size() || t.v[2] >= C.V.size()) continue;
        float cx = (C.V[t.v[0]][0] + C.V[t.v[1]][0] + C.V[t.v[2]][0]) / 3 - ctr3[0];
        float cy = (C.V[t.v[0]][1] + C.V[t.v[1]][1] + C.V[t.v[2]][1]) / 3 - ctr3[1];
        float dd = cx * cx + cy * cy;
        if (dd <= view * view) cand.emplace_back(dd, (int)i);
    }
    const size_t MAXT = 5000;
    if (cand.size() > MAXT) {
        std::nth_element(cand.begin(), cand.begin() + MAXT, cand.end());
        cand.resize(MAXT);
    }
    float thin = dist > 40 ? 0.5f : 0.7f;
    for (auto &cd : cand) {
        const CTri &t = C.T[cd.second];
        const float *p[3] = {C.V[t.v[0]].data(), C.V[t.v[1]].data(), C.V[t.v[2]].data()};
        bool in = have && cd.first <= r * r;
        uint32_t col = !(t.flags & 4) ? 0xC0FF3030 : (k_cat_colors[t.flags >> 12] & 0x00FFFFFF) | 0x90000000;
        if (in) col |= 0xFF000000;
        float v[3][3];
        for (int k = 0; k < 3; k++) {
            float w[3] = {p[k][0], p[k][1], p[k][2] + 0.03f};
            ed_to_cam(cf, w, v[k]);
        }
        for (int k = 0; k < 3; k++) ed_cam_line(cf, v[k], v[(k + 1) % 3], in ? 1.2f : thin, col);
    }
    if (have) {                                         // the brush ring
        float prev[3];
        for (int i = 0; i <= 32; i++) {
            float a = i * 6.2831853f / 32, w[3] = {hit[0] + r * cosf(a), hit[1] + r * sinf(a), hit[2] + 0.08f}, v[3];
            float gz;
            if (ed_ground_at(w[0], w[1], hit[2] + 4.0f, gz)) w[2] = gz + 0.08f;
            ed_to_cam(cf, w, v);
            if (i) ed_cam_line(cf, prev, v, 1.5f, 0xFFFFFFFF);
            memcpy(prev, v, sizeof v);
        }
    }
    char b[200];
    _snprintf(b, sizeof b, "COLLISION  %s  radius %.1f%s   %s", k_tools[g_tool], r,
              g_tool == T_CATEGORY ? (" cat " + std::to_string(g_cat)).c_str() : "", C.dirty ? "UNSAVED" : "");
    b[sizeof b - 1] = 0;
    ov_text(b, 12, 54, OV_LEFT, 0xFF80FFFF, true);
    if (have) {
        int ti = g_pick_tri;
        uint16_t fl = ti >= 0 && ti < (int)C.T.size() ? C.T[ti].flags : 0;
        _snprintf(b, sizeof b, "at %.2f, %.2f  height %.2f   surface %s%s", hit[0], hit[1], hit[2], k_cat_names[fl >> 12],
                  fl & 4 ? "" : "  (NOT floor)");
        b[sizeof b - 1] = 0;
        ov_text(b, 12, 72, OV_LEFT, 0xFFC0FFFF, true);
    }
    LeaveCriticalSection(&g_ccs);
}

// ------------------------------------------------------------------------------------------------ input
bool coll_active()
{
    int st = game_state();
    return g_cpaint && (st == 7 || st == 12);
}
void coll_press(float nx, float ny, bool ctrl)
{
    g_cin.nx = nx, g_cin.ny = ny, g_cin.alt = ctrl;
    g_cin.valid = g_cin.down = g_cin.press = true;
}
void coll_move(float nx, float ny, bool down)
{
    g_cin.nx = nx, g_cin.ny = ny, g_cin.valid = true;
    if (!down) g_cin.down = false;
}
void coll_release() { g_cin.down = false; }

// ------------------------------------------------------------------------------------------------ window (Level page)
enum { IDC_PAINT = 700, IDC_TOOL, IDC_RADIUS, IDC_CAT, IDC_SAVE, IDC_UNDO, IDC_REVERT, IDC_STATUS };

static void save_pms()                                  // editor thread
{
    std::string out, key;
    EnterCriticalSection(&g_ccs);
    bool ok = C.loaded && build(out, false);
    key = C.key;
    LeaveCriticalSection(&g_ccs);
    if (!ok) { SetWindowTextA(g_cstatus, "nothing to save - turn on Paint collision in a scene first"); return; }
    if (!ed_save_file(key, out)) { SetWindowTextA(g_cstatus, ("could NOT write " + key).c_str()); return; }
    port_mods_rescan();
    EnterCriticalSection(&g_ccs);
    C.dirty = false;
    LeaveCriticalSection(&g_ccs);
    SetWindowTextA(g_cstatus, ("saved " + key + " (" + std::to_string(out.size()) + " bytes) - already live").c_str());
}

bool coll_command(int id, int code)                     // editor thread
{
    HWND page = GetParent(g_cstatus);
    if (id == IDC_PAINT && code == BN_CLICKED) {
        g_cpaint = SendDlgItemMessageA(page, IDC_PAINT, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (g_cpaint) ed_level_guide(2, k_guide_heads[g_tool]);
        SetWindowTextA(g_cstatus, g_cpaint ? "loading the scene's collision..." : "collision painting off");
        return true;
    }
    if (id == IDC_TOOL && code == CBN_SELCHANGE) {
        g_tool = (int)SendDlgItemMessageA(page, IDC_TOOL, CB_GETCURSEL, 0, 0);
        ed_level_guide(2, k_guide_heads[g_tool]);
        return true;
    }
    if (id == IDC_CAT && code == CBN_SELCHANGE) ed_level_guide(2, "Surface category");
    if (id == IDC_RADIUS && code == CBN_SELCHANGE) { g_radius = (int)SendDlgItemMessageA(page, IDC_RADIUS, CB_GETCURSEL, 0, 0); return true; }
    if (id == IDC_CAT && code == CBN_SELCHANGE) { g_cat = (int)SendDlgItemMessageA(page, IDC_CAT, CB_GETCURSEL, 0, 0); return true; }
    if (id == IDC_SAVE && code == BN_CLICKED) { save_pms(); return true; }
    if (id == IDC_UNDO && code == BN_CLICKED) { g_undo_req = true; return true; }
    if (id == IDC_REVERT && code == BN_CLICKED) {
        if (MessageBoxA(page, "Reload the collision from its file (the last save)?\n\nUnsaved collision edits are lost.",
                        "Gladius Editor", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            g_revert_req = true;
        return true;
    }
    return false;
}

static HWND cadd(HWND page, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, page, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

int coll_create_ui(HWND page, int y)                    // editor thread -> the y below the section
{
    InitializeCriticalSection(&g_ccs);
    cadd(page, "BUTTON", "Collision (.pms)", BS_GROUPBOX, 6, y, 398, 186, -1);
    y += 20;
    cadd(page, "BUTTON", "Paint collision (world map / battle: clicks edit the floor)", BS_AUTOCHECKBOX, 14, y, 370, 20, IDC_PAINT);
    y += 26;
    cadd(page, "STATIC", "Tool", 0, 14, y + 4, 40, 16, -1);
    HWND tc = cadd(page, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 56, y, 200, 220, IDC_TOOL);
    for (auto n : k_tools) SendMessageA(tc, CB_ADDSTRING, 0, (LPARAM)n);
    SendMessageA(tc, CB_SETCURSEL, 0, 0);
    cadd(page, "STATIC", "Radius", 0, 264, y + 4, 40, 16, -1);
    HWND rc = cadd(page, "COMBOBOX", "", CBS_DROPDOWNLIST, 306, y, 90, 160, IDC_RADIUS);
    for (auto n : {"0.5", "1", "2", "4", "8"}) SendMessageA(rc, CB_ADDSTRING, 0, (LPARAM)n);
    SendMessageA(rc, CB_SETCURSEL, 2, 0);
    y += 30;
    cadd(page, "STATIC", "Category", 0, 14, y + 4, 50, 16, -1);
    HWND cc = cadd(page, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 66, y, 90, 260, IDC_CAT);
    for (int i = 0; i < 16; i++) SendMessageA(cc, CB_ADDSTRING, 0, (LPARAM)k_cat_names[i]);
    SendMessageA(cc, CB_SETCURSEL, 1, 0);
    cadd(page, "BUTTON", "Undo stroke", BS_PUSHBUTTON, 164, y - 1, 76, 24, IDC_UNDO);
    cadd(page, "BUTTON", "Revert", BS_PUSHBUTTON, 246, y - 1, 64, 24, IDC_REVERT);
    cadd(page, "BUTTON", "Save collision", BS_PUSHBUTTON, 316, y - 1, 80, 24, IDC_SAVE);
    y += 30;
    g_cstatus = cadd(page, "STATIC", "Edits go live at once (the overlay shows the triangles near the cursor: red = not "
                     "floor, colours = category). Ctrl+click: the opposite tool. Save writes the scene's loose .pms.",
                     0, 14, y, 382, 52, IDC_STATUS);
    return y + 64;
}
