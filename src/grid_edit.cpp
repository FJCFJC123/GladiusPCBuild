// Battle grid painter (editor, Level page): paint the arena's grid layers live in a battle and save the level's .grd.
// The engine's grid is one byte per cell at 0x39C2C0[x * 32 + y] (32 x 32 cells of 2 units), built by the .grd loader
// (0x56C40) from the encounter's GRIDFILEs by layer name (NoMove 1, NoMoveNoCursor 3, KnockbackOnly 4, Gate 8,
// NoLargeUnits 0x10, MapCenter 0x20, LineOfSightPass 0x40, NoTeleport 0x80). The cursor and pathfinder read it, so a
// painted cell works at once. The level's own grid is the encounter's first GRIDFILE; file layout: tools/grd.py.
#include "port.h"
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include "overlay.h"
#include "editor.h"

bool port_read_game_file(const std::string &key, std::string &out);   // bec_read.cpp
void port_mods_rescan();                                               // mods.cpp

static const char *const k_layers[] = {"NoMove", "NoMoveNoCursor", "NoLargeUnits", "KnockbackOnly", "Gate", "MapCenter",
                                       "LineOfSightPass", "NoTeleport"};
static const uint8_t k_flags[] = {1, 3, 0x10, 4, 8, 0x20, 0x40, 0x80};
static const uint32_t k_colors[] = {0xE0FF4040, 0xE0FF40FF, 0xE0FFA020, 0xE040A0FF, 0xE0FFFF40, 0xE040FF40, 0xE040FFFF,
                                    0xE0A060FF};
enum { NLAYERS = 8, NSTART = 4, GSIZE = 32, CLEAR_ALL = 100 };
static const uint32_t k_start_colors[NSTART] = {0xFF40FFC0, 0xFFFFC040, 0xFFC080FF, 0xFF80C0FF};

static volatile bool g_painting;                        // Paint grid on: the game window's clicks paint cells
static volatile int g_clear = -1;                       // pending clear: layer index, NLAYERS = every layer
static volatile int g_layer, g_brush = 1;               // layer index, brush 1 / 3 / 5 cells
static HWND g_gstatus;
static CRITICAL_SECTION g_gcs;

static struct {
    bool loaded;
    std::string key;                                    // "data/levels/.../x.grd"
    std::vector<std::string> names;                     // the file's layers (without the F prefix)
    uint32_t cells[GSIZE][GSIZE];                       // [y][x] bit masks over names
    float heights[GSIZE + 1][GSIZE + 1];                // ground at the cell corners, so the overlay follows slopes
    bool dirty;
} G;

// Start-position files (the encounter's later GRIDFILEs, one per team). The engine reads them at battle setup, so
// painted starts apply from the next battle.
struct StartFile {
    std::string key;
    std::vector<std::string> names;
    uint32_t cells[GSIZE][GSIZE];
    bool dirty;
};
static std::vector<StartFile> g_sf;

static const char *layer_name(int L)                    // combo index -> name (NoMove.. then Start1..)
{
    static const char *const st[NSTART] = {"Start1", "Start2", "Start3", "Start4"};
    return L < NLAYERS ? k_layers[L] : st[(L - NLAYERS) % NSTART];
}

static bool read_grd(const std::string &key, std::vector<std::string> &names, uint32_t (*cells)[GSIZE])
{
    std::string d;
    if (!port_read_game_file(key, d) || d.size() < 8 + 960 + 4096) return false;
    uint32_t n;
    memcpy(&n, &d[4], 4);
    for (uint32_t i = 0; i < n && i < 30; i++) {
        const char *nm = &d[8 + 32 * i];
        std::string s(nm, strnlen(nm, 31));
        names.push_back(s.size() > 1 && s[0] == 'F' ? s.substr(1) : s);
    }
    for (int y = 0; y < GSIZE; y++)
        for (int x = 0; x < GSIZE; x++) memcpy(&cells[y][x], &d[8 + 960 + 4 * (y * GSIZE + x)], 4);
    return true;
}

static std::string grd_bytes(const std::vector<std::string> &names, const uint32_t (*cells)[GSIZE])
{
    std::string out(8 + 960 + 4096, 0);
    uint32_t ver = 2, n = (uint32_t)names.size();
    memcpy(&out[0], &ver, 4), memcpy(&out[4], &n, 4);
    for (int i = 0; i < 30; i++) {
        std::string nm = i < (int)n ? "F" + names[i] : "E";
        memcpy(&out[8 + 32 * i], nm.c_str(), (std::min)((size_t)31, nm.size()));
    }
    for (int y = 0; y < GSIZE; y++)
        for (int x = 0; x < GSIZE; x++) memcpy(&out[8 + 960 + 4 * (y * GSIZE + x)], &cells[y][x], 4);
    return out;
}

static int start_at(int x, int y)                      // the start layer (1..) a cell is in, 0 = none
{
    for (auto &f : g_sf)
        for (int i = 0; i < (int)f.names.size(); i++)
            if (f.cells[y][x] >> i & 1 && !_strnicmp(f.names[i].c_str(), "Start", 5)) {
                int n = atoi(f.names[i].c_str() + 5);
                if (n >= 1) return n;
            }
    return 0;
}

static struct {                                         // input (game window thread -> game thread)
    bool press, down, release, valid;
    float nx, ny;
    bool erase;
} g_gin;

static void gstatus(const char *fmt, ...)
{
    char b[300];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    b[sizeof b - 1] = 0;
    port_log("grid: %s\n", b);
    static char ring[8][300];                           // posted, not sent: the editor thread may be waiting on us
    static LONG slot;
    char *dst = ring[InterlockedIncrement(&slot) & 7];
    memcpy(dst, b, sizeof b);
    if (g_gstatus) PostMessageA(g_gstatus, WM_SETTEXT, 0, (LPARAM)dst);
}

static int battle_state()
{
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    return state ? *(const int *)(state + 8) : -1;
}

// "$l\x\y.grd" -> "data/levels/x/y.grd", "$d\encounters\..." -> "data/encounters/..."
static std::string data_key(std::string p)
{
    for (auto &c : p) if (c == (char)0x5C) c = '/';
    std::string low = p;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    if (low.compare(0, 3, "$l/") == 0) low = "data/levels/" + low.substr(3);
    else if (low.compare(0, 3, "$d/") == 0) low = "data/" + low.substr(3);
    std::string out;
    for (char c : low) if (!(c == '/' && !out.empty() && out.back() == '/')) out += c;
    return out;
}

static bool load_level_grid()                           // game thread
{
    G = {};
    std::string enc = ed_current_enc();
    if (enc.empty()) { gstatus("no encounter loaded - enter a battle first"); return false; }
    std::string text;
    if (!port_read_game_file(enc, text)) { gstatus("could not read %s", enc.c_str()); return false; }
    size_t p = text.find("GRIDFILE:");
    if (p == std::string::npos) { gstatus("%s has no GRIDFILE", enc.c_str()); return false; }
    size_t q0 = text.find('"', p), q1 = q0 == std::string::npos ? q0 : text.find('"', q0 + 1);
    if (q1 == std::string::npos) { gstatus("bad GRIDFILE line in %s", enc.c_str()); return false; }
    G.key = data_key(text.substr(q0 + 1, q1 - q0 - 1));
    read_grd(G.key, G.names, G.cells);
    g_sf.clear();
    for (p = text.find("GRIDFILE:", q1); p != std::string::npos; p = text.find("GRIDFILE:", p + 9)) {
        size_t a = text.find('"', p), b = a == std::string::npos ? a : text.find('"', a + 1);
        size_t eol = text.find('\n', p);
        if (b == std::string::npos || (eol != std::string::npos && a > eol)) continue;
        std::string k = data_key(text.substr(a + 1, b - a - 1));
        bool have = k == G.key;
        for (auto &f : g_sf) have |= f.key == k;
        if (have) continue;
        StartFile f{};
        f.key = k;
        if (read_grd(k, f.names, f.cells)) g_sf.push_back(f);
        else port_log("grid: could not read start file %s\n", k.c_str());
    }
    // ground at the cell corners for the overlay: median of a 3 x 3 patch per corner, holes filled from the neighbours,
    // outlier corners pulled back to their neighbours' median
    static bool have[GSIZE + 1][GSIZE + 1];
    for (int y = 0; y <= GSIZE; y++)
        for (int x = 0; x <= GSIZE; x++) {
            float s[9];
            int n = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    float h;
                    if (ed_ground_at(2.0f * x - 1 + 0.4f * i, 2.0f * y - 1 + 0.4f * j, 50.0f, h)) s[n++] = h;
                }
            std::sort(s, s + n);
            have[y][x] = n > 0;
            G.heights[y][x] = n ? s[n / 2] : 0.0f;
        }
    for (int pass = 0; pass < 8; pass++)                // holes: the mean of the known neighbours
        for (int y = 0; y <= GSIZE; y++)
            for (int x = 0; x <= GSIZE; x++) {
                if (have[y][x]) continue;
                float sum = 0;
                int n = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int xx = x + i, yy = y + j;
                        if ((i || j) && xx >= 0 && yy >= 0 && xx <= GSIZE && yy <= GSIZE && have[yy][xx])
                            sum += G.heights[yy][xx], n++;
                    }
                if (n) G.heights[y][x] = sum / n, have[y][x] = true;
            }
    for (int pass = 0; pass < 2; pass++)                // spikes
        for (int y = 0; y <= GSIZE; y++)
            for (int x = 0; x <= GSIZE; x++) {
                float nb[8];
                int n = 0;
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        int xx = x + i, yy = y + j;
                        if ((i || j) && xx >= 0 && yy >= 0 && xx <= GSIZE && yy <= GSIZE) nb[n++] = G.heights[yy][xx];
                    }
                if (n < 3) continue;
                std::sort(nb, nb + n);
                float h = G.heights[y][x], med = nb[n / 2];
                if ((h > nb[n - 1] + 0.3f || h < nb[0] - 0.3f) && fabsf(h - med) > 0.6f) G.heights[y][x] = med;
            }
    G.loaded = true;
    gstatus("%s: %d layers, %d start file(s) - pick a layer, click / drag cells in the game window (Ctrl erases)",
            G.key.c_str(), (int)G.names.size(), (int)g_sf.size());
    return true;
}

static int layer_bit(const char *name, bool add)       // the file's bit for a layer (added to the file when new)
{
    for (int i = 0; i < (int)G.names.size(); i++)
        if (!_stricmp(G.names[i].c_str(), name)) return i;
    if (!add || G.names.size() >= 30) return -1;
    G.names.push_back(name);
    return (int)G.names.size() - 1;
}

// The battle floor cell under a screen point.
static bool cell_under(float nx, float ny, int &cx, int &cy)
{
    CamFrame f;
    if (!ed_cam_frame(f)) return false;
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = -f.bk[k] * f.n + f.rt[k] * nx * f.hw + f.up[k] * ny * f.hh;
    if (fabsf(d[2]) < 1e-6f) return false;
    float z = 0, p[3];
    for (int it = 0; it < 4; it++) {
        float t = (z - f.c[2]) / d[2];
        if (t <= 0) return false;
        for (int k = 0; k < 3; k++) p[k] = f.c[k] + d[k] * t;
        float h;
        if (!ed_ground_at(p[0], p[1], p[2] + 20.0f, h)) break;
        if (fabsf(h - z) < 0.01f) break;
        z = h;
    }
    cx = (int)floorf((p[0] + 1.0f) / 2.0f), cy = (int)floorf((p[1] + 1.0f) / 2.0f);
    return cx >= 0 && cy >= 0 && cx < GSIZE && cy < GSIZE;
}

static void paint_start(int cx, int cy, bool erase)     // game thread: a StartN layer (the encounter's start file)
{
    const char *nm = layer_name(g_layer);
    StartFile *sf = nullptr;
    int bit = -1;
    for (auto &f : g_sf)
        for (int i = 0; i < (int)f.names.size() && !sf; i++)
            if (!_stricmp(f.names[i].c_str(), nm)) sf = &f, bit = i;
    if (!sf && !erase && !g_sf.empty() && g_sf[0].names.size() < 30) {   // a new start layer: into the first file
        sf = &g_sf[0];
        sf->names.push_back(nm);
        bit = (int)sf->names.size() - 1;
    }
    if (!sf) {
        static DWORD warned;
        if (GetTickCount() - warned > 3000) gstatus(g_sf.empty() ? "this encounter has no start file" : "no %s cells to erase", nm), warned = GetTickCount();
        return;
    }
    int r = g_brush / 2;
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= GSIZE || y >= GSIZE) continue;
            uint32_t before = sf->cells[y][x];
            sf->cells[y][x] = erase ? before & ~(1u << bit) : before | (1u << bit);
            if (sf->cells[y][x] != before) sf->dirty = G.dirty = true;
        }
}

static void paint_at(int cx, int cy, bool erase)        // game thread: our copy + the engine's grid
{
    if (g_layer >= NLAYERS) { paint_start(cx, cy, erase); return; }
    int L = g_layer;
    int bit = layer_bit(k_layers[L], !erase);
    uint8_t *eg = (uint8_t *)0x39C2C0;
    int r = g_brush / 2;
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= GSIZE || y >= GSIZE) continue;
            uint32_t before = G.cells[y][x];
            if (bit >= 0) G.cells[y][x] = erase ? before & ~(1u << bit) : before | (1u << bit);
            if (G.cells[y][x] != before) G.dirty = true;
            uint8_t &b = eg[x * GSIZE + y];             // engine: x-major
            b = erase ? (uint8_t)(b & ~k_flags[L]) : (uint8_t)(b | k_flags[L]);
        }
}

// ------------------------------------------------------------------------------------------------ game thread
void grid_frame()
{
    if (!g_painting) return;
    if (battle_state() != 12) {
        if (G.loaded) G.loaded = false;
        return;
    }
    if (!G.loaded && !load_level_grid()) { g_painting = false; return; }
    if (g_clear >= 0) {                                 // Clear layer / Clear all: our copy + the engine's grid
        int L = g_clear;
        g_clear = -1;
        uint8_t *eg = (uint8_t *)0x39C2C0, mask = 0;
        if (L >= NLAYERS && L < CLEAR_ALL) {            // a start layer: in the start files only
            for (auto &f : g_sf)
                for (int i = 0; i < (int)f.names.size(); i++)
                    if (!_stricmp(f.names[i].c_str(), layer_name(L))) {
                        for (int y = 0; y < GSIZE; y++)
                            for (int x = 0; x < GSIZE; x++) f.cells[y][x] &= ~(1u << i);
                        f.dirty = G.dirty = true;
                    }
            gstatus("%s cleared - paint new start cells, then Save grid (used from the next battle)", layer_name(L));
            goto cleared;
        }
        if (L >= CLEAR_ALL) {
            for (int i = 0; i < NLAYERS; i++) mask |= k_flags[i];
            for (int y = 0; y < GSIZE; y++)
                for (int x = 0; x < GSIZE; x++) {
                    uint32_t keep = 0;                  // layers this painter does not know (FStart...) stay
                    for (int i = 0; i < (int)G.names.size(); i++) {
                        bool known = false;
                        for (int k = 0; k < NLAYERS; k++) known |= !_stricmp(G.names[i].c_str(), k_layers[k]);
                        if (!known) keep |= 1u << i;
                    }
                    G.cells[y][x] &= keep;
                }
        } else {
            mask = k_flags[L];
            int bit = layer_bit(k_layers[L], false);
            if (bit >= 0)
                for (int y = 0; y < GSIZE; y++)
                    for (int x = 0; x < GSIZE; x++) G.cells[y][x] &= ~(1u << bit);
        }
        for (int i = 0; i < GSIZE * GSIZE; i++) eg[i] &= (uint8_t)~mask;
        G.dirty = true;
        gstatus(L >= CLEAR_ALL ? "every grid layer cleared (start positions kept) - paint the grid, then Save grid"
                               : "%s cleared everywhere - Save grid keeps it", L >= CLEAR_ALL ? "" : k_layers[L]);
    }
cleared:
    EnterCriticalSection(&g_gcs);
    auto in = g_gin;
    g_gin.press = g_gin.release = false;
    LeaveCriticalSection(&g_gcs);
    if (in.press || in.down) {
        int cx, cy;
        if (cell_under(in.nx, in.ny, cx, cy)) paint_at(cx, cy, in.erase);
    }
}

void grid_draw(const CamFrame &cf)                      // game thread, overlay frame open
{
    if (!g_painting || !G.loaded) return;
    int hx = -1, hy = -1, nblocked = 0;
    if (g_gin.valid) cell_under(g_gin.nx, g_gin.ny, hx, hy);
    for (int y = 0; y < GSIZE; y++)
        for (int x = 0; x < GSIZE; x++) {
            float c[4][3] = {{2.0f * x - 1, 2.0f * y - 1, G.heights[y][x] + 0.05f},
                             {2.0f * x + 1, 2.0f * y - 1, G.heights[y][x + 1] + 0.05f},
                             {2.0f * x + 1, 2.0f * y + 1, G.heights[y + 1][x + 1] + 0.05f},
                             {2.0f * x - 1, 2.0f * y + 1, G.heights[y + 1][x] + 0.05f}};
            // shown layer: the selected one, else the first layer the cell is in
            uint32_t col = 0;
            int Ls = g_layer, bs = -1;
            int st = start_at(x, y);
            if (st) {                                   // a start cell: team colour; red when the grid blocks it
                bool blocked = (((const uint8_t *)0x39C2C0)[x * GSIZE + y] & 1) != 0;
                if (blocked) nblocked++;
                float v[4][3];
                for (int k = 0; k < 4; k++) ed_to_cam(cf, c[k], v[k]);
                uint32_t sc = blocked ? 0xFFFF2020 : k_start_colors[(st - 1) % NSTART];
                float in[4][3];                          // an inset square + its diagonals
                for (int k = 0; k < 4; k++)
                    for (int j = 0; j < 3; j++) in[k][j] = v[k][j] * 0.8f + (v[0][j] + v[1][j] + v[2][j] + v[3][j]) * 0.05f;
                for (int k = 0; k < 4; k++) ed_cam_line(cf, in[k], in[(k + 1) % 4], blocked ? 2.0f : 1.4f, sc);
                ed_cam_line(cf, in[0], in[2], 1.0f, sc), ed_cam_line(cf, in[1], in[3], 1.0f, sc);
            }
            if (Ls < NLAYERS)
                for (int i = 0; i < (int)G.names.size(); i++)
                    if (!_stricmp(G.names[i].c_str(), k_layers[Ls])) bs = i;
            if (bs >= 0 && (G.cells[y][x] >> bs & 1)) col = k_colors[Ls];
            else
                for (int L = 0; L < NLAYERS && !col; L++)
                    for (int i = 0; i < (int)G.names.size(); i++)
                        if (!_stricmp(G.names[i].c_str(), k_layers[L]) && (G.cells[y][x] >> i & 1)) {
                            col = (k_colors[L] & 0x00FFFFFF) | 0x70000000;
                            break;
                        }
            bool hot = x >= hx - g_brush / 2 && x <= hx + g_brush / 2 && y >= hy - g_brush / 2 && y <= hy + g_brush / 2 && hx >= 0;
            if (!col && !hot) {                         // empty: a faint outline near the cursor only
                if (hx < 0 || abs(x - hx) > 8 || abs(y - hy) > 8) continue;
                col = 0x30FFFFFF;
            }
            if (hot) col = 0xFFFFFFFF;
            float v[4][3];
            for (int k = 0; k < 4; k++) ed_to_cam(cf, c[k], v[k]);
            for (int k = 0; k < 4; k++) ed_cam_line(cf, v[k], v[(k + 1) % 4], hot ? 1.5f : 0.8f, col);
            if (col >> 24 >= 0xE0 && !hot) ed_cam_line(cf, v[0], v[2], 0.6f, col), ed_cam_line(cf, v[1], v[3], 0.6f, col);
        }
    char b[160];
    _snprintf(b, sizeof b, "GRID  layer %s  brush %d   cell %d,%d   %s", layer_name(g_layer), g_brush, hx, hy,
              G.dirty ? "UNSAVED" : "");
    b[sizeof b - 1] = 0;
    ov_text(b, 12, 14, OV_LEFT, 0xFFFFE080, true);
    if (nblocked) {
        _snprintf(b, sizeof b, "%d START CELL(S) BLOCKED (red) - units cannot be placed there", nblocked);
        b[sizeof b - 1] = 0;
        ov_text(b, 12, 34, OV_LEFT, 0xFFFF4040, true);
    }
}

// ------------------------------------------------------------------------------------------------ input
bool grid_active() { return g_painting && battle_state() == 12; }
void grid_press(float nx, float ny, bool ctrl)
{
    EnterCriticalSection(&g_gcs);
    g_gin.press = g_gin.down = g_gin.valid = true, g_gin.nx = nx, g_gin.ny = ny, g_gin.erase = ctrl;
    LeaveCriticalSection(&g_gcs);
}
void grid_move(float nx, float ny, bool down)
{
    EnterCriticalSection(&g_gcs);
    g_gin.nx = nx, g_gin.ny = ny, g_gin.valid = true;
    if (!down) g_gin.down = false;
    LeaveCriticalSection(&g_gcs);
}
void grid_release()
{
    EnterCriticalSection(&g_gcs);
    g_gin.down = false;
    LeaveCriticalSection(&g_gcs);
}

// ------------------------------------------------------------------------------------------------ window (Level page)
enum { IDG_PAINT = 600, IDG_LAYER, IDG_BRUSH, IDG_SAVE, IDG_STATUS, IDG_CLEARLAYER, IDG_CLEARALL };

static bool save_grid()                                 // editor thread
{
    if (!G.loaded) { SetWindowTextA(g_gstatus, "nothing to save - turn on Paint grid in a battle first"); return false; }
    if (!ed_save_file(G.key, grd_bytes(G.names, G.cells))) {
        SetWindowTextA(g_gstatus, ("could NOT write " + G.key).c_str());
        return false;
    }
    std::string msg = "saved " + G.key;
    for (auto &f : g_sf) {
        if (!f.dirty) continue;
        if (!ed_save_file(f.key, grd_bytes(f.names, f.cells))) {
            SetWindowTextA(g_gstatus, ("could NOT write " + f.key).c_str());
            return false;
        }
        f.dirty = false;
        msg += " + " + f.key.substr(f.key.find_last_of('/') + 1) + " (starts: next battle)";
    }
    port_mods_rescan();
    G.dirty = false;
    SetWindowTextA(g_gstatus, msg.c_str());
    return true;
}

bool grid_command(int id, int code)                     // editor thread: Level page controls -> true = handled
{
    if (id == IDG_PAINT && code == BN_CLICKED) {
        g_painting = SendDlgItemMessageA(GetParent(g_gstatus), IDG_PAINT, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (g_painting) ed_level_guide(1, g_layer >= NLAYERS ? "Start1 / Start2 / Start3 / Start4" : layer_name(g_layer));
        if (!g_painting) G.loaded = false;
        SetWindowTextA(g_gstatus, g_painting ? (battle_state() == 12 ? "loading the arena's grid..."
                                                                     : "enter a battle - the grid shows there")
                                             : "grid painting off");
        return true;
    }
    if (id == IDG_LAYER && code == CBN_SELCHANGE) {
        g_layer = (int)SendDlgItemMessageA(GetParent(g_gstatus), IDG_LAYER, CB_GETCURSEL, 0, 0);
        ed_level_guide(1, g_layer >= NLAYERS ? "Start1 / Start2 / Start3 / Start4" : layer_name(g_layer));
        return true;
    }
    if (id == IDG_BRUSH && code == CBN_SELCHANGE) {
        int i = (int)SendDlgItemMessageA(GetParent(g_gstatus), IDG_BRUSH, CB_GETCURSEL, 0, 0);
        g_brush = 1 + 2 * (i < 0 ? 0 : i);
        return true;
    }
    if (id == IDG_SAVE && code == BN_CLICKED) { save_grid(); return true; }
    if ((id == IDG_CLEARLAYER || id == IDG_CLEARALL) && code == BN_CLICKED) {
        if (!G.loaded) { SetWindowTextA(g_gstatus, "turn on Paint grid in a battle first"); return true; }
        std::string q = id == IDG_CLEARALL ? std::string("Clear EVERY layer of this arena's grid?")
                                           : "Clear the " + std::string(layer_name(g_layer)) + " layer everywhere?";
        if (MessageBoxA(GetParent(g_gstatus), (q + "\n\nIt takes effect in the battle now; Save grid writes it.").c_str(),
                        "Gladius Editor", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            g_clear = id == IDG_CLEARALL ? CLEAR_ALL : g_layer;
        return true;
    }
    return false;
}

static HWND gadd(HWND page, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, page, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

int grid_create_ui(HWND page, int y)                    // editor thread -> the y below the section
{
    InitializeCriticalSection(&g_gcs);
    gadd(page, "BUTTON", "Battle grid", BS_GROUPBOX, 6, y, 398, 158, -1);
    y += 20;
    gadd(page, "BUTTON", "Paint grid (in a battle: clicks paint cells)", BS_AUTOCHECKBOX, 14, y, 300, 20, IDG_PAINT);
    y += 26;
    gadd(page, "STATIC", "Layer", 0, 14, y + 4, 40, 16, -1);
    HWND lc = gadd(page, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 56, y, 160, 260, IDG_LAYER);
    for (int L = 0; L < NLAYERS + NSTART; L++) SendMessageA(lc, CB_ADDSTRING, 0, (LPARAM)layer_name(L));
    SendMessageA(lc, CB_SETCURSEL, 0, 0);
    gadd(page, "STATIC", "Brush", 0, 226, y + 4, 36, 16, -1);
    HWND bc = gadd(page, "COMBOBOX", "", CBS_DROPDOWNLIST, 264, y, 60, 100, IDG_BRUSH);
    for (auto n : {"1", "3", "5"}) SendMessageA(bc, CB_ADDSTRING, 0, (LPARAM)n);
    SendMessageA(bc, CB_SETCURSEL, 0, 0);
    gadd(page, "BUTTON", "Save grid", BS_PUSHBUTTON, 330, y - 1, 66, 24, IDG_SAVE);
    y += 30;
    gadd(page, "BUTTON", "Clear this layer", BS_PUSHBUTTON, 14, y, 120, 24, IDG_CLEARLAYER);
    gadd(page, "BUTTON", "Clear all layers (blank grid)", BS_PUSHBUTTON, 140, y, 186, 24, IDG_CLEARALL);
    y += 30;
    g_gstatus = gadd(page, "STATIC", "Ctrl+click erases. Saves into the arena's level .grd (the encounter's first GRIDFILE).",
                     0, 14, y, 382, 40, IDG_STATUS);
    return y + 52;
}
