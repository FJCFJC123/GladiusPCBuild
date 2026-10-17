// Live mesh editing (gladius_editor.exe): reshape a loaded static prop model in the running game, Save writes the
// change into a loose copy of its .pax.
//
// Where the vertices live: the model build (0x1E44E0) resolves the XRND geometry (0x219C40: header offsets ->
// pointers, 0x219910 / 0x2193A0 / 0x219680 block fixups). Each vertex-buffer entry (0x14 bytes: +0 -> D3D VB
// header, +4 vertex count, +0x10 stride) whose header has Common = -1 gets a D3D buffer (0x2154A0) that the file's
// vertices are copied into (Lock2 + copy); then the file buffer is cut off at the vertex data block (0x176C80),
// so the vertices the game draws exist only in that buffer (header +4 Data). The port's renderer hashes every
// vertex / index range it draws each frame and uploads what changed (d3d_program.cpp cached_stream /
// index_list), so bytes written into those buffers show on the next frame - no reload.
//
// Chain from a prop: prop +0x7C model instance -> +0x18 model -> +0x1C pax object (+0x28 SKEL, +0x4C doeg) and
// model +0x28 geometry record (+0x1C count A / +0x20 sub-mesh pointer list, +0x28 / +0x2C list B, +0x50 sphere
// radius, +0x54 centre). Sub-mesh (0x38): +0x04 VB entry, +0x08 index entry (+4 -> u16 indices, +0x0C count,
// +0x10 primitive 5 list / 6 strip / 7 fan), +0x0C sphere radius, +0x10 centre, +0x34 name. Model +0x40 centre /
// +0x4C radius (culling), +0x5C / +0x68 box (picking).
//
// Save: the pax is read again through the engine's loader (loose copy or .bec) and the edited vertex and index
// bytes are written over the file's own (every in-block pointer is block-relative, so a vertex buffer entry's
// data lands at base + block 0x6C + its D3D header's Data offset); sub-mesh and geometry spheres are updated too.
// The edits are only in game memory until then (undo / redo journal per stroke).
#include "port.h"
#include <vector>
#include <string>
#include <algorithm>
#include <array>
#include <set>
#include <functional>
#include <math.h>
#include <stdarg.h>
#include "overlay.h"
#include "editor.h"
#include <wincodec.h>
#include <commdlg.h>

void port_mods_rescan();                                // mods.cpp
void port_editor_reload();                              // editor.cpp
const char *port_game_dir();                            // xapi_file.cpp
void d3d_vb_invalidate(DWORD data);                     // d3d_program.cpp

enum { T_SELECT, T_MOVE, T_GRAB, T_SMOOTH, T_INFLATE, T_DEFLATE, T_PAINT, T_TEXPAINT, T_PIECE, T_COUNT };
static const char *const k_tool_names[T_COUNT] = {"Select", "Move", "Grab", "Smooth", "Inflate", "Deflate", "Paint",
                                                  "Texture paint", "Grab sub-mesh"};

// settings (editor thread writes, game thread reads)
static volatile int s_tool = T_SELECT, s_axis;          // axis: 0 view plane, 1 x, 2 y, 3 z (world)
static volatile float s_radius = 30.0f, s_strength = 0.3f;   // brush radius in 640x480 pixels
static volatile bool s_front = true, s_renormal = true, s_wire = true;
static volatile uint32_t s_color = 0xFFFFFFFF;          // paint colour (D3DCOLOR 0xAARRGGBB)

static CRITICAL_SECTION g_mcs;
static volatile bool g_on;                              // a session is open (game thread sets)
static HWND g_mwnd, g_msubs, g_mmodel, g_mstatus;
static const UINT WM_MESH_INFO = WM_APP + 20, WM_MESH_STATUS = WM_APP + 21, WM_MESH_RADIUS = WM_APP + 22,
                  WM_MESH_TEXDATA = WM_APP + 24, WM_MESH_PICKED = WM_APP + 26;
struct TexData { std::string name; int w, h, fmt; std::vector<uint8_t> top; };   // top mip level (export)

// ------------------------------------------------------------------------------------------------ commands
enum { C_START, C_STOP, C_SEL_SUB, C_HIDE_SUB, C_SEL_ALL, C_SEL_NONE, C_SEL_GROW, C_MOVE, C_ROTATE, C_SCALE, C_UNDO,
       C_REDO, C_REVERT, C_SAVE, C_SAVE_SHARED, C_FILL, C_TEX_REPLACE, C_TEX_RESTORE, C_TEX_EXPORT, C_DELETE, C_ADD_COLORS, C_MERGE, C_LEVEL,
       C_LEVEL_RESET };
struct Cmd {
    int type;
    std::string file, model, name;
    int live;
    float f[3];
    std::vector<int> list;
    bool add;
    int index;                                          // texture (C_TEX_*)
    std::vector<uint8_t> data;                          // encoded texture levels (C_TEX_REPLACE)
};
static std::vector<Cmd> g_cmds;

static void post_cmd(const Cmd &c)
{
    EnterCriticalSection(&g_mcs);
    g_cmds.push_back(c);
    LeaveCriticalSection(&g_mcs);
}
static void post_simple(int type, float a = 0, float b = 0, float c = 0)
{
    Cmd k{};
    k.type = type;
    k.f[0] = a, k.f[1] = b, k.f[2] = c;
    post_cmd(k);
}

static void mstatus(const char *fmt, ...)               // any thread
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    b[sizeof b - 1] = 0;
    port_log("mesh: %s\n", b);
    if (g_mwnd) PostMessageA(g_mwnd, WM_MESH_STATUS, 0, (LPARAM) new std::string(b));
}

// ------------------------------------------------------------------------------------------------ session
struct Vb { uint8_t *data; int stride, count, base, entry; std::vector<uint8_t> orig; };
// a texture the model draws with: texture object (command 0x01 -> +0x10 name, +0x14 D3D texture header in the
// model's bitmap, +0x28 / +0x2C size); DXT1 (0x0C) / DXT5 (0x0F) are editable, mip levels follow each other
struct Tex {
    std::string name;
    uint8_t *hdr;                                       // XPixelContainer: +4 Data, +0xC Format
    int w, h, levels, fmt;
    size_t bytes;
    std::vector<uint8_t> orig;
    std::vector<uint8_t> rgba;                          // texture paint: decoded top level (empty = not decoded)
    std::vector<char> dirty;                            // ... and its 4x4 blocks changed since the last encode
};

// DXT helpers (texture import / export / paint, defined with the window code)
static void dxt_decode(const uint8_t *src, int w, int h, int fmt, std::vector<uint8_t> &rgba);
static void dxt_color_block(const uint8_t px[16][4], uint8_t out[8]);
static void dxt_alpha_block(const uint8_t px[16][4], uint8_t out[8]);
static void encode_levels(std::vector<uint8_t> img, int w, int h, int levels, bool dxt5, std::vector<uint8_t> &out);
struct Sub {
    int vb, entry_ib, rec, prim, nidx;
    WORD *idx;
    WORD *idx0;                                         // the file's own index memory (orig.size() entries)
    std::vector<WORD> shown;                            // indices before Hide (Show brings them back)
    std::set<std::array<WORD, 3>> painted;              // faces texture paint touched (local indices, canonical)
    uint8_t *ibe;                                       // index entry: +4 indices, +8 triangles, +0x0C count
    uint8_t *recp;
    std::string name;
    bool hidden;
    std::vector<WORD> orig;                             // indices at the start (Show / Revert)
    std::vector<int> verts;                             // vertices it uses (global)
    std::vector<int> tex;                               // textures its material binds
    uint8_t rec0[16];                                   // sphere as loaded (+0x0C radius, +0x10 centre)
};
struct Tri { int v[3], sub; };
struct VSave { int gi; uint8_t v[64]; };                // a whole vertex (position, normal, colour, UVs)
struct ISave { int sub; std::vector<WORD> idx; bool hidden; };
struct TSave { int tex; std::vector<uint8_t> bytes; };
struct Op { std::string what; std::vector<VSave> v; std::vector<ISave> ib; std::vector<TSave> tx; };

static struct Session {
    std::string file, model, name;                      // prop file, MODELS path (relative to data), prop name
    int live;
    uint8_t *prop, *mobj, *geom, *doeg;
    bool level;                                         // the scene's level geometry (.pao), not a prop
    uint32_t scene_var;                                 // level: the scene pointer's address ...
    uint8_t *scene;                                     // ... and its value when the session started
    std::vector<int> listsub;                           // list A then B position -> S.subs index (-1 = skipped)
    std::vector<Vb> vbs;
    std::vector<Sub> subs;
    std::vector<uint8_t *> P;                           // vertex start per global index
    std::vector<int> vvb;                               // vertex buffer per global index
    std::vector<Tex> texs;
    std::vector<int> vsub;                              // a sub-mesh using the vertex (-1 = none)
    int nv, ng;
    std::vector<float> pos0;
    std::vector<uint32_t> nrm0;
    std::vector<int> grp, gstart, gmem;                 // weld groups (same position): vertex -> group, members
    std::vector<int> nstart, nbr;                       // group adjacency
    std::vector<int> tstart, tlist;                     // triangles per vertex
    std::vector<Tri> tris;
    std::vector<std::pair<int, int>> edges;             // group pairs (wireframe)
    bool flip;                                          // face winding opposite to the stored normals
    float radius;                                       // model bounding radius (brush step sizes)
    std::vector<float> gsel;                            // selection weight per group
    std::vector<int> pieces;                            // Grab sub-mesh: the picked sub-meshes
    bool sel_is_pieces;                                 // the selection is exactly the picked pieces
    std::vector<Op> undo, redo;
    Op cur;
    bool in_op;
    std::vector<int> stamp;
    int opno;
    std::vector<int> moved;                             // groups moved this frame (normals)
    std::vector<char> moved_mark;
    std::vector<float> sx, sy, sz;                      // projected groups (overlay coords, depth)
    bool dirty;
    uint8_t geom0[16], msph0[16], mbox0[24];           // bounds as loaded: geometry +0x50, model +0x40 / +0x5C
} S;
static struct { float nx, ny, px, py; bool shift, ctrl, valid; } G;   // game thread's copy of the cursor

static float *posp(int gi) { return (float *)S.P[gi]; }
static uint32_t *nrmp(int gi) { return (uint32_t *)(S.P[gi] + 12); }
static int stridep(int gi) { return S.vbs[S.vvb[gi]].stride; }
static uint32_t *colp(int gi)                           // vertex colour (stride 28 / 36: after the normal) or null
{
    int st = stridep(gi);
    return st == 28 || st == 36 ? (uint32_t *)(S.P[gi] + 16) : nullptr;
}
static const uint8_t *origp(int gi) { const Vb &v = S.vbs[S.vvb[gi]]; return &v.orig[(gi - v.base) * v.stride]; }

static void unpack_n(uint32_t v, float n[3])
{
    n[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
    n[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
    n[2] = (float)((int32_t)v >> 22) / 511.0f;
}
static uint32_t pack_n(const float n[3])
{
    auto q = [](float v, int bits) {
        int m = (1 << (bits - 1)) - 1;
        int i = (int)lroundf(v * m);
        i = i < -m ? -m : i > m ? m : i;
        return (uint32_t)i & ((1u << bits) - 1);
    };
    return q(n[0], 11) | (q(n[1], 11) << 11) | (q(n[2], 10) << 22);
}

static const float k_identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static const float *world_matrix()                      // levels are in world space
{
    return S.level ? (S.mobj ? k_identity : nullptr) : S.prop ? *(const float *const *)(S.prop + 8) : nullptr;
}

static void to_world(const float *M, const float l[3], float w[3])
{
    for (int k = 0; k < 3; k++) w[k] = l[0] * M[k] + l[1] * M[4 + k] + l[2] * M[8 + k] + M[12 + k];
}
static void dir_to_world(const float *M, const float l[3], float w[3])
{
    for (int k = 0; k < 3; k++) w[k] = l[0] * M[k] + l[1] * M[4 + k] + l[2] * M[8 + k];
}
// world direction -> model direction (inverse of the matrix's 3x3; rows of M are the model axes in the world)
static bool dir_to_local(const float *M, const float w[3], float l[3])
{
    float a[3][3];                                      // world = a * local
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) a[r][c] = M[4 * c + r];
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return false;
    float inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    for (int r = 0; r < 3; r++) l[r] = inv[r][0] * w[0] + inv[r][1] * w[1] + inv[r][2] * w[2];
    return true;
}
static bool to_local(const float *M, const float w[3], float l[3])
{
    float d[3] = {w[0] - M[12], w[1] - M[13], w[2] - M[14]};
    return dir_to_local(M, d, l);
}

static void notify_info();

static void end_session(const char *why)
{
    if (!g_on && !S.prop) return;
    if (why) mstatus("%s", why);
    g_on = false;
    S = Session();
    notify_info();
}

// ------------------------------------------------------------------------------------------------ journal
static void begin_op(const char *what)
{
    S.cur = Op();
    S.cur.what = what;
    S.in_op = true;
    S.opno++;
}
static void touch(int gi)
{
    if (!S.in_op || S.stamp[gi] == S.opno) return;
    S.stamp[gi] = S.opno;
    VSave v;
    v.gi = gi;
    memcpy(v.v, S.P[gi], stridep(gi));
    S.cur.v.push_back(v);
}
static void touch_tex(int t)
{
    for (auto &x : S.cur.tx) if (x.tex == t) return;
    const Tex &tx = S.texs[t];
    const uint8_t *d = *(uint8_t *const *)(tx.hdr + 4);
    S.cur.tx.push_back({t, std::vector<uint8_t>(d, d + tx.bytes)});
}
void d3d_texture_dirty(struct XPixelContainer *t);      // d3d_render.cpp
static void write_tex(int t, const uint8_t *bytes)
{
    Tex &tx = S.texs[t];
    memcpy(*(uint8_t **)(tx.hdr + 4), bytes, tx.bytes);
    d3d_texture_dirty((struct XPixelContainer *)tx.hdr);
    tx.rgba.clear();                                    // texture paint decodes it again
}
static void touch_sub(int s)
{
    for (auto &i : S.cur.ib) if (i.sub == s) return;
    Sub &b = S.subs[s];
    S.cur.ib.push_back({s, std::vector<WORD>(b.idx, b.idx + b.nidx), b.hidden});
}

static void update_bounds();
static void build_topology();
static void invalidate_all()
{
    for (auto &v : S.vbs) d3d_vb_invalidate((DWORD)v.data);
}

static void commit_op()
{
    if (!S.in_op) return;
    S.in_op = false;
    if (S.cur.v.empty() && S.cur.ib.empty() && S.cur.tx.empty()) return;
    S.undo.push_back(std::move(S.cur));
    S.cur = Op();
    S.redo.clear();
    S.dirty = true;
    update_bounds();
    notify_info();
}

// triangles of an index list (strip: odd ones flipped back so all share one winding; degenerate ones dropped)
static void list_tris(const std::vector<WORD> &ix, int prim, std::vector<std::array<WORD, 3>> &out)
{
    out.clear();
    auto add = [&](WORD a, WORD b, WORD c) { if (a != b && b != c && a != c) out.push_back({a, b, c}); };
    int n = (int)ix.size();
    if (prim == 6)
        for (int k = 0; k + 2 < n; k++) k & 1 ? add(ix[k], ix[k + 2], ix[k + 1]) : add(ix[k], ix[k + 1], ix[k + 2]);
    else if (prim == 7)
        for (int k = 1; k + 1 < n; k++) add(ix[0], ix[k], ix[k + 1]);
    else
        for (int k = 0; k + 2 < n; k += 3) add(ix[k], ix[k + 1], ix[k + 2]);
}

// triangles -> one strip with degenerate joins, winding kept (tools/xrnd.py tris_to_strip)
static std::vector<WORD> strip_of(const std::vector<std::array<WORD, 3>> &tris)
{
    std::vector<WORD> s;
    for (auto &t : tris) {
        if (!s.empty()) {
            WORD last = s.back();
            s.push_back(last), s.push_back(t[0]), s.push_back(t[0]);
            if ((s.size() - 1) % 2) s.push_back(t[0]);  // odd positions draw flipped: start on an even one
            s.push_back(t[1]), s.push_back(t[2]);
        } else {
            s.push_back(t[0]), s.push_back(t[1]), s.push_back(t[2]);
        }
    }
    return s;
}

static int triangle_count(const std::vector<WORD> &ix, int prim)
{
    std::vector<std::array<WORD, 3>> t;
    list_tris(ix, prim, t);
    return (int)t.size();
}

// a sub-mesh's indices. Same length: written in place. Back to the original length: into the file's own memory.
// Otherwise a buffer of the editor's (kept for the life of the process - the model may keep drawing from it) and
// the index entry points there: +4 indices, +0x0C count, +8 triangle count. The renderer keys index lists by
// address + count, so the new list shows on the next frame.
static void set_indices(int s, const std::vector<WORD> &v)
{
    Sub &b = S.subs[s];
    WORD *dst = (int)v.size() == b.nidx ? b.idx : v.size() == b.orig.size() ? b.idx0 : (WORD *)malloc(v.size() * 2 + 4);
    if (!dst || v.empty()) return;
    memcpy(dst, v.data(), v.size() * 2);
    if (dst != b.idx || (int)v.size() != b.nidx) {
        *(WORD **)(b.ibe + 4) = dst;
        *(int *)(b.ibe + 0x0C) = (int)v.size();
        *(int *)(b.ibe + 8) = triangle_count(v, b.prim);
        b.idx = dst;
        b.nidx = (int)v.size();
    }
}

static bool same_indices(const Sub &b, const std::vector<WORD> &v)
{
    return (int)v.size() == b.nidx && !memcmp(b.idx, v.data(), v.size() * 2);
}

// apply a journal entry, returning its inverse (the values it replaced)
static Op apply_op(const Op &op)
{
    Op inv;
    inv.what = op.what;
    for (auto it = op.v.rbegin(); it != op.v.rend(); ++it) {
        VSave cur;
        cur.gi = it->gi;
        memcpy(cur.v, S.P[it->gi], stridep(it->gi));
        inv.v.push_back(cur);
        memcpy(S.P[it->gi], it->v, stridep(it->gi));
    }
    for (auto &x : op.tx) {
        const Tex &tx = S.texs[x.tex];
        const uint8_t *d = *(uint8_t *const *)(tx.hdr + 4);
        inv.tx.push_back({x.tex, std::vector<uint8_t>(d, d + tx.bytes)});
        write_tex(x.tex, x.bytes.data());
    }
    for (auto &i : op.ib) {
        Sub &b = S.subs[i.sub];
        inv.ib.push_back({i.sub, std::vector<WORD>(b.idx, b.idx + b.nidx), b.hidden});
        set_indices(i.sub, i.idx);
        b.hidden = i.hidden;
    }
    if (!op.ib.empty()) build_topology();
    invalidate_all();
    return inv;
}

static void undo_redo(bool undo)
{
    auto &from = undo ? S.undo : S.redo;
    auto &to = undo ? S.redo : S.undo;
    if (from.empty()) { mstatus(undo ? "nothing to undo" : "nothing to redo"); return; }
    Op op = std::move(from.back());
    from.pop_back();
    to.push_back(apply_op(op));
    S.dirty = true;
    update_bounds();
    notify_info();
    mstatus("%s %s (%u vertices)", undo ? "undid" : "redid", to.back().what.c_str(), (unsigned)to.back().v.size());
}

// ------------------------------------------------------------------------------------------------ geometry
static void face_normal(const Tri &t, float n[3])      // area-weighted, oriented like the stored normals
{
    const float *a = posp(t.v[0]), *b = posp(t.v[1]), *c = posp(t.v[2]);
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    if (S.flip) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
}

static void mark_moved(int g)
{
    if (S.moved_mark[g]) return;
    S.moved_mark[g] = 1;
    S.moved.push_back(g);
}

static void move_group(int g, const float d[3])        // d in model space
{
    for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
        int gi = S.gmem[k];
        touch(gi);
        float *p = posp(gi);
        p[0] += d[0], p[1] += d[1], p[2] += d[2];
    }
    mark_moved(g);
}
static void set_group(int g, const float p[3])
{
    for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
        int gi = S.gmem[k];
        touch(gi);
        memcpy(posp(gi), p, 12);
    }
    mark_moved(g);
}
static const float *gpos(int g) { return posp(S.gmem[S.gstart[g]]); }

// normals of the moved groups and their neighbours: area-weighted face normals over the weld group's members
// whose ORIGINAL normal agrees with this vertex's (keeps hard edges hard - a crate's corner stays sharp)
static void renormal_moved()
{
    if (S.moved.empty()) return;
    std::vector<int> groups = S.moved;
    for (int g : S.moved)
        for (int k = S.nstart[g]; k < S.nstart[g + 1]; k++)
            if (!S.moved_mark[S.nbr[k]]) { S.moved_mark[S.nbr[k]] = 2; groups.push_back(S.nbr[k]); }
    for (int g : groups) S.moved_mark[g] = 0;
    S.moved.clear();
    if (!s_renormal) return;
    for (int g : groups)
        for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
            int gi = S.gmem[k];
            float n0[3], acc[3] = {0, 0, 0};
            unpack_n(S.nrm0[gi], n0);
            for (int m = S.gstart[g]; m < S.gstart[g + 1]; m++) {
                int gm = S.gmem[m];
                float nm[3];
                unpack_n(S.nrm0[gm], nm);
                if (gm != gi && nm[0] * n0[0] + nm[1] * n0[1] + nm[2] * n0[2] < 0.5f) continue;
                for (int t = S.tstart[gm]; t < S.tstart[gm + 1]; t++) {
                    const Tri &tr = S.tris[S.tlist[t]];
                    if (S.subs[tr.sub].hidden) continue;
                    float fn[3];
                    face_normal(tr, fn);
                    acc[0] += fn[0], acc[1] += fn[1], acc[2] += fn[2];
                }
            }
            float len = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            if (len < 1e-12f) continue;
            for (int c = 0; c < 3; c++) acc[c] /= len;
            uint32_t pn = pack_n(acc);
            if (pn != *nrmp(gi)) { touch(gi); *nrmp(gi) = pn; }
        }
}

static void sphere_of(const std::vector<int> &verts, float &r, float c[3])
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int gi : verts)
        for (int k = 0; k < 3; k++) lo[k] = (std::min)(lo[k], posp(gi)[k]), hi[k] = (std::max)(hi[k], posp(gi)[k]);
    for (int k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) / 2;
    float rr = 0;
    for (int gi : verts) {
        const float *p = posp(gi);
        float d = (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]);
        rr = (std::max)(rr, d);
    }
    r = sqrtf(rr);
}

static void refit_obbt();
static void update_bounds()                             // culling spheres + picking box follow the edit
{
    refit_obbt();
    for (auto &b : S.subs) {
        if (b.verts.empty()) continue;
        float r, c[3];
        sphere_of(b.verts, r, c);
        *(float *)(b.recp + 0x0C) = r;
        memcpy(b.recp + 0x10, c, 12);
    }
    std::vector<int> all;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int gi = 0; gi < S.nv; gi++) {
        if (S.vsub[gi] < 0) continue;
        all.push_back(gi);
        for (int k = 0; k < 3; k++) lo[k] = (std::min)(lo[k], posp(gi)[k]), hi[k] = (std::max)(hi[k], posp(gi)[k]);
    }
    if (all.empty()) return;
    float r, c[3];
    sphere_of(all, r, c);
    *(float *)(S.geom + 0x50) = r;
    memcpy(S.geom + 0x54, c, 12);
    *(float *)(S.mobj + 0x4C) = r;
    memcpy(S.mobj + 0x40, c, 12);
    memcpy(S.mobj + 0x5C, lo, 12);
    memcpy(S.mobj + 0x68, hi, 12);
    S.radius = r;
}

// ------------------------------------------------------------------------------------------------ start
static void build_topology()
{
    // weld groups: vertices at the same position (to 1e-4) move together
    std::vector<int> order(S.nv);
    std::vector<int64_t> q(S.nv * 3);
    for (int i = 0; i < S.nv; i++) {
        order[i] = i;
        for (int k = 0; k < 3; k++) q[i * 3 + k] = (int64_t)llroundf(S.pos0[i * 3 + k] * 10000.0f);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        for (int k = 0; k < 3; k++)
            if (q[a * 3 + k] != q[b * 3 + k]) return q[a * 3 + k] < q[b * 3 + k];
        return a < b;
    });
    S.grp.assign(S.nv, -1);
    S.gstart.clear();
    S.gmem.clear();
    for (int i = 0; i < S.nv; i++) {
        int v = order[i];
        bool same = i > 0 && q[v * 3] == q[order[i - 1] * 3] && q[v * 3 + 1] == q[order[i - 1] * 3 + 1] &&
                    q[v * 3 + 2] == q[order[i - 1] * 3 + 2];
        if (!same) S.gstart.push_back((int)S.gmem.size());
        S.grp[v] = (int)S.gstart.size() - 1;
        S.gmem.push_back(v);
    }
    S.ng = (int)S.gstart.size();
    S.gstart.push_back((int)S.gmem.size());

    // triangles (strips: odd triangles flipped back; degenerate ones skipped)
    S.tris.clear();
    S.vsub.assign(S.nv, -1);
    for (int s = 0; s < (int)S.subs.size(); s++) {
        Sub &b = S.subs[s];
        const Vb &vb = S.vbs[b.vb];
        auto add = [&](int a, int c, int d) {
            if (a >= vb.count || c >= vb.count || d >= vb.count || a == c || c == d || a == d) return;
            S.tris.push_back({{vb.base + a, vb.base + c, vb.base + d}, s});
        };
        const std::vector<WORD> ix = b.hidden ? b.orig : std::vector<WORD>(b.idx, b.idx + b.nidx);
        int n = (int)ix.size();
        if (b.prim == 6)
            for (int k = 0; k + 2 < n; k++) k & 1 ? add(ix[k], ix[k + 2], ix[k + 1]) : add(ix[k], ix[k + 1], ix[k + 2]);
        else if (b.prim == 7)
            for (int k = 1; k + 1 < n; k++) add(ix[0], ix[k], ix[k + 1]);
        else
            for (int k = 0; k + 2 < n; k += 3) add(ix[k], ix[k + 1], ix[k + 2]);
        std::vector<int> used;
        for (WORD i : ix) if (i < vb.count) used.push_back(vb.base + i);
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        for (int gi : used) S.vsub[gi] = s;
        b.verts = used;
    }
    // triangles per vertex
    S.tstart.assign(S.nv + 1, 0);
    for (auto &t : S.tris) for (int v : t.v) S.tstart[v + 1]++;
    for (int i = 0; i < S.nv; i++) S.tstart[i + 1] += S.tstart[i];
    S.tlist.assign(S.tstart[S.nv], 0);
    std::vector<int> fill(S.tstart.begin(), S.tstart.end() - 1);
    for (int t = 0; t < (int)S.tris.size(); t++) for (int v : S.tris[t].v) S.tlist[fill[v]++] = t;
    // group adjacency + wireframe edges
    std::vector<std::pair<int, int>> e;
    for (auto &t : S.tris)
        for (int k = 0; k < 3; k++) {
            int a = S.grp[t.v[k]], b = S.grp[t.v[(k + 1) % 3]];
            if (a == b) continue;
            e.push_back({a, b});
            e.push_back({b, a});
        }
    std::sort(e.begin(), e.end());
    e.erase(std::unique(e.begin(), e.end()), e.end());
    S.nstart.assign(S.ng + 1, 0);
    S.nbr.clear();
    S.edges.clear();
    for (auto &p : e) {
        S.nstart[p.first + 1]++;
        S.nbr.push_back(p.second);
        if (p.first < p.second) S.edges.push_back(p);
    }
    for (int g = 0; g < S.ng; g++) S.nstart[g + 1] += S.nstart[g];
    // winding vs stored normals
    int agree = 0, disagree = 0;
    S.flip = false;
    for (auto &t : S.tris) {
        float fn[3], ns[3] = {0, 0, 0};
        face_normal(t, fn);
        for (int v : t.v) {
            float n[3];
            unpack_n(S.nrm0[v], n);
            ns[0] += n[0], ns[1] += n[1], ns[2] += n[2];
        }
        float d = fn[0] * ns[0] + fn[1] * ns[1] + fn[2] * ns[2];
        if (d > 0) agree++; else if (d < 0) disagree++;
    }
    S.flip = disagree > agree;
}

// a material's textures: programs (+0x04 count, +0x08 pointers) -> +0x14 command stream; op 0x01 sub-op 0 binds
// the texture object at +8 (resolved by 0x219700)
static int add_texture(uint8_t *to)
{
    uint8_t *hdr = *(uint8_t **)(to + 0x14);
    for (int k = 0; k < (int)S.texs.size(); k++) if (S.texs[k].hdr == hdr) return k;
    if (!hdr || !*(uint8_t **)(hdr + 4)) return -1;
    const char *nm = *(const char **)(to + 0x10);
    Tex t;
    t.name = (uintptr_t)nm > 0x10000 ? std::string(nm, strnlen(nm, 127)) : "?";
    t.hdr = hdr;
    uint32_t fmt = *(uint32_t *)(hdr + 0xC);
    t.fmt = (fmt >> 8) & 0xFF;
    t.levels = (fmt >> 16) & 0xF;
    if (!t.levels) t.levels = 1;
    t.w = 1 << ((fmt >> 20) & 0xF);
    t.h = 1 << ((fmt >> 24) & 0xF);
    t.bytes = 0;
    if (t.fmt == 0x0C || t.fmt == 0x0F)
        for (int l = 0, w = t.w, h = t.h; l < t.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
            t.bytes += (size_t)((w + 3) / 4) * ((h + 3) / 4) * (t.fmt == 0x0C ? 8 : 16);
    if (t.bytes) {
        const uint8_t *d = *(uint8_t **)(hdr + 4);
        t.orig.assign(d, d + t.bytes);
    }
    S.texs.push_back(t);
    return (int)S.texs.size() - 1;
}

static void material_textures(uint8_t *mat, std::vector<int> &out)
{
    if (!mat) return;
    int nprog = *(int *)(mat + 4);
    for (int j = 0; j < nprog && j < 16; j++) {
        uint8_t *prog = ((uint8_t **)(mat + 8))[j];
        uint8_t *c = prog ? *(uint8_t **)(prog + 0x14) : nullptr;
        for (int n = 0; c && c[0] && n < 256; n++, c += c[1] > 4 ? c[1] : 4)
            if (c[0] == 0x01 && c[2] == 0 && c[1] >= 12)
                if (uint8_t *to = *(uint8_t **)(c + 8)) {
                    int k = add_texture(to);
                    if (k >= 0 && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
                }
    }
}

static void open_model(const Cmd &c, uint8_t *prop, uint8_t *mobj);

static void start(const Cmd &c)
{
    end_session(nullptr);
    uint8_t *prop = ed_live_prop(c.file, c.live);
    if (!prop) { mstatus("%s is not loaded in this level", c.name.c_str()); return; }
    if (*(const short *)(prop + 0x10) != 1) { mstatus("%s is not a static model (animated or an emitter)", c.name.c_str()); return; }
    uint8_t *inst = *(uint8_t **)(prop + 0x7C);
    open_model(c, prop, inst ? *(uint8_t **)(inst + 0x18) : nullptr);
}

// ------------------------------------------------------------------------------------------------ level geometry
// The scene's level (.scn "LEVEL:" -> <name>.pao, scene object type "!LVL") is an ordinary model object: model +0x1C
// pax object (+0xC file name, +0x40 OBBT), +0x28 geometry. Where the scene keeps it is found at run time: pointers
// out of the scene object (world map [0x417D64], battle [0x39D320]) are followed a few levels deep until one is a
// model whose pax file name ends in .pao. The path is logged.
static bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static const char *model_file(const uint8_t *m)          // a model object's pax file name, or null
{
    if (!readable(m, 0x74)) return nullptr;
    const uint8_t *pax = *(const uint8_t *const *)(m + 0x1C);
    if (!readable(pax, 0x50)) return nullptr;
    const char *nm = *(const char *const *)(pax + 0x0C);
    if (!readable(nm, 8)) return nullptr;
    size_t n = strnlen(nm, 260);
    if (n < 5 || n >= 260) return nullptr;
    return nm;
}

static uint8_t *find_level_model(uint32_t &scene_var, uint8_t *&scene, std::string &path)
{
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    int id = state ? *(const int *)(state + 8) : -1;
    scene_var = id == 7 ? 0x417D64 : id == 12 ? 0x39D320 : 0;
    scene = scene_var ? *(uint8_t **)scene_var : nullptr;
    if (!scene) return nullptr;
    struct Node { const uint8_t *p; int depth; std::string path; };
    std::vector<Node> q{{scene, 0, "scene"}};
    std::vector<const uint8_t *> seen{scene};
    for (size_t qi = 0; qi < q.size() && qi < 4000; qi++) {
        Node n = q[qi];
        if (!readable(n.p, 0x200)) continue;
        for (int o = 0; o < 0x200; o += 4) {
            const uint8_t *c = *(const uint8_t *const *)(n.p + o);
            if ((uintptr_t)c < 0x10000 || (uintptr_t)c & 3) continue;
            char step[32];
            _snprintf(step, sizeof step, "+%X", o);
            step[31] = 0;
            if (const char *nm = model_file(c)) {
                size_t len = strlen(nm);
                if (len > 4 && !_stricmp(nm + len - 4, ".pao") && readable(c + 0x28, 4) && *(void *const *)(c + 0x28) &&
                    *(void *const *)(*(const uint8_t *const *)(c + 0x1C) + 0x4C)) {
                    path = n.path + step + " -> " + nm;
                    return (uint8_t *)c;
                }
            }
            if (n.depth < 3 && std::find(seen.begin(), seen.end(), c) == seen.end()) {
                seen.push_back(c);
                q.push_back({c, n.depth + 1, n.path + step});
            }
        }
    }
    return nullptr;
}

static std::string level_rel_path(const char *f);   // below
static const UINT WM_MESH_LEVELRESET = WM_APP + 28;

static void reset_level()                               // game thread: which loose files are this scene's level
{
    uint32_t sv;
    uint8_t *scene;
    std::string path;
    uint8_t *m = find_level_model(sv, scene, path);
    if (!m) { mstatus("no level in this scene (world map or battle only)"); return; }
    if (g_on && S.level) end_session(nullptr);
    std::string *rel = new std::string(level_rel_path(model_file(m)));
    if (!g_mwnd || !PostMessageA(g_mwnd, WM_MESH_LEVELRESET, 0, (LPARAM)rel)) delete rel;
}

static void start_level()
{
    end_session(nullptr);
    uint32_t sv;
    uint8_t *scene;
    std::string path;
    uint8_t *m = find_level_model(sv, scene, path);
    if (!m) { mstatus("no level model found in this scene (world map or battle only)"); return; }
    port_log("mesh: level model %p via %s\n", m, path.c_str());
    std::string rel = level_rel_path(model_file(m));
    Cmd c{};
    c.model = rel;
    size_t sl = rel.find_last_of((char)0x5C);
    c.name = sl == std::string::npos ? rel : rel.substr(sl + 1);
    c.live = -1;
    open_model(c, nullptr, m);
    if (g_on) S.level = true, S.scene_var = sv, S.scene = scene;
}

// "$l\worldmap\\x.pao" / "$d\levels\..." / "d:\data\..." -> "levels\worldmap\x.pao" (relative to data)
static std::string level_rel_path(const char *f)
{
    std::string rel = f ? f : "";
    for (auto &ch : rel) if (ch == '/') ch = (char)0x5C;
    std::string low = rel;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    // path aliases the engine keeps in names: "$d\" = the data folder, "$l\" = data\levels ("$l\worldmap\\x.pao")
    size_t k = low.find("data\\");
    if (k != std::string::npos) rel = rel.substr(k + 5);
    else if (low.size() > 3 && low[0] == '$' && low[1] == 'l') rel = "levels\\" + rel.substr(3);
    else if (low.size() > 3 && low[0] == '$') rel = rel.substr(3);
    else if (low.size() > 3 && low[1] == ':') rel = rel.substr(3);
    std::string clean;                                  // doubled separators collapse
    for (char ch : rel) if (!(ch == (char)0x5C && !clean.empty() && clean.back() == (char)0x5C)) clean += ch;
    while (!clean.empty() && clean[0] == (char)0x5C) clean.erase(0, 1);
    return clean;
}

// Culling (OBBT, pax +0x40 -> {node block, map}): map = int16 entries (-n = node n, >= 0 = sub-mesh in list A
// then B order), node 0x40 = 3 rows (axis xyz, half extent) + centre + u16 first, u16 count. After an edit every node's
// box is fitted again along its own axes around what it holds, so moved geometry is not culled where it no longer is.
static void refit_obbt()
{
    if (!S.level) return;
    uint8_t *pax = *(uint8_t **)(S.mobj + 0x1C);
    uint32_t *ob = *(uint32_t **)(pax + 0x40);
    if (!ob) return;
    uint8_t *nodes = (uint8_t *)(uintptr_t)ob[0];
    int16_t *map = (int16_t *)(uintptr_t)ob[1];
    int mapn = (int)((nodes - (uint8_t *)map) / 2);
    if (mapn <= 0 || mapn > 1000000) return;
    std::vector<char> done;
    std::function<bool(int, float *, float *)> fit = [&](int n, float *lo_out, float *hi_out) -> bool {
        float *f = (float *)(nodes + n * 0x40);
        uint16_t first = *(uint16_t *)(nodes + n * 0x40 + 0x3C), cnt = *(uint16_t *)(nodes + n * 0x40 + 0x3E);
        const float *ax[3] = {f, f + 4, f + 8};
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
        auto add = [&](const float p[3]) {
            for (int k = 0; k < 3; k++) {
                float d = p[0] * ax[k][0] + p[1] * ax[k][1] + p[2] * ax[k][2];
                lo[k] = (std::min)(lo[k], d), hi[k] = (std::max)(hi[k], d);
            }
            any = true;
        };
        for (int e = first; e < first + cnt && e < mapn; e++) {
            int v = map[e];
            if (v >= 0) {
                int si = v < (int)S.listsub.size() ? S.listsub[v] : -1;
                if (si >= 0) for (int gi : S.subs[si].verts) add(posp(gi));
            } else if (-v != n && (size_t)-v < 100000) {
                float cl[3], ch[3];
                if (!fit(-v, cl, ch)) continue;
                const float *cf = (const float *)(nodes + (-v) * 0x40);
                for (int c = 0; c < 8; c++) {           // the child box's corners
                    float pt[3];
                    for (int k = 0; k < 3; k++)
                        pt[k] = cf[12 + k] + cf[0 + k] * (c & 1 ? cf[3] : -cf[3]) + cf[4 + k] * (c & 2 ? cf[7] : -cf[7]) +
                                cf[8 + k] * (c & 4 ? cf[11] : -cf[11]);
                    add(pt);
                }
            }
        }
        if (!any) return false;
        float mid[3];
        for (int k = 0; k < 3; k++) mid[k] = (lo[k] + hi[k]) / 2, f[4 * k + 3] = (hi[k] - lo[k]) / 2 + 0.01f;
        for (int k = 0; k < 3; k++) f[12 + k] = ax[0][k] * mid[0] + ax[1][k] * mid[1] + ax[2][k] * mid[2];
        if (lo_out) memcpy(lo_out, lo, 12), memcpy(hi_out, hi, 12);
        return true;
    };
    fit(0, nullptr, nullptr);
}

static void open_model(const Cmd &c, uint8_t *prop, uint8_t *mobj)
{
    uint8_t *pax = mobj ? *(uint8_t **)(mobj + 0x1C) : nullptr;
    if (!pax) { mstatus("%s: no model loaded", c.name.c_str()); return; }
    if (*(void **)(pax + 0x28)) { mstatus("%s: skinned models (characters) are not editable yet", c.name.c_str()); return; }
    uint8_t *doeg = *(uint8_t **)(pax + 0x4C), *geom = *(uint8_t **)(mobj + 0x28);
    if (!doeg || !geom) { mstatus("%s: the model has no render data", c.name.c_str()); return; }
    uint8_t *vbblk = *(uint8_t **)(doeg + 0x3C), *ibblk = *(uint8_t **)(doeg + 0x38), *subblk = *(uint8_t **)(doeg + 0x34);
    S = Session();
    S.file = c.file, S.model = c.model, S.name = c.name, S.live = c.live;
    S.prop = prop, S.mobj = mobj, S.geom = geom, S.doeg = doeg;
    int listpos = 0;
    for (int list = 0; list < 2; list++) {
        int n = *(int *)(geom + (list ? 0x28 : 0x1C));
        uint8_t **recs = *(uint8_t ***)(geom + (list ? 0x2C : 0x20));
        uint8_t **mats = *(uint8_t ***)(geom + (list ? 0x30 : 0x24));
        if (n <= 0 || !recs) continue;
        for (int i = 0; i < n; i++, listpos++) {
            S.listsub.resize(listpos + 1, -1);
            uint8_t *r = recs[i];
            uint8_t *vbe = r ? *(uint8_t **)(r + 4) : nullptr, *ibe = r ? *(uint8_t **)(r + 8) : nullptr;
            if (!vbe || !ibe) continue;
            int ve = (int)(vbe - vbblk) / 0x14;
            int vi = -1;
            for (int k = 0; k < (int)S.vbs.size(); k++) if (S.vbs[k].entry == ve) vi = k;
            if (vi < 0) {
                uint8_t *hdr = *(uint8_t **)vbe;
                Vb v;
                v.data = hdr ? *(uint8_t **)(hdr + 4) : nullptr;
                v.count = *(int *)(vbe + 4);
                v.stride = *(int *)(vbe + 0x10);
                v.entry = ve;
                v.base = 0;
                if (!v.data || v.count <= 0 || v.count > 65536 || v.stride < 16 || v.stride > 64) {
                    mstatus("%s: unexpected vertex buffer (count %d stride %d)", c.name.c_str(), v.count, v.stride);
                    S = Session();
                    return;
                }
                vi = (int)S.vbs.size();
                S.vbs.push_back(v);
            }
            Sub b;
            b.vb = vi;
            b.entry_ib = (int)(ibe - ibblk) / 0x14;
            b.rec = (int)(r - subblk) / 0x38;
            b.recp = r;
            b.idx = b.idx0 = *(WORD **)(ibe + 4);
            b.ibe = ibe;
            b.nidx = *(int *)(ibe + 0x0C);
            b.prim = *(int *)(ibe + 0x10);
            const char *nm = *(const char **)(r + 0x34);
            b.name = (uintptr_t)nm > 0x10000 ? std::string(nm, strnlen(nm, 63)) : "sub" + std::to_string(S.subs.size());
            if (!b.idx || b.nidx <= 0) continue;
            b.orig.assign(b.idx, b.idx + b.nidx);
            if (mats) material_textures(mats[i], b.tex);
            b.hidden = std::all_of(b.orig.begin(), b.orig.end(), [&](WORD w) { return w == b.orig[0]; });
            S.listsub[listpos] = (int)S.subs.size();
            S.subs.push_back(b);
        }
    }
    if (S.subs.empty()) { mstatus("%s: no sub-meshes found", c.name.c_str()); S = Session(); return; }
    S.nv = 0;
    for (auto &v : S.vbs) {
        v.base = S.nv;
        S.nv += v.count;
        for (int i = 0; i < v.count; i++) S.P.push_back(v.data + i * v.stride), S.vvb.push_back((int)(&v - &S.vbs[0]));
        v.orig.assign(v.data, v.data + (size_t)v.count * v.stride);
    }
    S.pos0.resize(S.nv * 3);
    S.nrm0.resize(S.nv);
    for (int i = 0; i < S.nv; i++) memcpy(&S.pos0[i * 3], posp(i), 12), S.nrm0[i] = *nrmp(i);
    S.vsub.assign(S.nv, -1);
    build_topology();
    S.gsel.assign(S.ng, 0.0f);
    S.stamp.assign(S.nv, 0);
    S.moved_mark.assign(S.ng, 0);
    S.radius = *(float *)(geom + 0x50);
    memcpy(S.geom0, geom + 0x50, 16);
    memcpy(S.msph0, mobj + 0x40, 16);
    memcpy(S.mbox0, mobj + 0x5C, 24);
    for (auto &b : S.subs) memcpy(b.rec0, b.recp + 0x0C, 16);
    if (!(S.radius > 0)) S.radius = 1;
    g_on = true;
    notify_info();
    mstatus("editing %s: %d sub-meshes, %d vertices (%d welded), %d triangles", S.model.c_str(), (int)S.subs.size(),
            S.nv, S.ng, (int)S.tris.size());
}

// ------------------------------------------------------------------------------------------------ save
static uint32_t rd32(const std::string &f, size_t o) { uint32_t v; memcpy(&v, f.data() + o, 4); return v; }

// The finished file goes to the editor thread (WM_MESH_SAVED): a private copy for this prop (default - the prop's
// MODELS line in its .prp is pointed at it, then the level reloads) or over the shared model itself.
// texture paint on a shared patch: the painted faces get their own copy of the texture (tools/paxsplit.py)
struct PaintSplit { int sub; std::vector<std::array<WORD, 3>> tris; std::string tex, new_tex; std::vector<uint8_t> data;
                    int fmt, w, h, mips; };
struct SaveJob {
    std::vector<PaintSplit> splits;
    std::string file, name, model, pax, ptx;
    int live;
    bool shared, reload;
    std::string merge_glb;                              // GLB to merge into the saved copy (tools/paxmerge.py)
    float at[3], fit;                                   // merge: model-space position, fit size (0 = as is)
    bool ground;                                        // merge: rest on its origin + centred
    std::string up;                                     // merge: tools/glb2pao.py --up (the GLB axis that is up)
    float turn, scale, lean_x, lean_y;                  // merge: the placement's turn / tilt (degrees), scale
    bool clear;                                         // merge: --clear-box (centre, 3 axes, half extents)
    float box[15];
    bool level;                                         // merge: --level (scans stood upright)
};
static const UINT WM_MESH_SAVED = WM_APP + 23;
bool port_read_game_file(const std::string &key, std::string &out);   // bec_read.cpp

// path relative to data. Read directly (loose copy, else gladius.bec) - NOT through the engine's loader
// 0x1AE670: that pumps the asynchronous file system from our frame hook, and the reload after Save then came up
// with no retail prop models (see bec_read.cpp)
static bool load_data(const std::string &rel, std::string &out)
{
    return port_read_game_file("data/" + rel, out);
}

// The index data block (+0x68) rebuilt from every index entry in order: the session's current lists, the file's
// own for entries outside it. Entry +4 offset, +8 triangles, +0x0C count; later blocks shift by the size change
// (header offsets, data size +0x0C, XRND chunk size) - every in-block pointer is block-relative, nothing else moves.
static bool rebuild_indices(std::string &f, size_t d, size_t xr, const std::string &key)
{
    size_t base = d + rd32(f, d + 4);
    uint32_t o68 = rd32(f, d + 0x68), nent = rd32(f, d + 0x20), next = 0xFFFFFFFF;
    for (int k = 0x2C; k < 0x7C; k += 4) {
        uint32_t o = rd32(f, d + k);
        if (o != 0xFFFFFFFF && o > o68 && o < next) next = o;
    }
    size_t xr_end = xr + rd32(f, xr + 4);
    size_t ib = base + o68, ib_end = next != 0xFFFFFFFF ? base + next : xr_end;
    if (o68 == 0xFFFFFFFF || ib_end < ib || ib_end > f.size()) { mstatus("%s: index block not found - not saved", key.c_str()); return false; }
    std::string blk;
    std::vector<uint32_t> off(nent), cnt(nent), tri(nent);
    size_t ents = base + rd32(f, d + 0x38);
    for (uint32_t e = 0; e < nent; e++) {
        size_t en = ents + e * 0x14;
        if (en + 0x14 > f.size()) { mstatus("%s: index entries out of range - not saved", key.c_str()); return false; }
        const Sub *sb = nullptr;
        for (auto &b : S.subs) if (b.entry_ib == (int)e) sb = &b;
        off[e] = (uint32_t)blk.size();
        if (sb) {
            blk.append((const char *)sb->idx, (size_t)sb->nidx * 2);
            cnt[e] = sb->nidx;
            tri[e] = triangle_count(std::vector<WORD>(sb->idx, sb->idx + sb->nidx), sb->prim);
        } else {
            uint32_t o = rd32(f, en + 4), c = rd32(f, en + 0x0C);
            if (ib + o + (size_t)c * 2 > ib_end) { mstatus("%s: index data out of range - not saved", key.c_str()); return false; }
            blk.append(&f[ib + o], (size_t)c * 2);
            cnt[e] = c, tri[e] = rd32(f, en + 8);
        }
        if (blk.size() & 2) blk.append(2, (char)0);     // each list starts on 4 bytes
    }
    size_t oldlen = ib_end - ib;
    while ((blk.size() - oldlen) & 3) blk.push_back(0);
    for (uint32_t e = 0; e < nent; e++) {
        size_t en = ents + e * 0x14;
        memcpy(&f[en + 4], &off[e], 4), memcpy(&f[en + 8], &tri[e], 4), memcpy(&f[en + 0x0C], &cnt[e], 4);
        for (auto &b : S.subs)                          // primitive (a fan may have become a strip)
            if (b.entry_ib == (int)e) memcpy(&f[en + 0x10], &b.prim, 4);
    }
    int delta = (int)blk.size() - (int)oldlen;
    f.replace(ib, oldlen, blk);
    if (delta) {
        for (int k = 0x2C; k < 0x7C; k += 4) {
            uint32_t o = rd32(f, d + k);
            if (o != 0xFFFFFFFF && o > o68) o += delta, memcpy(&f[d + k], &o, 4);
        }
        uint32_t ds = rd32(f, d + 0x0C) + delta, cs = rd32(f, xr + 4) + delta;
        memcpy(&f[d + 0x0C], &ds, 4), memcpy(&f[xr + 4], &cs, 4);
    }
    return true;
}

// Vertex colours for a model without them: every stride-24 vertex buffer (position, normal, UV) becomes stride 28
// (position, normal, colour, UV) with white (multiplies 1:1 - the look does not change), the vertex data block
// (+0x6C) is rebuilt with the D3D headers' Data offsets (+4, relative to that block) and the entries' strides
// (+0x10) updated, and every vertex-format command (op 0x09, format word at +4) gets the colour bit 0x10.
// Later blocks shift like rebuild_indices. Save-time only: in memory the engine has replaced the format word with a
// vertex shader handle (0x20EF40), so the model reloads afterwards.
static bool add_vertex_colors(std::string &f, size_t d, size_t xr, const std::string &key)
{
    size_t base = d + rd32(f, d + 4);
    uint32_t o6c = rd32(f, d + 0x6C), nvb = rd32(f, d + 0x1C), next = 0xFFFFFFFF;
    for (int k = 0x2C; k < 0x7C; k += 4) {
        uint32_t o = rd32(f, d + k);
        if (o != 0xFFFFFFFF && o > o6c && o < next) next = o;
    }
    size_t vb = base + o6c, vb_end = next != 0xFFFFFFFF ? base + next : xr + rd32(f, xr + 4);
    size_t ents = base + rd32(f, d + 0x3C), hdrs = base + rd32(f, d + 0x40);
    if (o6c == 0xFFFFFFFF || vb_end < vb || vb_end > f.size()) { mstatus("%s: vertex block not found", key.c_str()); return false; }
    std::string blk;
    for (uint32_t e = 0; e < nvb; e++) {
        size_t en = ents + e * 0x14;
        uint32_t ho = rd32(f, en), cnt = rd32(f, en + 4), st = rd32(f, en + 0x10);
        size_t h = hdrs + ho, src = vb + rd32(f, h + 4);
        if (src + (size_t)cnt * st > vb_end) { mstatus("%s: vertex buffer %u out of range", key.c_str(), e); return false; }
        uint32_t at = (uint32_t)blk.size();
        memcpy(&f[h + 4], &at, 4);
        if (st == 24) {
            static const char white[4] = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
            for (uint32_t i = 0; i < cnt; i++) {
                const char *v = &f[src + (size_t)i * 24];
                blk.append(v, 16), blk.append(white, 4), blk.append(v + 16, 8);
            }
            uint32_t ns = 28;
            memcpy(&f[en + 0x10], &ns, 4);
        } else {
            blk.append(&f[src], (size_t)cnt * st);
        }
        while (blk.size() & 3) blk.push_back(0);
    }
    size_t oldlen = vb_end - vb;
    while ((blk.size() - oldlen) & 3) blk.push_back(0);
    // vertex formats: the command block (+0x64) holds every program's stream, ops (byte op, byte length), 0 = end
    uint32_t o64 = rd32(f, d + 0x64), n64 = 0xFFFFFFFF;
    for (int k = 0x2C; k < 0x7C; k += 4) {
        uint32_t o = rd32(f, d + k);
        if (o != 0xFFFFFFFF && o > o64 && o < n64) n64 = o;
    }
    if (o64 != 0xFFFFFFFF && n64 != 0xFFFFFFFF)
        for (size_t c = base + o64, end = base + n64; c + 2 <= end;) {
            uint8_t op = (uint8_t)f[c], ln = (uint8_t)f[c + 1];
            if (op == 0x09 && ln >= 8 && c + 8 <= end) {
                uint32_t fmt = rd32(f, c + 4) | 0x10;
                memcpy(&f[c + 4], &fmt, 4);
            }
            c += op == 0 || ln < 4 ? 4 : ln;
        }
    int delta = (int)blk.size() - (int)oldlen;
    f.replace(vb, oldlen, blk);
    if (delta) {
        for (int k = 0x2C; k < 0x7C; k += 4) {
            uint32_t o = rd32(f, d + k);
            if (o != 0xFFFFFFFF && o > o6c) o += delta, memcpy(&f[d + k], &o, 4);
        }
        uint32_t ds = rd32(f, d + 0x0C) + delta, cs = rd32(f, xr + 4) + delta;
        memcpy(&f[d + 0x0C], &ds, 4), memcpy(&f[xr + 4], &cs, 4);
    }
    return true;
}

static std::string lower(std::string s);
static std::string private_model(const std::string &model, const std::string &prop);

// edited textures into the model's .ptx (PTTP / NAME / NMPT / PFHD / PADD / PTDT, see tools/ptx.py): PFHD entry
// (32 bytes) u16 format (0x4100 DXT1, 0x4300 DXT5), u16 mips, u32 0, u16 w, u16 h, u32 size, u32 data offset
// relative to the entry, u32 name offset in NMPT. Same size and format only, so the levels overwrite in place.
static std::string tex_key(std::string n)                // "Foo.TGA" -> "foo"
{
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && n.size() - dot <= 4) n.erase(dot);
    return n;
}

static std::vector<int> g_ptx_skip;                     // textures patch_ptx leaves alone (split ones keep the original)
static bool patch_ptx(std::string &ptx)
{
    size_t p = 0, nmpt = 0, pfhd = 0;
    uint32_t nentries = 0;
    while (p + 16 <= ptx.size()) {
        uint32_t cs = rd32(ptx, p + 4);
        if (!memcmp(&ptx[p], "NMPT", 4)) nmpt = p + 16;
        if (!memcmp(&ptx[p], "PFHD", 4)) pfhd = p + 16, nentries = rd32(ptx, p + 12);
        if (!cs || !memcmp(&ptx[p], "END.", 4)) break;
        p += cs;
    }
    if (!nmpt || !pfhd) { mstatus("the .ptx has no NMPT / PFHD chunk - textures not saved"); return false; }
    for (auto &t : S.texs) {
        if (!t.bytes || !memcmp(*(uint8_t **)(t.hdr + 4), t.orig.data(), t.bytes)) continue;
        if (std::find(g_ptx_skip.begin(), g_ptx_skip.end(), (int)(&t - &S.texs[0])) != g_ptx_skip.end()) continue;
        bool done = false;
        for (uint32_t i = 0; i < nentries && !done; i++) {
            size_t e = pfhd + i * 32;
            if (e + 32 > ptx.size()) break;
            size_t no = nmpt + rd32(ptx, e + 20);
            if (no >= ptx.size()) continue;
            std::string nm(&ptx[no], strnlen(&ptx[no], ptx.size() - no));
            if (tex_key(nm) != tex_key(t.name)) continue;
            uint16_t fmt, mips, w, h;
            memcpy(&fmt, &ptx[e], 2), memcpy(&mips, &ptx[e + 2], 2), memcpy(&w, &ptx[e + 8], 2), memcpy(&h, &ptx[e + 10], 2);
            size_t dst = e + rd32(ptx, e + 16);
            // format codes: 0x4100 / Xbox 0x2000 = DXT1, 0x4300 / Xbox 0x2200 = DXT5 (Protius Converter-Arena-Map.py)
            bool fmt_ok = t.fmt == 0x0C ? fmt == 0x4100 || fmt == 0x2000 : fmt == 0x4300 || fmt == 0x2200;
            bool same = w == t.w && h == t.h && mips == t.levels && fmt_ok;
            if (!same || dst + t.bytes > ptx.size()) {
                mstatus("texture %s in the .ptx is %ux%u fmt %04X x%u, in memory %dx%d - not saved", t.name.c_str(), w, h,
                        fmt, mips, t.w, t.h);
                return false;
            }
            memcpy(&ptx[dst], *(uint8_t **)(t.hdr + 4), t.bytes);
            done = true;
        }
        if (!done) { mstatus("texture %s not found in the .ptx - not saved", t.name.c_str()); return false; }
    }
    return true;
}

struct MergeReq { std::string glb; float fit; bool ground; bool has_at; float at[3]; std::string up; bool level;
                  float turn, scale, lean_x, lean_y, lift; bool clear; float box[15]; };

static const float *uvp(int gi);
static std::array<WORD, 3> canon_tri(WORD a, WORD b, WORD c);

// Does any face the brush did NOT touch read a changed texel? Changed = the painted top level differs from the
// loaded one; each untouched face's UV triangle is rasterised over that mask (UVs wrap). If so, the touched faces
// of every sub-mesh drawing that texture go to their own copy (the shared texture is saved unpainted).
static void paint_splits(std::vector<PaintSplit> &out)
{
    for (int ti = 0; ti < (int)S.texs.size(); ti++) {
        Tex &t = S.texs[ti];
        if (!t.bytes || !memcmp(*(uint8_t **)(t.hdr + 4), t.orig.data(), t.bytes)) continue;
        bool painted = false;
        for (auto &b : S.subs)
            if (!b.painted.empty() && std::find(b.tex.begin(), b.tex.end(), ti) != b.tex.end()) painted = true;
        if (!painted) continue;                         // replaced / restored, not painted: saved as it is
        std::vector<uint8_t> a, c;
        dxt_decode(t.orig.data(), t.w, t.h, t.fmt, a);
        dxt_decode(*(uint8_t **)(t.hdr + 4), t.w, t.h, t.fmt, c);
        std::vector<char> mask((size_t)t.w * t.h);
        for (size_t i = 0; i < mask.size(); i++)
            for (int k = 0; k < 4; k++) mask[i] |= abs((int)a[i * 4 + k] - (int)c[i * 4 + k]) > 3;
        bool conflict = false;
        for (auto &b : S.subs) {
            if (conflict || b.hidden || std::find(b.tex.begin(), b.tex.end(), ti) == b.tex.end()) continue;
            std::vector<std::array<WORD, 3>> tris;
            list_tris(std::vector<WORD>(b.idx, b.idx + b.nidx), b.prim, tris);
            const Vb &vb = S.vbs[b.vb];
            for (auto &tr : tris) {
                if (b.painted.count(canon_tri(tr[0], tr[1], tr[2]))) continue;
                float px[3], py[3];
                bool ok = true;
                for (int k = 0; k < 3; k++) {
                    if (tr[k] >= vb.count) { ok = false; break; }
                    const float *uv = uvp(vb.base + tr[k]);
                    px[k] = uv[0] * t.w, py[k] = uv[1] * t.h;
                }
                if (!ok) continue;
                float x0 = floorf((std::min)({px[0], px[1], px[2]})), x1 = ceilf((std::max)({px[0], px[1], px[2]}));
                float y0 = floorf((std::min)({py[0], py[1], py[2]})), y1 = ceilf((std::max)({py[0], py[1], py[2]}));
                if ((x1 - x0) * (y1 - y0) > 4.0f * t.w * t.h) { conflict = true; break; }   // tiles the whole texture
                float d = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
                if (fabsf(d) < 1e-9f) continue;
                for (int y = (int)y0; y <= (int)y1 && !conflict; y++)
                    for (int x = (int)x0; x <= (int)x1; x++) {
                        float cx = x + 0.5f, cy = y + 0.5f;
                        float l0 = ((py[1] - py[2]) * (cx - px[2]) + (px[2] - px[1]) * (cy - py[2])) / d;
                        float l1 = ((py[2] - py[0]) * (cx - px[2]) + (px[0] - px[2]) * (cy - py[2])) / d;
                        if (l0 < 0 || l1 < 0 || l0 + l1 > 1) continue;
                        int wx = ((x % t.w) + t.w) % t.w, wy = ((y % t.h) + t.h) % t.h;
                        if (mask[(size_t)wy * t.w + wx]) { conflict = true; break; }
                    }
                if (conflict) break;
            }
        }
        if (!conflict) continue;
        // a free name: <texture>_u<n>, at most 31 characters
        std::string nn;
        // the suffix goes before the first dot ("PROPSHIP.CC" -> "PROPSHIP_u1.CC"): the engine's name compare
        // (0x1DB420) treats what follows a dot specially - "PROPSHIP.CC_u1" never found its .ptx entry
        size_t dot = t.name.find('.');
        std::string stem = dot == std::string::npos ? t.name : t.name.substr(0, dot);
        std::string ext = dot == std::string::npos ? "" : t.name.substr(dot);
        for (int n = 1;; n++) {
            std::string suf = "_u" + std::to_string(n);
            nn = stem.substr(0, 31 - suf.size() - (std::min)(ext.size(), (size_t)8)) + suf + ext.substr(0, 8);
            bool used = false;
            for (auto &x : S.texs) used |= _stricmp(x.name.c_str(), nn.c_str()) == 0;
            for (auto &x : out) used |= _stricmp(x.new_tex.c_str(), nn.c_str()) == 0;
            if (!used) break;
        }
        for (auto &b : S.subs) {
            if (b.painted.empty() || std::find(b.tex.begin(), b.tex.end(), ti) == b.tex.end()) continue;
            std::vector<std::array<WORD, 3>> tris, mine;
            list_tris(std::vector<WORD>(b.idx, b.idx + b.nidx), b.prim, tris);
            for (auto &tr : tris) if (b.painted.count(canon_tri(tr[0], tr[1], tr[2]))) mine.push_back(tr);
            if (mine.empty()) continue;
            const uint8_t *d = *(uint8_t **)(t.hdr + 4);
            out.push_back({b.rec, mine, t.name, nn, std::vector<uint8_t>(d, d + t.bytes), t.fmt == 0x0C ? 0x4100 : 0x4300,
                           t.w, t.h, t.levels});
        }
        g_ptx_skip.push_back(ti);
        mstatus("%s: painted faces share texels with other faces - they get their own copy %s on Save", t.name.c_str(), nn.c_str());
    }
}
static int ray_pick(const CamFrame &cf, const float *M, float o[3], float d[3], float &best, float &bu, float &bv);

// faces the predicate picks leave their sub-mesh (re-stripped; a sub-mesh left empty is hidden) - inside an open op.
// -> faces deleted. (Delete faces, and Merge GLB's "clear what it overlaps".)
static int delete_faces_where(const std::function<bool(const Vb &, const std::array<WORD, 3> &)> &doomed)
{
    int gone = 0;
    for (int si = 0; si < (int)S.subs.size(); si++) {
        Sub &b = S.subs[si];
        if (b.hidden) continue;
        const Vb &vb = S.vbs[b.vb];
        std::vector<std::array<WORD, 3>> tris, keep;
        list_tris(std::vector<WORD>(b.idx, b.idx + b.nidx), b.prim, tris);
        for (auto &t : tris)
            if (!doomed(vb, t)) keep.push_back(t);
        if (keep.size() == tris.size()) continue;
        gone += (int)(tris.size() - keep.size());
        touch_sub(si);
        if (keep.empty()) {                             // nothing left: one degenerate triangle keeps the entry valid
            WORD z = b.idx[0];
            set_indices(si, std::vector<WORD>{z, z, z});
            b.hidden = true;
            b.shown.clear();
            continue;
        }
        std::vector<WORD> v;
        if (b.prim == 5)
            for (auto &t : keep) v.insert(v.end(), t.begin(), t.end());
        else
            v = strip_of(keep);
        if (b.prim == 7) b.prim = 6, *(int *)(b.ibe + 0x10) = 6;   // a fan becomes a strip
        set_indices(si, v);
    }
    return gone;
}

static void save(bool shared, bool add_colors = false, const MergeReq *merge = nullptr)
{
    if (S.level) {
        if (add_colors) { mstatus("adding vertex colours is for props - not levels"); return; }
        shared = true;                                  // a level is saved over its own (loose) .pao
    }
    std::string rel = S.model;
    std::replace(rel.begin(), rel.end(), '/', (char)0x5C);
    std::string key = "data/" + S.model;
    std::replace(key.begin(), key.end(), (char)0x5C, '/');
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    std::string f;
    if (!load_data(rel, f)) { mstatus("could not read %s", key.c_str()); return; }
    size_t p = 0, xr = 0;
    while (p + 16 <= f.size()) {
        uint32_t cs = rd32(f, p + 4);
        if (!memcmp(f.data() + p, "XRND", 4)) { xr = p; break; }
        if (!cs) break;
        p += cs;
    }
    if (!xr) { mstatus("%s: no XRND chunk", key.c_str()); return; }
    size_t d = xr + 16;
    if (rd32(f, xr + 8) == 1) d += 4 + 4 * (size_t)rd32(f, d);
    if (d + 0x7C > f.size() || memcmp(f.data() + d, S.doeg, 4)) { mstatus("%s: geometry header not found", key.c_str()); return; }
    size_t base = d + rd32(f, d + 4);
    auto blk = [&](int o) { return base + rd32(f, d + o); };
    auto in = [&](size_t o, size_t n) { return o + n <= f.size(); };
    for (auto &v : S.vbs) {
        size_t e = blk(0x3C) + v.entry * 0x14;
        if (!in(e, 0x14) || (int)rd32(f, e + 4) != v.count || (int)rd32(f, e + 0x10) != v.stride) {
            mstatus("%s: vertex buffer %d does not match the file - not saved", key.c_str(), v.entry);
            return;
        }
        size_t h = blk(0x40) + rd32(f, e);
        size_t dst = blk(0x6C) + rd32(f, h + 4);
        if (!in(h, 8) || !in(dst, (size_t)v.count * v.stride)) { mstatus("%s: vertex data out of range - not saved", key.c_str()); return; }
        memcpy(&f[dst], v.data, (size_t)v.count * v.stride);
    }
    // spheres first (their blocks lie before the index data in every model, but positions are taken before any
    // block moves anyway)
    for (auto &b : S.subs) {
        size_t r = blk(0x34) + b.rec * 0x38;
        if (in(r, 0x38)) memcpy(&f[r + 0x0C], b.recp + 0x0C, 16);   // sphere radius + centre
    }
    size_t g = blk(0x2C);
    if (in(g, 0x70)) memcpy(&f[g + 0x50], S.geom + 0x50, 16);
    if (S.level) {                                      // the culling tree as fitted in memory (no pointers inside)
        uint8_t *pax = *(uint8_t **)(S.mobj + 0x1C);
        uint32_t *ob = *(uint32_t **)(pax + 0x40);
        size_t q = 0;
        while (ob && q + 16 <= f.size()) {
            uint32_t cs = rd32(f, q + 4);
            if (!memcmp(&f[q], "OBBT", 4)) {
                size_t mapb = rd32(f, q + 12), nodes_bytes = cs - 16 - mapb;
                const uint8_t *mem_nodes = (const uint8_t *)(uintptr_t)ob[0];
                if ((size_t)(mem_nodes - (const uint8_t *)(uintptr_t)ob[1]) == mapb && q + cs <= f.size())
                    memcpy(&f[q + 16 + mapb], mem_nodes, nodes_bytes & ~(size_t)0x3F);
                else
                    port_log("mesh: OBBT layout differs from memory - culling tree not saved\n");
                break;
            }
            if (!cs) break;
            q += cs;
        }
    }
    if (!rebuild_indices(f, d, xr, key)) return;
    if (add_colors && !add_vertex_colors(f, d, xr, key)) return;
    std::vector<PaintSplit> splits;
    g_ptx_skip.clear();
    if (!shared && !add_colors && !merge) paint_splits(splits);
    SaveJob *job = new SaveJob;
    job->file = S.file, job->name = S.name, job->model = S.model, job->live = S.live, job->shared = shared;
    job->reload = add_colors || merge || !splits.empty();
    job->splits.swap(splits);
    if (merge) {
        job->merge_glb = merge->glb, job->fit = merge->fit, job->ground = merge->ground;
        job->up = merge->up, job->level = merge->level, job->turn = merge->turn, job->scale = merge->scale;
        job->lean_x = merge->lean_x, job->lean_y = merge->lean_y;
        job->clear = merge->clear;
        memcpy(job->box, merge->box, sizeof job->box);
        if (merge->has_at) memcpy(job->at, merge->at, 12);
        float c[3] = {0, 0, 0};                         // the selection's centre (model space), else the origin
        int n = merge->has_at ? -1 : 0;                 // (a clicked point wins)
        for (int g = 0; g < S.ng && n >= 0; g++)
            if (S.gsel[g] > 0) { for (int k = 0; k < 3; k++) c[k] += gpos(g)[k]; n++; }
        if (n >= 0) for (int k = 0; k < 3; k++) job->at[k] = n ? c[k] / n : 0.0f;
        CamFrame cf;
        const float *M = world_matrix();
        if (!n && M && ed_cam_frame(cf)) {             // nothing selected: where the screen's centre hits the model
            float o[3], d[3], best, bu, bv, nx = G.nx, ny = G.ny;
            G.nx = G.ny = 0;
            if (ray_pick(cf, M, o, d, best, bu, bv) >= 0)
                for (int k = 0; k < 3; k++) job->at[k] = o[k] + d[k] * best;
            G.nx = nx, G.ny = ny;
        }
    }
    job->pax.swap(f);
    bool tex_changed = false;
    for (auto &t : S.texs)
        tex_changed |= t.bytes && memcmp(*(uint8_t **)(t.hdr + 4), t.orig.data(), t.bytes) != 0;
    if (!shared || tex_changed || merge) {              // the copy (or the merge) needs the texture bundle
        std::string ptx = rel.size() > 4 ? rel.substr(0, rel.size() - 4) + ".ptx" : rel;
        if (!load_data(ptx, job->ptx)) {
            mstatus("could not read %s - not saved", ptx.c_str());
            delete job;
            return;
        }
        if (tex_changed && !patch_ptx(job->ptx)) { delete job; return; }
    }
    S.dirty = false;
    if (!shared && private_model(S.model, S.name) != lower(S.model)) {
        // a NEW copy for this prop: the shared model in memory goes back to what it was, so the other props using
        // it look right whether or not the engine keeps the model cached over the reload. (A prop that already has
        // its own copy keeps showing the edit - nothing else uses that model.)
        // Only what differs is written back (like an undo), and the bounds go back to the values the engine
        // loaded: a full rewrite (every vertex + every index list + recomputed bounds) preceded the two reloads
        // that came up with no props drawn (2026-10-11).
        for (int gi = 0; gi < S.nv; gi++)
            if (memcmp(S.P[gi], origp(gi), stridep(gi))) memcpy(S.P[gi], origp(gi), stridep(gi));
        for (int t = 0; t < (int)S.texs.size(); t++)
            if (S.texs[t].bytes && memcmp(*(uint8_t **)(S.texs[t].hdr + 4), S.texs[t].orig.data(), S.texs[t].bytes))
                write_tex(t, S.texs[t].orig.data());
        for (auto &b : S.subs)
            if (!same_indices(b, b.orig)) set_indices((int)(&b - &S.subs[0]), b.orig);
        for (auto &b : S.subs) memcpy(b.recp + 0x0C, b.rec0, 16);
        memcpy(S.geom + 0x50, S.geom0, 16);
        memcpy(S.mobj + 0x40, S.msph0, 16);
        memcpy(S.mobj + 0x5C, S.mbox0, 24);
        invalidate_all();
        end_session(nullptr);
    }
    if ((add_colors || merge || !job->splits.empty()) && g_on) end_session(nullptr);
    notify_info();
    if (!g_mwnd || !PostMessageA(g_mwnd, WM_MESH_SAVED, 0, (LPARAM)job)) delete job;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// model "mesh\misc\rocks\rock_gen_01.pax" + prop "Rock03" -> "mesh\custom\rock_gen_01_rock03.pax", the prop's own
// copy (a model that already is this prop's copy keeps its name: saved again in place)
static std::string private_model(const std::string &model, const std::string &prop)
{
    const char bs = (char)0x5C;
    std::string m = model;
    std::replace(m.begin(), m.end(), '/', bs);
    size_t slash = m.find_last_of(bs);
    std::string base = m.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4) base.erase(base.size() - 4);
    std::string own = "_";
    for (char c : prop) own += isalnum((unsigned char)c) || c == '-' ? (char)tolower((unsigned char)c) : '_';
    if (base.size() >= own.size() && lower(base).compare(base.size() - own.size(), own.size(), own) == 0)
        own.clear();
    return lower(std::string("mesh") + bs + "custom" + bs + base + own + ".pax");
}

static std::string data_key(const std::string &rel)
{
    std::string k = "data/" + rel;
    std::replace(k.begin(), k.end(), (char)0x5C, '/');
    return lower(k);
}

// Merge: python tools/paxmerge.py <the prop's copy> <part.glb> --at x y z [--fit s] [--ground --center], run on a
// worker thread (the window keeps painting); its output goes to paxmerge.log next to the exe. Tools folder: the
// GLADIUS_TOOLS environment variable, else <exe folder>..\tools.
static const UINT WM_MESH_MERGED = WM_APP + 25;
struct MergeRun { SaveJob *job; std::string pax_path, own; DWORD exit; std::string log, split_json; };
static void finish_after_merge(SaveJob *job, const std::string &own);

static std::string exe_dir()
{
    char b[MAX_PATH];
    GetModuleFileNameA(nullptr, b, MAX_PATH);
    std::string s(b);
    size_t k = s.find_last_of((char)0x5C);
    return k == std::string::npos ? "." : s.substr(0, k);
}

static DWORD WINAPI merge_thread(void *arg)
{
    MergeRun *r = (MergeRun *)arg;
    char env[MAX_PATH] = "";
    std::string tools = GetEnvironmentVariableA("GLADIUS_TOOLS", env, MAX_PATH) ? std::string(env)
                                                                                 : exe_dir() + "\\..\\tools";
    char at[160];
    _snprintf(at, sizeof at, " --at %.5f %.5f %.5f", r->job->at[0], r->job->at[1], r->job->at[2]);
    at[sizeof at - 1] = 0;
    std::string cmd;
    if (!r->split_json.empty()) {
        cmd = "python \"" + tools + "\\paxsplit.py\" \"" + r->pax_path + "\" \"" + r->split_json + "\"";
    } else {
        cmd = "python \"" + tools + "\\paxmerge.py\" \"" + r->pax_path + "\" \"" + r->job->merge_glb + "\"" + at;
        if (r->job->fit > 0) { char f[48]; _snprintf(f, sizeof f, " --fit %.4f", r->job->fit); f[47] = 0; cmd += f; }
        if (r->job->ground) cmd += " --ground --center";
        if (!r->job->up.empty()) cmd += " --up=" + r->job->up;
        if (r->job->level) cmd += " --level";
        if (r->job->turn != 0) { char t[48]; _snprintf(t, sizeof t, " --turn %.3f", r->job->turn); t[47] = 0; cmd += t; }
        if (r->job->scale != 1) { char t[48]; _snprintf(t, sizeof t, " --scale %.5f", r->job->scale); t[47] = 0; cmd += t; }
        if (r->job->clear) {                            // faces inside the part's box removed, crossing ones cut
            std::string b = " --clear-box=";
            for (int i = 0; i < 15; i++) {
                char t[32];
                _snprintf(t, sizeof t, "%s%.5f", i ? "," : "", r->job->box[i]);
                t[31] = 0;
                b += t;
            }
            cmd += b;
        }
        if (r->job->lean_x != 0 || r->job->lean_y != 0) {
            char t[64];
            _snprintf(t, sizeof t, " --lean=%.3f,%.3f", r->job->lean_x, r->job->lean_y);
            t[63] = 0;
            cmd += t;
        }
    }
    std::string logp = exe_dir() + "\\paxmerge.log";
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileA(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    port_log("mesh: merge: %s\n", cmd.c_str());
    std::vector<char> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    r->exit = 0xFFFFFFFF;
    if (CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 180000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &r->exit);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        r->log = "could not start python (is it on PATH?)";
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (r->log.empty()) {                               // the log's last line for the status
        HANDLE f = CreateFileA(logp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char b[4096];
            DWORD n = 0;
            DWORD sz = GetFileSize(f, nullptr);
            if (sz > sizeof b - 1) SetFilePointer(f, sz - (sizeof b - 1), nullptr, FILE_BEGIN);
            ReadFile(f, b, sizeof b - 1, &n, nullptr);
            b[n] = 0;
            CloseHandle(f);
            std::string t(b);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
            size_t k = t.find_last_of('\n');
            r->log = k == std::string::npos ? t : t.substr(k + 1);
        }
    }
    if (!g_mwnd || !PostMessageA(g_mwnd, WM_MESH_MERGED, 0, (LPARAM)r)) { delete r->job; delete r; }
    return 0;
}

// Merge GLB... -> python paxmerge.py none <glb> --preview <file> (same shaping options): the wireframe the
// placement shows. Done -> WM_MESH_PREVIEWED -> C_MERGE with the preview bytes.
static const UINT WM_MESH_PREVIEWED = WM_APP + 27;
struct PreviewRun { Cmd c; std::string out; DWORD exit; };
static DWORD WINAPI preview_thread(void *arg)
{
    PreviewRun *r = (PreviewRun *)arg;
    char env[MAX_PATH] = "";
    std::string tools = GetEnvironmentVariableA("GLADIUS_TOOLS", env, MAX_PATH) ? std::string(env)
                                                                                 : exe_dir() + "\\..\\tools";
    r->out = exe_dir() + "\\merge_preview.bin";
    DeleteFileA(r->out.c_str());
    std::string cmd = "python \"" + tools + "\\paxmerge.py\" none \"" + r->c.file + "\" --preview \"" + r->out + "\"";
    if (r->c.f[0] > 0) { char f[48]; _snprintf(f, sizeof f, " --fit %.4f", r->c.f[0]); f[47] = 0; cmd += f; }
    if (r->c.add) cmd += " --ground --center";
    if (!r->c.name.empty()) cmd += " --up=" + r->c.name;
    if (r->c.f[1] != 0) cmd += " --level";
    std::string logp = exe_dir() + "\\paxmerge.log";
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileA(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    r->exit = 0xFFFFFFFF;
    if (CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 180000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &r->exit);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!g_mwnd || !PostMessageA(g_mwnd, WM_MESH_PREVIEWED, 0, (LPARAM)r)) delete r;
    return 0;
}

static void finish_save(SaveJob *job)                   // editor thread: write the files, repoint the prop
{
    if (job->shared) {
        std::string key = data_key(job->model);
        if (!ed_save_file(key, job->pax)) { mstatus("could NOT write %s", key.c_str()); return; }
        if (!job->ptx.empty()) {
            std::string ptx = key.substr(0, key.size() - 4) + ".ptx";
            if (!ed_save_file(ptx, job->ptx)) { mstatus("could NOT write %s", ptx.c_str()); return; }
        }
        if (!job->merge_glb.empty()) {                  // a level (saved over its own .pao): merge, then reload
            std::string own = lower(job->model);
            MergeRun *r = new MergeRun{new SaveJob(*job), std::string(port_game_dir()) + (char)0x5C + key, own, 0, ""};
            for (auto &c : r->pax_path) if (c == '/') c = (char)0x5C;
            mstatus("merging %s into %s ...", job->merge_glb.c_str(), key.c_str());
            CloseHandle(CreateThread(nullptr, 0, merge_thread, r, 0, nullptr));
            return;
        }
        port_mods_rescan();
        mstatus("saved %s - every prop and level using this model gets the change", key.c_str());
        return;
    }
    std::string own = private_model(job->model, job->name);
    std::string pax = data_key(own), ptx = pax.substr(0, pax.size() - 4) + ".ptx";
    if (!ed_save_file(pax, job->pax) || !ed_save_file(ptx, job->ptx)) { mstatus("could NOT write %s", pax.c_str()); return; }
    if (!job->splits.empty()) {                         // painted faces onto their own texture copies first
        std::string dir = exe_dir() + "\\paint_split";
        CreateDirectoryA(dir.c_str(), nullptr);
        std::string js = "{\"splits\": [";
        for (size_t i = 0; i < job->splits.size(); i++) {
            const PaintSplit &sp = job->splits[i];
            std::string bin = dir + "\\" + sp.new_tex + "_" + std::to_string(i) + ".bin";
            HANDLE f = CreateFileA(bin.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD n = 0;
            if (f != INVALID_HANDLE_VALUE) WriteFile(f, sp.data.data(), (DWORD)sp.data.size(), &n, nullptr), CloseHandle(f);
            std::string jbin;
            for (char c : bin) jbin += c == (char)0x5C ? std::string("/") : std::string(1, c);
            char head[512];
            _snprintf(head, sizeof head, "%s{\"sub\": %d, \"texture\": \"%s\", \"new_texture\": \"%s\", \"data\": \"%s\", "
                      "\"fmt\": %d, \"w\": %d, \"h\": %d, \"mips\": %d, \"tris\": [", i ? ", " : "", sp.sub, sp.tex.c_str(),
                      sp.new_tex.c_str(), jbin.c_str(), sp.fmt, sp.w, sp.h, sp.mips);
            head[sizeof head - 1] = 0;
            js += head;
            for (size_t k = 0; k < sp.tris.size(); k++)
                js += (k ? ", [" : "[") + std::to_string(sp.tris[k][0]) + ", " + std::to_string(sp.tris[k][1]) + ", " +
                      std::to_string(sp.tris[k][2]) + "]";
            js += "]}";
        }
        js += "]}";
        std::string jp = dir + "\\split.json";
        HANDLE f = CreateFileA(jp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        if (f != INVALID_HANDLE_VALUE) WriteFile(f, js.data(), (DWORD)js.size(), &n, nullptr), CloseHandle(f);
        MergeRun *r = new MergeRun{new SaveJob(*job), std::string(port_game_dir()) + (char)0x5C + pax, own, 0, "", jp};
        for (auto &c : r->pax_path) if (c == '/') c = (char)0x5C;
        mstatus("giving the painted faces their own texture copy ...");
        CloseHandle(CreateThread(nullptr, 0, merge_thread, r, 0, nullptr));
        return;
    }
    if (!job->merge_glb.empty()) {                      // merge into the copy first, then repoint / reload
        MergeRun *r = new MergeRun{new SaveJob(*job), std::string(port_game_dir()) + (char)0x5C + pax, own, 0, ""};
        for (auto &c : r->pax_path) if (c == '/') c = (char)0x5C;
        mstatus("merging %s into %s ...", job->merge_glb.c_str(), pax.c_str());
        CloseHandle(CreateThread(nullptr, 0, merge_thread, r, 0, nullptr));
        return;
    }
    finish_after_merge(job, own);
}

static void finish_after_merge(SaveJob *job, const std::string &own)
{
    std::string pax = data_key(own);
    if (own == lower(job->model)) {                     // the prop already had its own copy
        if (job->reload) {
            port_editor_reload();                       // new vertex layout: the model has to load again
            mstatus("saved %s - reloading (the model's layout changed), edit the prop again afterwards", pax.c_str());
        } else {
            port_mods_rescan();
            mstatus("saved %s (this prop's own copy)", pax.c_str());
        }
        return;
    }
    if (!ed_set_prop_model(job->file, job->live, own))
        mstatus("wrote %s, but %s is no longer open - set the prop's MODELS to %s by hand", pax.c_str(),
                job->file.c_str(), own.c_str());
    else
        mstatus("saved %s as %s's own copy; %s now uses it", pax.c_str(), job->name.c_str(), job->file.c_str());
}

// ------------------------------------------------------------------------------------------------ tools
static struct {
    bool press, release, down, valid;
    float nx, ny, px, py;                               // cursor now / at the press
    bool shift, ctrl;
    int wheel;
    bool wshift, wctrl;
    std::vector<std::pair<int, bool>> keys;             // numpad keys (+ Shift) for placing / Grab sub-mesh
} g_in;
static float g_last[2];                                 // cursor of the previous drag step
static float g_depth;                                   // camera depth the drag moves at
static std::vector<std::pair<int, float>> g_grab;       // grabbed groups + weights
static bool g_stroke, g_sel_remove;

static float ov_x(float nx) { return 320.0f + 320.0f * nx; }
static float ov_y(float ny) { return 240.0f - 240.0f * ny; }

static bool project(const CamFrame &cf, const float *M)    // all groups -> S.sx / sy / sz (sz < 0 = hidden)
{
    S.sx.resize(S.ng), S.sy.resize(S.ng), S.sz.resize(S.ng);
    for (int g = 0; g < S.ng; g++) {
        float w[3], v[3];
        to_world(M, gpos(g), w);
        ed_to_cam(cf, w, v);
        if (v[2] < cf.n) { S.sz[g] = -1; continue; }
        S.sx[g] = 320.0f + 320.0f * (v[0] / v[2]) * cf.n / cf.hw;
        S.sy[g] = 240.0f - 240.0f * (v[1] / v[2]) * cf.n / cf.hh;
        S.sz[g] = v[2];
    }
    return true;
}

static bool faces_camera(const CamFrame &cf, const float *M, int g)
{
    float w[3];
    to_world(M, gpos(g), w);
    float toc[3] = {cf.c[0] - w[0], cf.c[1] - w[1], cf.c[2] - w[2]};
    for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
        float n[3], nw[3];
        unpack_n(*nrmp(S.gmem[k]), n);
        dir_to_world(M, n, nw);
        if (nw[0] * toc[0] + nw[1] * toc[1] + nw[2] * toc[2] > 0) return true;
    }
    return false;
}

// occlusion: a depth map of the mesh as the camera sees it (320 x 240 over the 640 x 480 overlay), drawn from the
// projected groups (needs project() first) - a brush then skips vertices something nearer covers (the ground behind a
// rock). Rebuilt when a stroke starts and when the camera moves.
enum { ZW = 320, ZH = 240 };
static std::vector<float> g_zb;
static float g_zcam[9];
static bool g_zdirty = true;

static void build_zbuf(const CamFrame &cf)
{
    float key[9] = {cf.c[0], cf.c[1], cf.c[2], cf.bk[0], cf.bk[1], cf.bk[2], cf.up[0], cf.up[1], cf.up[2]};
    if (!g_zdirty && !g_zb.empty() && !memcmp(key, g_zcam, sizeof key)) return;
    memcpy(g_zcam, key, sizeof key);
    g_zdirty = false;
    g_zb.assign(ZW * ZH, 1e30f);
    for (const Tri &tr : S.tris) {
        if (S.subs[tr.sub].hidden) continue;
        int g0 = S.grp[tr.v[0]], g1 = S.grp[tr.v[1]], g2 = S.grp[tr.v[2]];
        if (S.sz[g0] < 0 || S.sz[g1] < 0 || S.sz[g2] < 0) continue;
        float x0 = S.sx[g0] * 0.5f, y0 = S.sy[g0] * 0.5f, x1 = S.sx[g1] * 0.5f, y1 = S.sy[g1] * 0.5f;
        float x2 = S.sx[g2] * 0.5f, y2 = S.sy[g2] * 0.5f;
        int bx0 = (std::max)(0, (int)floorf((std::min)({x0, x1, x2}))), bx1 = (std::min)(ZW - 1, (int)ceilf((std::max)({x0, x1, x2})));
        int by0 = (std::max)(0, (int)floorf((std::min)({y0, y1, y2}))), by1 = (std::min)(ZH - 1, (int)ceilf((std::max)({y0, y1, y2})));
        if (bx0 > bx1 || by0 > by1) continue;
        float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (fabsf(den) < 1e-6f) continue;
        float z0 = S.sz[g0], z1 = S.sz[g1], z2 = S.sz[g2];
        for (int y = by0; y <= by1; y++)
            for (int x = bx0; x <= bx1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
                float b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
                float c = 1 - a - b;
                if (a < -0.01f || b < -0.01f || c < -0.01f) continue;
                float z = a * z0 + b * z1 + c * z2;
                float &d = g_zb[y * ZW + x];
                if (z < d) d = z;
            }
    }
}

static bool visible(int g)                              // not covered by a nearer surface (build_zbuf first)
{
    if (g_zb.empty()) return true;
    int x = (int)(S.sx[g] * 0.5f), y = (int)(S.sy[g] * 0.5f);
    if (x < 0 || y < 0 || x >= ZW || y >= ZH) return true;
    float best = 1e30f;                                 // the nearest of the pixel and its neighbours (edges)
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < ZW && yy < ZH) best = (std::min)(best, g_zb[yy * ZW + xx]);
        }
    float z = S.sz[g];
    return z <= best + (std::max)(0.05f, z * 0.015f);
}

// groups under the brush at the cursor (needs project() first): weight = smooth falloff
static void brush_groups(const CamFrame &cf, const float *M, std::vector<std::pair<int, float>> &out)
{
    out.clear();
    float mx = ov_x(G.nx), my = ov_y(G.ny), R = s_radius;
    if (s_front) build_zbuf(cf);
    for (int g = 0; g < S.ng; g++) {
        if (S.sz[g] < 0) continue;
        float dx = S.sx[g] - mx, dy = S.sy[g] - my, d2 = dx * dx + dy * dy;
        if (d2 > R * R) continue;
        if (s_front && (!faces_camera(cf, M, g) || !visible(g))) continue;
        float t = 1.0f - d2 / (R * R);
        out.push_back({g, t * t});
    }
}

static bool sel_centroid(float c[3], const float *M)    // world centroid of the selection
{
    double acc[3] = {0, 0, 0}, wsum = 0;
    for (int g = 0; g < S.ng; g++) {
        if (S.gsel[g] <= 0) continue;
        float w[3];
        to_world(M, gpos(g), w);
        for (int k = 0; k < 3; k++) acc[k] += w[k];
        wsum += 1;
    }
    if (wsum == 0) return false;
    for (int k = 0; k < 3; k++) c[k] = (float)(acc[k] / wsum);
    return true;
}

static int sel_count()
{
    int n = 0;
    for (float w : S.gsel) n += w > 0;
    return n;
}

// world displacement for a cursor step (dnx, dny) at camera depth z: view plane, or along one world axis
static bool drag_delta(const CamFrame &cf, const float *M, float dnx, float dny, float z, const float centre[3],
                       float out[3])
{
    int axis = s_axis;
    if (axis == 0) {
        float a = dnx * cf.hw * z / cf.n, b = dny * cf.hh * z / cf.n;
        for (int k = 0; k < 3; k++) out[k] = cf.rt[k] * a + cf.up[k] * b;
        return true;
    }
    float e[3] = {0, 0, 0};
    e[axis - 1] = 1;
    float p0[3], p1[3], v0[3], v1[3];
    memcpy(p0, centre, 12);
    for (int k = 0; k < 3; k++) p1[k] = centre[k] + e[k];
    ed_to_cam(cf, p0, v0);
    ed_to_cam(cf, p1, v1);
    if (v0[2] < cf.n || v1[2] < cf.n) return false;
    float s0x = (v0[0] / v0[2]) * cf.n / cf.hw, s0y = (v0[1] / v0[2]) * cf.n / cf.hh;
    float s1x = (v1[0] / v1[2]) * cf.n / cf.hw, s1y = (v1[1] / v1[2]) * cf.n / cf.hh;
    float sx = s1x - s0x, sy = s1y - s0y, ss = sx * sx + sy * sy;
    if (ss < 1e-8f) return false;
    float amount = (dnx * sx + dny * sy) / ss;
    for (int k = 0; k < 3; k++) out[k] = e[k] * amount;
    return true;
}

static void move_selection_world(const float *M, const float dw[3])
{
    float dl[3];
    if (!dir_to_local(M, dw, dl)) return;
    for (int g = 0; g < S.ng; g++) {
        float w = S.gsel[g];
        if (w <= 0) continue;
        float d[3] = {dl[0] * w, dl[1] * w, dl[2] * w};
        move_group(g, d);
    }
}

// rotate about the world z axis through the selection's centre (degrees) and / or scale (factor)
static void transform_selection(const float *M, float deg, float scale)
{
    float c[3];
    if (!sel_centroid(c, M)) { mstatus("select vertices first"); return; }
    for (int g = 0; g < S.ng; g++) {
        float wgt = S.gsel[g];
        if (wgt <= 0) continue;
        float a = deg * wgt * 0.017453293f, s = 1.0f + (scale - 1.0f) * wgt;
        float w[3];
        to_world(M, gpos(g), w);
        float v[3] = {w[0] - c[0], w[1] - c[1], w[2] - c[2]};
        float r[3] = {v[0] * cosf(a) - v[1] * sinf(a), v[0] * sinf(a) + v[1] * cosf(a), v[2]};
        float nw[3] = {c[0] + r[0] * s, c[1] + r[1] * s, c[2] + r[2] * s}, l[3];
        if (to_local(M, nw, l)) set_group(g, l);
    }
}

// ------------------------------------------------------------------------------------------------ Grab sub-mesh
// Click picks the sub-mesh under the cursor (Shift adds); drag moves the picked sub-meshes - their OWN vertices,
// not the welded seam neighbours of other sub-meshes (the piece comes away clean); wheel turns about z, Shift+wheel
// scales, both about the pieces' centre. Normals turn with the piece (no recompute: hard edges stay as they are).
static std::vector<int> piece_verts()
{
    std::vector<int> v;
    for (int s : S.pieces) v.insert(v.end(), S.subs[s].verts.begin(), S.subs[s].verts.end());
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

static bool piece_centre(const float *M, float c[3])    // world, bounding-box centre of the picked pieces
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (int gi : piece_verts()) {
        float w[3];
        to_world(M, posp(gi), w);
        for (int k = 0; k < 3; k++) lo[k] = (std::min)(lo[k], w[k]), hi[k] = (std::max)(hi[k], w[k]);
        any = true;
    }
    for (int k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) / 2;
    return any;
}

static void select_pieces()                             // the picked pieces show as the selection
{
    std::fill(S.gsel.begin(), S.gsel.end(), 0.0f);
    for (int gi : piece_verts()) S.gsel[S.grp[gi]] = 1.0f;
    S.sel_is_pieces = true;
    if (g_mwnd) PostMessageA(g_mwnd, WM_MESH_PICKED, 0, (LPARAM) new std::vector<int>(S.pieces));
}

// move (world delta), turn (degrees about the world axis `axis`: 0 / 3 = z, 1 = x, 2 = y, through c) and / or scale
static void rot_axis(const float v[3], int axis, float ca, float sa, float out[3])
{
    float e[3] = {0, 0, 0};
    e[axis == 1 ? 0 : axis == 2 ? 1 : 2] = 1;
    float d = e[0] * v[0] + e[1] * v[1] + e[2] * v[2];
    float x[3] = {e[1] * v[2] - e[2] * v[1], e[2] * v[0] - e[0] * v[2], e[0] * v[1] - e[1] * v[0]};
    for (int k = 0; k < 3; k++) out[k] = v[k] * ca + x[k] * sa + e[k] * d * (1 - ca);
}

static void transform_pieces(const float *M, const float dw[3], float deg, float scale, const float c[3], int axis = 0)
{
    float a = deg * 0.017453293f, ca = cosf(a), sa = sinf(a);
    for (int gi : piece_verts()) {
        touch(gi);
        float w[3], l[3];
        to_world(M, posp(gi), w);
        float v[3] = {w[0] - c[0], w[1] - c[1], w[2] - c[2]}, r[3];
        rot_axis(v, axis, ca, sa, r);
        float nw[3] = {c[0] + r[0] * scale + dw[0], c[1] + r[1] * scale + dw[1], c[2] + r[2] * scale + dw[2]};
        if (to_local(M, nw, l)) memcpy(posp(gi), l, 12);
        if (deg != 0) {                                 // the normal turns with the piece
            float n[3], nwo[3], nl[3], rn[3];
            unpack_n(*nrmp(gi), n);
            dir_to_world(M, n, nwo);
            rot_axis(nwo, axis, ca, sa, rn);
            if (dir_to_local(M, rn, nl)) {
                float len = sqrtf(nl[0] * nl[0] + nl[1] * nl[1] + nl[2] * nl[2]);
                if (len > 1e-6f) { for (int k = 0; k < 3; k++) nl[k] /= len; *nrmp(gi) = pack_n(nl); }
            }
        }
    }
}

static void paint_selection(const CamFrame &cf, const float *M)
{
    std::vector<std::pair<int, float>> hit;
    brush_groups(cf, M, hit);
    for (auto &h : hit) S.gsel[h.first] = g_sel_remove ? 0.0f : 1.0f;
}

// vertex colour toward c by t (0..1), per channel incl. alpha
static uint32_t mix_color(uint32_t a, uint32_t c, float t)
{
    uint32_t o = 0;
    for (int k = 0; k < 32; k += 8) {
        float x = (float)((a >> k) & 0xFF), y = (float)((c >> k) & 0xFF);
        o |= (uint32_t)lroundf(x + (y - x) * t) << k;
    }
    return o;
}
static int paint_group(int g, float t)                  // -> members that carry a colour
{
    int n = 0;
    uint32_t c = s_color;
    for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
        int gi = S.gmem[k];
        uint32_t *cp = colp(gi);
        if (!cp) continue;
        uint32_t nc = mix_color(*cp, c, t);
        if (nc != *cp) { touch(gi); *cp = nc; }
        n++;
    }
    return n;
}

static void brush_step(const CamFrame &cf, const float *M)    // smooth / inflate / deflate / paint, every frame held
{
    std::vector<std::pair<int, float>> hit;
    brush_groups(cf, M, hit);
    if (hit.empty()) return;
    float st = s_strength;
    if (s_tool == T_PAINT) {
        for (auto &h : hit) paint_group(h.first, (std::min)(1.0f, st * h.second));
        return;
    }
    if (s_tool == T_SMOOTH) {
        std::vector<float> target(hit.size() * 3);
        for (size_t i = 0; i < hit.size(); i++) {
            int g = hit[i].first;
            float acc[3] = {0, 0, 0};
            int n = S.nstart[g + 1] - S.nstart[g];
            for (int k = S.nstart[g]; k < S.nstart[g + 1]; k++)
                for (int c = 0; c < 3; c++) acc[c] += gpos(S.nbr[k])[c];
            const float *p = gpos(g);
            float t = (std::min)(1.0f, st * hit[i].second);
            for (int c = 0; c < 3; c++) target[i * 3 + c] = n ? p[c] + (acc[c] / n - p[c]) * t : p[c];
        }
        for (size_t i = 0; i < hit.size(); i++) set_group(hit[i].first, &target[i * 3]);
    } else {
        float sign = s_tool == T_INFLATE ? 1.0f : -1.0f, step = S.radius * 0.004f * st;
        for (auto &h : hit) {
            int g = h.first;
            float n[3] = {0, 0, 0};
            for (int k = S.gstart[g]; k < S.gstart[g + 1]; k++) {
                float m[3];
                unpack_n(*nrmp(S.gmem[k]), m);
                n[0] += m[0], n[1] += m[1], n[2] += m[2];
            }
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-6f) continue;
            float d[3];
            for (int c = 0; c < 3; c++) d[c] = n[c] / len * step * h.second * sign;
            move_group(g, d);
        }
    }
}

// ------------------------------------------------------------------------------------------------ texture paint
// The cursor's ray (model space) against the visible triangles; the nearest hit's UV (vertex UVs interpolated:
// stride 24 at +16, 28 / 36 at +20 - D3D convention, v down) picks the texel in the sub-mesh's texture. The brush
// radius goes screen -> world at the hit depth -> texels through that triangle's UV / world scale. The decoded top
// level is painted, only its touched 4x4 blocks are encoded each frame; the stroke's end rebuilds the mip levels.
static const float *uvp(int gi) { return (const float *)(S.P[gi] + (stridep(gi) == 24 ? 16 : 20)); }

static bool ray_tri(const float o[3], const float d[3], const float *a, const float *b, const float *c, float &t,
                    float &u, float &v)
{
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabsf(det) < 1e-12f) return false;
    float inv = 1.0f / det, s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0 || u > 1) return false;
    float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return false;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0;
}

static void encode_dirty(int ti)                        // touched blocks of the top level -> texture memory
{
    Tex &t = S.texs[ti];
    int bw = (t.w + 3) / 4, bh = (t.h + 3) / 4, bs = t.fmt == 0x0F ? 16 : 8;
    uint8_t *dst = *(uint8_t **)(t.hdr + 4);
    bool any = false;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            if (!t.dirty[by * bw + bx]) continue;
            t.dirty[by * bw + bx] = 0;
            any = true;
            uint8_t px[16][4], blk[16];
            for (int i = 0; i < 16; i++) {
                int x = (std::min)(bx * 4 + (i & 3), t.w - 1), y = (std::min)(by * 4 + (i >> 2), t.h - 1);
                memcpy(px[i], &t.rgba[((size_t)y * t.w + x) * 4], 4);
            }
            if (bs == 16) dxt_alpha_block(px, blk), dxt_color_block(px, blk + 8);
            else dxt_color_block(px, blk);
            memcpy(dst + ((size_t)by * bw + bx) * bs, blk, bs);
        }
    if (any) d3d_texture_dirty((struct XPixelContainer *)t.hdr);
}

static void mark_painted(const CamFrame &cf, const float *M, int ti, int hit);

// the nearest visible triangle under the cursor (model-space ray from the scene camera): its S.tris index or -1
static int ray_pick(const CamFrame &cf, const float *M, float o[3], float d[3], float &best, float &bu, float &bv)
{
    float dw[3];
    for (int k = 0; k < 3; k++) dw[k] = -cf.bk[k] * cf.n + cf.rt[k] * G.nx * cf.hw + cf.up[k] * G.ny * cf.hh;
    if (!to_local(M, cf.c, o) || !dir_to_local(M, dw, d)) return -1;
    best = 1e30f, bu = bv = 0;
    int hit = -1;
    for (int i = 0; i < (int)S.tris.size(); i++) {
        const Tri &tr = S.tris[i];
        if (S.subs[tr.sub].hidden) continue;
        float t, u, v;
        if (ray_tri(o, d, posp(tr.v[0]), posp(tr.v[1]), posp(tr.v[2]), t, u, v) && t < best) best = t, bu = u, bv = v, hit = i;
    }
    return hit;
}

static void texpaint_step(const CamFrame &cf, const float *M)
{
    float dw[3], o[3], d[3];
    for (int k = 0; k < 3; k++) dw[k] = -cf.bk[k] * cf.n + cf.rt[k] * G.nx * cf.hw + cf.up[k] * G.ny * cf.hh;
    if (!to_local(M, cf.c, o) || !dir_to_local(M, dw, d)) return;
    float best = 1e30f, bu = 0, bv = 0;
    int hit = -1;
    for (int i = 0; i < (int)S.tris.size(); i++) {
        const Tri &tr = S.tris[i];
        if (S.subs[tr.sub].hidden) continue;
        float t, u, v;
        if (ray_tri(o, d, posp(tr.v[0]), posp(tr.v[1]), posp(tr.v[2]), t, u, v) && t < best) best = t, bu = u, bv = v, hit = i;
    }
    if (hit < 0) return;
    const Tri &tr = S.tris[hit];
    int ti = -1;
    for (int k : S.subs[tr.sub].tex) if (S.texs[k].bytes) { ti = k; break; }
    static DWORD warned;
    if (ti < 0) {
        if (GetTickCount() - warned > 2000) warned = GetTickCount(), mstatus("this sub-mesh has no editable (DXT1 / DXT5) texture");
        return;
    }
    Tex &t = S.texs[ti];
    if (t.rgba.empty()) {
        dxt_decode(*(uint8_t **)(t.hdr + 4), t.w, t.h, t.fmt, t.rgba);
        t.dirty.assign((size_t)((t.w + 3) / 4) * ((t.h + 3) / 4), 0);
    }
    touch_tex(ti);                                      // the stroke's undo keeps the whole texture once
    const float *ua = uvp(tr.v[0]), *ub = uvp(tr.v[1]), *uc = uvp(tr.v[2]);
    float w0 = 1 - bu - bv, uu = ua[0] * w0 + ub[0] * bu + uc[0] * bv, vv = ua[1] * w0 + ub[1] * bu + uc[1] * bv;
    // texels per world unit on this triangle: sqrt(UV area / world area) x texture size
    const float *a = posp(tr.v[0]), *b = posp(tr.v[1]), *c = posp(tr.v[2]);
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    float wa = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
    float ta = fabsf((ub[0] - ua[0]) * (uc[1] - ua[1]) - (uc[0] - ua[0]) * (ub[1] - ua[1]));
    if (wa < 1e-12f || ta < 1e-12f) return;
    float uv_per_unit = sqrtf(ta / wa);                 // UV per model unit on this triangle
    // brush radius: overlay pixels -> world units at the hit's view depth -> model units (matrix scale)
    float hl[3] = {o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best}, hw[3], v[3];
    to_world(M, hl, hw);
    ed_to_cam(cf, hw, v);
    float scale = sqrtf(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]);
    float world_r = s_radius / 320.0f * cf.hw * (std::max)(v[2], cf.n) / cf.n;
    float r = (std::max)(0.75f, world_r / (scale > 1e-6f ? scale : 1.0f) * uv_per_unit * (float)(std::max)(t.w, t.h));
    r = (std::min)(r, (float)(std::max)(t.w, t.h) / 2);
    float px = (uu - floorf(uu)) * t.w, py = (vv - floorf(vv)) * t.h;
    uint32_t col = s_color;
    float cR = (float)((col >> 16) & 0xFF), cG = (float)((col >> 8) & 0xFF), cB = (float)(col & 0xFF);
    int bw = (t.w + 3) / 4, ir = (int)ceilf(r);
    for (int y = (int)floorf(py) - ir; y <= (int)floorf(py) + ir; y++)
        for (int x = (int)floorf(px) - ir; x <= (int)floorf(px) + ir; x++) {
            float dx = x + 0.5f - px, dy = y + 0.5f - py, d2 = (dx * dx + dy * dy) / (r * r);
            if (d2 > 1) continue;
            float k = (1 - d2) * (1 - d2) * s_strength;
            int wx = ((x % t.w) + t.w) % t.w, wy = ((y % t.h) + t.h) % t.h;   // UVs wrap
            uint8_t *q = &t.rgba[((size_t)wy * t.w + wx) * 4];
            q[0] = (uint8_t)lroundf(q[0] + (cR - q[0]) * k);
            q[1] = (uint8_t)lroundf(q[1] + (cG - q[1]) * k);
            q[2] = (uint8_t)lroundf(q[2] + (cB - q[2]) * k);
            t.dirty[(wy / 4) * bw + wx / 4] = 1;
        }
    encode_dirty(ti);
    mark_painted(cf, M, ti, hit);
}

static std::array<WORD, 3> canon_tri(WORD a, WORD b, WORD c)   // the same face whatever corner it starts at
{
    if (b < a && b < c) return {b, c, a};
    if (c < a && c < b) return {c, a, b};
    return {a, b, c};
}

// faces of the painted texture under the brush circle (any corner inside it, facing the camera) + the hit face:
// Save compares them with the faces that read the changed texels (see paint_splits)
static void mark_painted(const CamFrame &cf, const float *M, int ti, int hit)
{
    float mx = ov_x(G.nx), my = ov_y(G.ny), R = s_radius;
    for (int i = 0; i < (int)S.tris.size(); i++) {
        const Tri &tr = S.tris[i];
        Sub &b = S.subs[tr.sub];
        if (b.hidden || std::find(b.tex.begin(), b.tex.end(), ti) == b.tex.end()) continue;
        bool in = i == hit;
        if (!in) {
            float fn[3], fw[3], w[3];
            face_normal(tr, fn);
            dir_to_world(M, fn, fw);
            to_world(M, posp(tr.v[0]), w);
            if ((cf.c[0] - w[0]) * fw[0] + (cf.c[1] - w[1]) * fw[1] + (cf.c[2] - w[2]) * fw[2] <= 0) continue;
            for (int k = 0; k < 3 && !in; k++) {
                float ww[3], v[3];
                to_world(M, posp(tr.v[k]), ww);
                ed_to_cam(cf, ww, v);
                if (v[2] < cf.n) continue;
                float sx = 320.0f + 320.0f * (v[0] / v[2]) * cf.n / cf.hw - mx;
                float sy = 240.0f - 240.0f * (v[1] / v[2]) * cf.n / cf.hh - my;
                in = sx * sx + sy * sy <= R * R;
            }
        }
        if (!in) continue;
        int base = S.vbs[b.vb].base;
        b.painted.insert(canon_tri((WORD)(tr.v[0] - base), (WORD)(tr.v[1] - base), (WORD)(tr.v[2] - base)));
    }
}

static void texpaint_finish()                           // stroke end: every mip level from the painted top level
{
    for (auto &x : S.cur.tx) {
        Tex &t = S.texs[x.tex];
        if (t.rgba.empty()) continue;
        std::vector<uint8_t> all;
        encode_levels(t.rgba, t.w, t.h, t.levels, t.fmt == 0x0F, all);
        if (all.size() == t.bytes) {
            memcpy(*(uint8_t **)(t.hdr + 4), all.data(), t.bytes);
            d3d_texture_dirty((struct XPixelContainer *)t.hdr);
        }
    }
}

// Merge GLB, click to place: the press's ray hits the model -> that point (model space); a miss uses where the ray
// meets the horizontal plane through the model's origin. Then the merge runs there (save, paxmerge, reload).
static MergeReq g_place;
static bool g_place_armed;
static std::vector<float> g_ghost;                      // the part's wireframe (edges, 6 floats), floor at z = 0
static float g_ghost_lo[3], g_ghost_hi[3];             // its box (the preview's last 12 edges), part space

// model space <-> the part's own space for the current placement (inverse of paxmerge --lean / --turn / --scale)
struct GhostXf {
    float at[3], ca, sa, cbx, sbx, cby, sby, k;
    GhostXf(const float a[3])
    {
        memcpy(at, a, 12);
        float t = g_place.turn * 0.017453293f, bx = g_place.lean_x * 0.017453293f, by = g_place.lean_y * 0.017453293f;
        ca = cosf(t), sa = sinf(t), cbx = cosf(bx), sbx = sinf(bx), cby = cosf(by), sby = sinf(by), k = g_place.scale;
    }
    void fwd(const float p[3], float l[3]) const        // part space -> model space (paxmerge --lean / --turn / --at)
    {
        float x = p[0] * k, y = p[1] * k, z = p[2] * k, t;
        t = y * cbx - z * sbx, z = y * sbx + z * cbx, y = t;
        t = x * cby + z * sby, z = -x * sby + z * cby, x = t;
        l[0] = x * ca - y * sa + at[0], l[1] = x * sa + y * ca + at[1], l[2] = z + at[2];
    }
    void axis(int i, float o[3]) const                  // the box's axis i in model space (unit)
    {
        float e[3] = {0, 0, 0}, z0[3] = {0, 0, 0}, a0[3], a1[3];
        e[i] = 1.0f / k;                                // fwd scales by k: hand it 1/k
        GhostXf g = *this;
        memset(g.at, 0, 12);
        g.fwd(z0, a0), g.fwd(e, a1);
        for (int q = 0; q < 3; q++) o[q] = a1[q] - a0[q];
    }
    bool touches(const float *a, const float *b, const float *c) const   // a face inside or crossing the box
    {
        float m[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
        return inside(a) || inside(b) || inside(c) || inside(m);
    }
    bool inside(const float q[3]) const                 // in the ghost's box?
    {
        float x = q[0] - at[0], y = q[1] - at[1], z = q[2] - at[2], t;
        t = x * ca + y * sa, y = -x * sa + y * ca, x = t;           // turn back
        t = x * cby - z * sby, z = x * sby + z * cby, x = t;        // lean y back
        t = y * cbx + z * sbx, z = -y * sbx + z * cbx, y = t;       // lean x back
        float p[3] = {x / k, y / k, z / k};
        for (int i = 0; i < 3; i++)
            if (p[i] < g_ghost_lo[i] - 1e-3f || p[i] > g_ghost_hi[i] + 1e-3f) return false;
        return true;
    }
};

static bool tri_in_ghost(const GhostXf &gx, const float *a, const float *b, const float *c)   // its centre inside
{
    float m[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
    return gx.inside(m);
}

// where a placement would land now: the cursor's ray on the model, else on the origin's horizontal plane
static bool place_point(const CamFrame &cf, const float *M, float out[3])
{
    float o[3], d[3], best, bu, bv;
    if (ray_pick(cf, M, o, d, best, bu, bv) >= 0) {
        for (int k = 0; k < 3; k++) out[k] = o[k] + d[k] * best;
        return true;
    }
    if (fabsf(d[2]) > 1e-6f && -o[2] / d[2] > 0) {
        float t = -o[2] / d[2];
        for (int k = 0; k < 3; k++) out[k] = o[k] + d[k] * t;
        return true;
    }
    return false;
}

// Collision view while placing: existing sub-meshes whose sphere meets the ghost's box are drawn orange; a height map
// of their triangles under the ghost's footprint (GH x GH cells, highest surface per cell, model space) marks the
// ghost edges whose ends lie below that surface red - where the part sinks into the ground / cuts into a wall. The
// map is rebuilt only when the placement changes.
enum { GH = 64 };
static struct {
    float key[7];                                       // at xyz, turn, lean x / y, scale of the last build
    float lo[2], hi[2];                                 // footprint (model x / y)
    std::vector<float> h;                               // GH * GH highest surface, -1e30 = nothing
    std::vector<int> subs;                              // overlapping sub-meshes
    bool valid;
} g_gh;

static void ghost_heights(const std::vector<float> &pts, const float key[7])
{
    if (g_gh.valid && !memcmp(g_gh.key, key, sizeof g_gh.key)) return;
    memcpy(g_gh.key, key, sizeof g_gh.key);
    g_gh.valid = true;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < pts.size(); i += 3)
        for (int k = 0; k < 3; k++) lo[k] = (std::min)(lo[k], pts[i + k]), hi[k] = (std::max)(hi[k], pts[i + k]);
    float pad = 0.05f * (std::max)(hi[0] - lo[0], hi[1] - lo[1]) + 0.1f;
    for (int k = 0; k < 2; k++) g_gh.lo[k] = lo[k] - pad, g_gh.hi[k] = hi[k] + pad;
    g_gh.subs.clear();
    for (int si = 0; si < (int)S.subs.size(); si++) {   // sphere vs the ghost's box
        const Sub &b = S.subs[si];
        if (b.hidden) continue;
        float r = *(const float *)(b.recp + 0x0C), d2 = 0;
        const float *c = (const float *)(b.recp + 0x10);
        for (int k = 0; k < 3; k++) {
            float v = c[k] < lo[k] ? lo[k] - c[k] : c[k] > hi[k] ? c[k] - hi[k] : 0;
            d2 += v * v;
        }
        if (d2 <= r * r) g_gh.subs.push_back(si);
    }
    g_gh.h.assign(GH * GH, -1e30f);
    std::vector<char> want(S.subs.size(), 0);
    for (int si : g_gh.subs) want[si] = 1;
    float cw = (g_gh.hi[0] - g_gh.lo[0]) / GH, ch = (g_gh.hi[1] - g_gh.lo[1]) / GH;
    for (const Tri &t : S.tris) {
        if (!want[t.sub]) continue;
        const float *A = posp(t.v[0]), *B = posp(t.v[1]), *C = posp(t.v[2]);
        float d = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1]);
        if (fabsf(d) < 1e-12f) continue;                // vertical faces: their tops are other faces
        int x0 = (int)floorf(((std::min)({A[0], B[0], C[0]}) - g_gh.lo[0]) / cw);
        int x1 = (int)floorf(((std::max)({A[0], B[0], C[0]}) - g_gh.lo[0]) / cw);
        int y0 = (int)floorf(((std::min)({A[1], B[1], C[1]}) - g_gh.lo[1]) / ch);
        int y1 = (int)floorf(((std::max)({A[1], B[1], C[1]}) - g_gh.lo[1]) / ch);
        if (x1 < 0 || y1 < 0 || x0 >= GH || y0 >= GH) continue;
        for (int y = (std::max)(y0, 0); y <= (std::min)(y1, GH - 1); y++)
            for (int x = (std::max)(x0, 0); x <= (std::min)(x1, GH - 1); x++) {
                float px = g_gh.lo[0] + (x + 0.5f) * cw, py = g_gh.lo[1] + (y + 0.5f) * ch;
                float l0 = ((B[1] - C[1]) * (px - C[0]) + (C[0] - B[0]) * (py - C[1])) / d;
                float l1 = ((C[1] - A[1]) * (px - C[0]) + (A[0] - C[0]) * (py - C[1])) / d;
                if (l0 < -0.02f || l1 < -0.02f || l0 + l1 > 1.02f) continue;
                float z = A[2] * l0 + B[2] * l1 + C[2] * (1 - l0 - l1);
                float &cell = g_gh.h[y * GH + x];
                if (z > cell) cell = z;
            }
    }
}

static bool ghost_buried(const float *p)                 // model-space point below the existing surface?
{
    float cw = (g_gh.hi[0] - g_gh.lo[0]) / GH, ch = (g_gh.hi[1] - g_gh.lo[1]) / GH;
    int x = (int)floorf((p[0] - g_gh.lo[0]) / cw), y = (int)floorf((p[1] - g_gh.lo[1]) / ch);
    if (x < 0 || y < 0 || x >= GH || y >= GH) return false;
    float h = g_gh.h[y * GH + x];
    return h > -1e29f && p[2] < h - 0.05f;
}

// The part's footprint for the preview (as paxmerge footprint(): ~96 cells across the box's longer side, in the
// box's frame; a cell the ghost's edges pass over keeps their height range - an approximation of paxmerge's "fully
// covered" cells) - magenta = faces in a covered cell at or above the part's lowest surface there (what gets cut).
static struct {
    float key[8];
    float c[3], ax[3][3], h[3], cs, pad;
    int nx, ny;
    std::vector<float> zlo, zhi;                        // nx * ny, zlo > zhi = not covered
} g_fp;

static void ghost_footprint(const GhostXf &gx, const std::vector<float> &pts, const float key[8])
{
    if (!memcmp(g_fp.key, key, sizeof g_fp.key) && !g_fp.zlo.empty()) return;
    memcpy(g_fp.key, key, sizeof g_fp.key);
    float mid[3];
    for (int k = 0; k < 3; k++) mid[k] = (g_ghost_lo[k] + g_ghost_hi[k]) / 2;
    gx.fwd(mid, g_fp.c);
    for (int i = 0; i < 3; i++) gx.axis(i, g_fp.ax[i]), g_fp.h[i] = (g_ghost_hi[i] - g_ghost_lo[i]) / 2 * gx.k;
    g_fp.cs = (std::max)(2 * g_fp.h[0], 2 * g_fp.h[1]) / 96;
    if (!(g_fp.cs > 0)) g_fp.cs = 1;
    g_fp.nx = (std::max)(1, (int)ceilf(2 * g_fp.h[0] / g_fp.cs)), g_fp.ny = (std::max)(1, (int)ceilf(2 * g_fp.h[1] / g_fp.cs));
    g_fp.pad = (std::max)(0.3f, 0.05f * 2 * g_fp.h[2]);
    g_fp.zlo.assign(g_fp.nx * g_fp.ny, 1e30f);
    g_fp.zhi.assign(g_fp.nx * g_fp.ny, -1e30f);
    auto loc = [&](const float *p, float l[3]) {
        float d[3] = {p[0] - g_fp.c[0], p[1] - g_fp.c[1], p[2] - g_fp.c[2]};
        for (int k = 0; k < 3; k++) l[k] = d[0] * g_fp.ax[k][0] + d[1] * g_fp.ax[k][1] + d[2] * g_fp.ax[k][2];
    };
    auto mark = [&](int ix, int iy, float z0, float z1) {
        if (ix < 0 || iy < 0 || ix >= g_fp.nx || iy >= g_fp.ny) return;
        float &a = g_fp.zlo[iy * g_fp.nx + ix], &b = g_fp.zhi[iy * g_fp.nx + ix];
        a = (std::min)(a, z0), b = (std::max)(b, z1);
    };
    size_t n = pts.size() / 6;
    for (size_t i = 0; i + 12 < n; i++) {               // the edges (not the box), walked in steps of half a cell
        float A[3], B[3];
        loc(&pts[i * 6], A), loc(&pts[i * 6 + 3], B);
        float len = sqrtf((B[0] - A[0]) * (B[0] - A[0]) + (B[1] - A[1]) * (B[1] - A[1]));
        int steps = (std::min)(200, (int)(len / (g_fp.cs * 0.5f)) + 1);
        for (int k = 0; k <= steps; k++) {
            float t = (float)k / steps, x = A[0] + (B[0] - A[0]) * t, y = A[1] + (B[1] - A[1]) * t;
            mark((int)floorf((x + g_fp.h[0]) / g_fp.cs), (int)floorf((y + g_fp.h[1]) / g_fp.cs), (std::min)(A[2], B[2]),
                 (std::max)(A[2], B[2]));
        }
    }
}

static bool fp_covers(const float *a, const float *b, const float *c)   // a face the merge would cut into
{
    float m[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3}, l[3];
    const float *pl[4] = {m, a, b, c};
    for (const float *p : pl) {
        float d[3] = {p[0] - g_fp.c[0], p[1] - g_fp.c[1], p[2] - g_fp.c[2]};
        for (int k = 0; k < 3; k++) l[k] = d[0] * g_fp.ax[k][0] + d[1] * g_fp.ax[k][1] + d[2] * g_fp.ax[k][2];
        int ix = (int)floorf((l[0] + g_fp.h[0]) / g_fp.cs), iy = (int)floorf((l[1] + g_fp.h[1]) / g_fp.cs);
        if (ix < 0 || iy < 0 || ix >= g_fp.nx || iy >= g_fp.ny) continue;
        float z0 = g_fp.zlo[iy * g_fp.nx + ix], z1 = g_fp.zhi[iy * g_fp.nx + ix];
        if (z0 <= z1 && l[2] >= z0 - g_fp.pad) return true;   // at or above the part's lowest surface there
    }
    return false;
}

static void draw_ghost(const CamFrame &cf, const float *M)
{
    float at[3];
    if (!g_place_armed || !G.valid || !place_point(cf, M, at)) return;
    at[2] += g_place.lift;                              // numpad 1 / 3: above / below the point under the cursor
    float a = g_place.turn * 0.017453293f, ca = cosf(a), sa = sinf(a), k = g_place.scale;
    float bx = g_place.lean_x * 0.017453293f, by = g_place.lean_y * 0.017453293f;
    float cbx = cosf(bx), sbx = sinf(bx), cby = cosf(by), sby = sinf(by);
    auto local = [&](const float *p, float l[3]) {      // as paxmerge --lean / --turn / --scale / --at (model space)
        float x = p[0] * k, y = p[1] * k, z = p[2] * k, t;
        t = y * cbx - z * sbx, z = y * sbx + z * cbx, y = t;
        t = x * cby + z * sby, z = -x * sby + z * cby, x = t;
        l[0] = x * ca - y * sa + at[0], l[1] = x * sa + y * ca + at[1], l[2] = z + at[2];
    };
    auto cam = [&](const float l[3], float w[3]) {
        float v[3];
        to_world(M, l, v);
        ed_to_cam(cf, v, w);
    };
    size_t n = g_ghost.size() / 6;
    std::vector<float> pts(n * 6);
    for (size_t i = 0; i < n * 2; i++) local(&g_ghost[i * 3], &pts[i * 3]);
    float key[7] = {at[0], at[1], at[2], g_place.turn, g_place.lean_x, g_place.lean_y, g_place.scale};
    ghost_heights(std::vector<float>(pts.begin(), pts.end() - 12 * 6 > pts.begin() ? pts.end() - 12 * 6 : pts.end()), key);
    // the existing pieces it meets: orange (edges of their triangles, capped)
    int drawn = 0;
    std::vector<char> want(S.subs.size(), 0);
    for (int si : g_gh.subs) want[si] = 1;
    GhostXf gx(at);
    int doomed = 0;
    if (g_place.clear && g_ghost_hi[0] > g_ghost_lo[0]) {
        float fkey[8] = {at[0], at[1], at[2], g_place.turn, g_place.lean_x, g_place.lean_y, g_place.scale, (float)n};
        ghost_footprint(gx, pts, fkey);
    }
    for (size_t ti = 0; ti < S.tris.size() && drawn < 6000; ti++) {
        const Tri &t = S.tris[ti];
        if (!want[t.sub]) continue;
        float A[3], B[3], C[3];
        bool gone = g_place.clear && g_ghost_hi[0] > g_ghost_lo[0] && gx.touches(posp(t.v[0]), posp(t.v[1]), posp(t.v[2])) &&
                    fp_covers(posp(t.v[0]), posp(t.v[1]), posp(t.v[2]));
        doomed += gone;
        cam(posp(t.v[0]), A), cam(posp(t.v[1]), B), cam(posp(t.v[2]), C);
        uint32_t col = gone ? 0xE0FF40FF : 0x70FF9030;   // magenta = cleared on placing
        ed_cam_line(cf, A, B, gone ? 0.8f : 0.5f, col), ed_cam_line(cf, B, C, gone ? 0.8f : 0.5f, col);
        if (gone) ed_cam_line(cf, C, A, 0.8f, col);
        drawn++;
    }
    // the ghost: blue, red where it is below the existing surface; its box yellow
    int buried = 0, total = 0;
    for (size_t i = 0; i < n; i++) {
        float A[3], B[3];
        cam(&pts[i * 6], A);
        cam(&pts[i * 6 + 3], B);
        bool box = i + 12 >= n;
        bool under = !box && (ghost_buried(&pts[i * 6]) || ghost_buried(&pts[i * 6 + 3]));
        if (!box) total++, buried += under;
        ed_cam_line(cf, A, B, under ? 0.8f : 0.5f, box ? 0xE0FFD040 : under ? 0xE0FF3030 : 0x9040E0FF);
    }
    char b[200];
    _snprintf(b, sizeof b, "placing: turn %.0f  tilt %.0f / %.0f  scale %.2f  height %+.2f  at %.2f %.2f %.2f   "
              "(numpad 4/6 8/2 7/9 1/3 +/- 5)", g_place.turn, g_place.lean_x, g_place.lean_y, g_place.scale, g_place.lift,
              at[0], at[1], at[2]);
    b[sizeof b - 1] = 0;
    ov_text(b, 12, 32, OV_LEFT, 0xFFFFD040, true);
    if (g_place.clear)
        _snprintf(b, sizeof b, "meets %d existing piece%s   -   %d of their faces are cut at the box on placing (magenta)",
                  (int)g_gh.subs.size(), g_gh.subs.size() == 1 ? "" : "s", doomed);
    else
        _snprintf(b, sizeof b, "meets %d existing piece%s   -   %d%% of the part below their surface (red)", (int)g_gh.subs.size(),
                  g_gh.subs.size() == 1 ? "" : "s", total ? buried * 100 / total : 0);
    b[sizeof b - 1] = 0;
    ov_text(b, 12, 48, OV_LEFT, buried ? 0xFFFF8060 : 0xFFA0E0A0, true);
}
static void save(bool shared, bool add_colors, const MergeReq *merge);

static bool place_merge(const CamFrame &cf, const float *M)
{
    if (!g_place_armed) return false;
    g_place_armed = false;
    float nx = G.nx, ny = G.ny;
    G.nx = G.px, G.ny = G.py;
    bool ok = place_point(cf, M, g_place.at);
    G.nx = nx, G.ny = ny;
    g_place.at[2] += g_place.lift;
    g_ghost.clear();
    if (!ok) {
        mstatus("that click hit nothing - press Merge GLB... and click on the ground again");
        return true;
    }
    g_place.has_at = true;
    g_place.clear = g_place.clear && g_ghost_hi[0] > g_ghost_lo[0];
    if (g_place.clear) {                                // the part's box in model space: paxmerge cuts the faces at it
        GhostXf gx(g_place.at);
        float mid[3], c[3];
        for (int k = 0; k < 3; k++) mid[k] = (g_ghost_lo[k] + g_ghost_hi[k]) / 2;
        gx.fwd(mid, c);
        memcpy(g_place.box, c, 12);
        for (int i = 0; i < 3; i++) {
            gx.axis(i, g_place.box + 3 + i * 3);
            g_place.box[12 + i] = (g_ghost_hi[i] - g_ghost_lo[i]) / 2 * gx.k;
        }
    }
    mstatus("placing the part at %.2f %.2f %.2f ...", g_place.at[0], g_place.at[1], g_place.at[2]);
    save(false, false, &g_place);
    return true;
}

static void do_press(const CamFrame &cf, const float *M)
{
    if (place_merge(cf, M)) return;
    g_last[0] = G.px, g_last[1] = G.py;
    float nx = G.nx, ny = G.ny;
    G.nx = G.px, G.ny = G.py;               // brushes start where the button went down
    project(cf, M);
    g_zdirty = true;                        // occlusion taken afresh for each stroke
    switch (s_tool) {
    case T_SELECT:
        if (!G.shift && !G.ctrl) std::fill(S.gsel.begin(), S.gsel.end(), 0.0f);
        g_sel_remove = G.ctrl;
        S.sel_is_pieces = false;
        paint_selection(cf, M);
        g_stroke = true;
        break;
    case T_MOVE: {
        float c[3], v[3];
        if (!sel_centroid(c, M)) { mstatus("select vertices first (Select tool, or Select sub-mesh)"); break; }
        ed_to_cam(cf, c, v);
        g_depth = (std::max)(v[2], cf.n);
        begin_op("move");
        g_stroke = true;
        break;
    }
    case T_PIECE: {
        float o[3], d[3], best, bu, bv;
        G.nx = G.px, G.ny = G.py;
        int hit = ray_pick(cf, M, o, d, best, bu, bv);
        if (hit < 0) {
            if (!G.shift) { S.pieces.clear(); select_pieces(); }
            break;
        }
        int sub = S.tris[hit].sub;
        // the sub-meshes the current selection touches (Select brush, All, Select sub-mesh): pressing on one of
        // them grabs them all - a group of buildings moves together; pressing elsewhere picks just that piece
        // (a selection made by picking = those pieces; a brush selection counts a sub-mesh with at least a quarter of
        // its vertices selected, so the few seam vertices it shares with the ground do not drag the terrain along)
        std::vector<int> selected;
        if (S.sel_is_pieces) selected = S.pieces;
        else
            for (int si = 0; si < (int)S.subs.size(); si++) {
                int n = 0;
                for (int gi : S.subs[si].verts) n += S.gsel[S.grp[gi]] > 0;
                if (n && n * 4 >= (int)S.subs[si].verts.size()) selected.push_back(si);
            }
        bool in_sel = std::find(selected.begin(), selected.end(), sub) != selected.end();
        if (!G.shift && !G.ctrl && in_sel && selected.size() > 1) {
            S.pieces = selected;
        } else {
            if (!G.shift) S.pieces.clear();
            if (std::find(S.pieces.begin(), S.pieces.end(), sub) == S.pieces.end()) S.pieces.push_back(sub);
            else if (G.ctrl) S.pieces.erase(std::find(S.pieces.begin(), S.pieces.end(), sub));
        }
        select_pieces();
        float c[3], v[3];
        if (!piece_centre(M, c)) break;
        ed_to_cam(cf, c, v);
        g_depth = (std::max)(v[2], cf.n);
        begin_op("move sub-mesh");
        g_stroke = true;
        mstatus("picked %s (%d sub-mesh%s) - drag to move, wheel turns, Shift+wheel scales", S.subs[sub].name.c_str(),
                (int)S.pieces.size(), S.pieces.size() == 1 ? "" : "es");
        break;
    }
    case T_GRAB: {
        brush_groups(cf, M, g_grab);
        if (g_grab.empty()) break;
        double z = 0, ws = 0;
        for (auto &h : g_grab) z += S.sz[h.first] * h.second, ws += h.second;
        g_depth = (std::max)((float)(z / ws), cf.n);
        begin_op("grab");
        g_stroke = true;
        break;
    }
    default:
        begin_op(k_tool_names[s_tool]);
        g_stroke = true;
        break;
    }
    G.nx = nx, G.ny = ny;
}

static void do_drag(const CamFrame &cf, const float *M)
{
    float dnx = G.nx - g_last[0], dny = G.ny - g_last[1];
    if (s_tool == T_SELECT) {
        project(cf, M);
        paint_selection(cf, M);
        return;
    }
    if (dnx == 0 && dny == 0) return;
    if (s_tool == T_MOVE) {
        float c[3], dw[3];
        if (!sel_centroid(c, M) || !drag_delta(cf, M, dnx, dny, g_depth, c, dw)) return;
        move_selection_world(M, dw);
    } else if (s_tool == T_PIECE) {
        float c[3], dw[3];
        if (S.pieces.empty() || !piece_centre(M, c) || !drag_delta(cf, M, dnx, dny, g_depth, c, dw)) return;
        transform_pieces(M, dw, 0, 1, c);
    } else if (s_tool == T_GRAB) {
        if (g_grab.empty()) return;
        float c[3] = {0, 0, 0}, dw[3], dl[3];
        for (auto &h : g_grab) {
            float w[3];
            to_world(M, gpos(h.first), w);
            for (int k = 0; k < 3; k++) c[k] += w[k] / g_grab.size();
        }
        if (!drag_delta(cf, M, dnx, dny, g_depth, c, dw) || !dir_to_local(M, dw, dl)) return;
        for (auto &h : g_grab) {
            float d[3] = {dl[0] * h.second, dl[1] * h.second, dl[2] * h.second};
            move_group(h.first, d);
        }
    }
    g_last[0] = G.nx, g_last[1] = G.ny;
}

static void do_release()
{
    if (!g_stroke) return;
    g_stroke = false;
    g_grab.clear();
    if (s_tool == T_SELECT) {
        notify_info();
        mstatus("%d vertices selected", sel_count());
        return;
    }
    if (s_tool == T_TEXPAINT) texpaint_finish();
    renormal_moved();
    commit_op();
}

// ------------------------------------------------------------------------------------------------ commands
static void run_cmd(const Cmd &c, const float *M)
{
    if (c.type == C_START) { start(c); return; }
    if (c.type == C_LEVEL) { start_level(); return; }
    if (c.type == C_LEVEL_RESET) { reset_level(); return; }
    if (!g_on) return;
    switch (c.type) {
    case C_STOP:
        g_place_armed = false;
        g_ghost.clear();
        end_session(S.dirty ? "stopped - unsaved edits stay in the game until the level reloads" : "stopped");
        break;
    case C_SEL_SUB:
        S.sel_is_pieces = false;
        if (!c.add) std::fill(S.gsel.begin(), S.gsel.end(), 0.0f);
        for (int s : c.list)
            if (s >= 0 && s < (int)S.subs.size())
                for (int gi : S.subs[s].verts) S.gsel[S.grp[gi]] = 1.0f;
        notify_info();
        mstatus("%d vertices selected", sel_count());
        break;
    case C_HIDE_SUB:
        begin_op("hide / show");
        for (int s : c.list) {
            if (s < 0 || s >= (int)S.subs.size()) continue;
            Sub &b = S.subs[s];
            touch_sub(s);
            if (b.hidden) {
                const std::vector<WORD> &back = b.shown.empty() ? b.orig : b.shown;
                set_indices(s, back);
                b.hidden = std::all_of(back.begin(), back.end(), [&](WORD w) { return w == back[0]; });
            } else {
                b.shown.assign(b.idx, b.idx + b.nidx);
                std::fill(b.idx, b.idx + b.nidx, b.idx[0]);   // every triangle degenerate: nothing drawn
                b.hidden = true;
            }
        }
        commit_op();
        build_topology();
        break;
    case C_DELETE: {
        // faces whose three corners are all selected go; the sub-mesh's list is re-stripped (vertices stay in the
        // buffer, unused)
        if (!sel_count()) { mstatus("select vertices first - faces with all three corners selected are deleted"); break; }
        begin_op("delete faces");
        int gone = delete_faces_where([](const Vb &vb, const std::array<WORD, 3> &t) {
            bool all = true;
            for (WORD i : t) all &= i < vb.count && S.gsel[S.grp[vb.base + i]] > 0;
            return all;
        });
        commit_op();
        build_topology();
        std::fill(S.gsel.begin(), S.gsel.end(), 0.0f);
        invalidate_all();
        notify_info();
        mstatus(gone ? "deleted %d faces (Undo brings them back; Save rebuilds the file's index data)"
                     : "no face has all three corners selected", gone);
        break;
    }
    case C_SEL_ALL: S.sel_is_pieces = false; std::fill(S.gsel.begin(), S.gsel.end(), 1.0f); notify_info(); break;
    case C_SEL_NONE: S.sel_is_pieces = false; std::fill(S.gsel.begin(), S.gsel.end(), 0.0f); notify_info(); break;
    case C_SEL_GROW: {
        S.sel_is_pieces = false;
        std::vector<float> s = S.gsel;
        for (int g = 0; g < S.ng; g++)
            if (S.gsel[g] > 0)
                for (int k = S.nstart[g]; k < S.nstart[g + 1]; k++) s[S.nbr[k]] = 1.0f;
        S.gsel = s;
        notify_info();
        mstatus("%d vertices selected", sel_count());
        break;
    }
    case C_MOVE:
        if (!sel_count()) { mstatus("select vertices first"); break; }
        begin_op("move");
        move_selection_world(M, c.f);
        renormal_moved();
        commit_op();
        break;
    case C_ROTATE:
    case C_SCALE:
        begin_op(c.type == C_ROTATE ? "rotate" : "scale");
        transform_selection(M, c.type == C_ROTATE ? c.f[0] : 0.0f, c.type == C_SCALE ? c.f[0] : 1.0f);
        renormal_moved();
        commit_op();
        break;
    case C_UNDO: undo_redo(true); break;
    case C_REDO: undo_redo(false); break;
    case C_REVERT:
        begin_op("revert");
        for (int gi = 0; gi < S.nv; gi++) {
            if (!memcmp(S.P[gi], origp(gi), stridep(gi))) continue;
            touch(gi);
            memcpy(S.P[gi], origp(gi), stridep(gi));
        }
        for (int t = 0; t < (int)S.texs.size(); t++)
            if (S.texs[t].bytes && memcmp(*(uint8_t **)(S.texs[t].hdr + 4), S.texs[t].orig.data(), S.texs[t].bytes)) {
                touch_tex(t);
                write_tex(t, S.texs[t].orig.data());
            }
        for (int s = 0; s < (int)S.subs.size(); s++) {
            Sub &b = S.subs[s];
            if (same_indices(b, b.orig)) continue;
            touch_sub(s);
            set_indices(s, b.orig);
            b.hidden = std::all_of(b.orig.begin(), b.orig.end(), [&](WORD w) { return w == b.orig[0]; });
        }
        build_topology();
        commit_op();
        mstatus("reverted to the loaded model (Undo brings the edits back)");
        break;
    case C_FILL: {
        if (!sel_count()) { mstatus("select vertices first"); break; }
        begin_op("fill colour");
        int n = 0;
        for (int g = 0; g < S.ng; g++)
            if (S.gsel[g] > 0) n += paint_group(g, S.gsel[g]);
        commit_op();
        invalidate_all();
        mstatus(n ? "filled %d vertices with %08X" : "this model's vertices carry no colour (stride 24) - nothing to paint",
                n, (unsigned)s_color);
        break;
    }
    case C_TEX_REPLACE:
    case C_TEX_RESTORE: {
        if (c.index < 0 || c.index >= (int)S.texs.size() || !S.texs[c.index].bytes) { mstatus("pick an editable texture first"); break; }
        Tex &t = S.texs[c.index];
        const std::vector<uint8_t> &src = c.type == C_TEX_RESTORE ? t.orig : c.data;
        if (src.size() != t.bytes) { mstatus("texture data size mismatch (%u vs %u)", (unsigned)src.size(), (unsigned)t.bytes); break; }
        begin_op(c.type == C_TEX_RESTORE ? "restore texture" : "replace texture");
        touch_tex(c.index);
        write_tex(c.index, src.data());
        commit_op();
        mstatus("%s %s (%dx%d)", c.type == C_TEX_RESTORE ? "restored" : "replaced", t.name.c_str(), t.w, t.h);
        break;
    }
    case C_TEX_EXPORT: {
        if (c.index < 0 || c.index >= (int)S.texs.size() || !S.texs[c.index].bytes) { mstatus("pick a DXT1 / DXT5 texture first"); break; }
        const Tex &t = S.texs[c.index];
        TexData *d = new TexData{t.name, t.w, t.h, t.fmt, {}};
        const uint8_t *src = *(uint8_t *const *)(t.hdr + 4);
        d->top.assign(src, src + (size_t)((t.w + 3) / 4) * ((t.h + 3) / 4) * (t.fmt == 0x0C ? 8 : 16));
        if (!g_mwnd || !PostMessageA(g_mwnd, WM_MESH_TEXDATA, 0, (LPARAM)d)) delete d;
        break;
    }
    case C_MERGE:                                       // armed: the next click in the game window places it
        g_gh.valid = false;
        g_fp.zlo.clear();
        g_place = MergeReq{c.file, c.f[0], c.add, false, {0, 0, 0}, c.name, c.f[1] != 0, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, c.f[2] != 0};
        g_ghost.clear();
        if (c.data.size() >= 8 && !memcmp(c.data.data(), "GPRV", 4)) {
            uint32_t n;
            memcpy(&n, c.data.data() + 4, 4);
            if (8 + (size_t)n * 24 <= c.data.size()) g_ghost.assign((const float *)(c.data.data() + 8), (const float *)(c.data.data() + 8) + n * 6);
        }
        for (int k = 0; k < 3; k++) g_ghost_lo[k] = 1e30f, g_ghost_hi[k] = -1e30f;
        for (size_t i = g_ghost.size() >= 72 ? g_ghost.size() - 72 : 0; i + 2 < g_ghost.size(); i += 3)
            for (int k = 0; k < 3; k++)
                g_ghost_lo[k] = (std::min)(g_ghost_lo[k], g_ghost[i + k]), g_ghost_hi[k] = (std::max)(g_ghost_hi[k], g_ghost[i + k]);
        g_place_armed = true;
        mstatus("move over the game window: the wireframe shows where it lands - wheel turns 5, Ctrl+wheel 45, "
                "Shift+wheel scales, click places (Stop editing cancels)");
        break;
    case C_ADD_COLORS: {
        bool any24 = false, other = false;
        for (auto &v : S.vbs) (v.stride == 24 ? any24 : other) |= v.stride != 28;
        if (!any24) { mstatus("this model already has vertex colours (or a layout this does not convert)"); break; }
        if (other) { mstatus("mixed vertex layouts - not converted"); break; }
        save(false, true);
        break;
    }
    case C_SAVE: save(false); break;
    case C_SAVE_SHARED: save(true); break;
    }
}

static void numpad_key(int vk, bool fine, const float *M);

void mesh_frame()                                       // game thread
{
    EnterCriticalSection(&g_mcs);
    std::vector<Cmd> cmds;
    cmds.swap(g_cmds);
    auto in = g_in;
    g_in.press = g_in.release = false;
    g_in.wheel = 0;
    g_in.keys.clear();
    LeaveCriticalSection(&g_mcs);
    if (g_on && (S.level ? *(uint8_t *volatile *)S.scene_var != S.scene : !ed_prop_alive(S.prop)))
        end_session("the level went away - mesh editing stopped (unsaved edits are gone)");
    const float *M = world_matrix();
    for (auto &c : cmds) {
        if (c.type != C_START && c.type != C_LEVEL && c.type != C_LEVEL_RESET && (!g_on || !M)) continue;
        run_cmd(c, M);
        M = world_matrix();
    }
    if (!g_on || !M) return;
    CamFrame cf;
    if (!ed_cam_frame(cf)) return;
    G.valid = in.valid;
    if (in.press) {
        G.px = in.px, G.py = in.py, G.shift = in.shift, G.ctrl = in.ctrl;
        G.nx = in.nx, G.ny = in.ny;
        do_press(cf, M);
    }
    G.nx = in.nx, G.ny = in.ny;
    if (g_stroke && (in.down || in.release)) {
        do_drag(cf, M);
        if (s_tool == T_SMOOTH || s_tool == T_INFLATE || s_tool == T_DEFLATE || s_tool == T_PAINT) {
            project(cf, M);
            brush_step(cf, M);
        } else if (s_tool == T_TEXPAINT) {
            texpaint_step(cf, M);
        }
        renormal_moved();
        invalidate_all();
    }
    if (in.release) { do_release(); invalidate_all(); }
    for (auto &k : in.keys) numpad_key(k.first, k.second, M);
    if (in.wheel && g_place_armed) {                    // placing a merge: the wheel turns / scales the ghost
        if (in.wshift) g_place.scale *= powf(1.05f, (float)in.wheel);
        else g_place.turn += (in.wctrl ? 45.0f : 5.0f) * in.wheel;
        while (g_place.turn > 180) g_place.turn -= 360;
        while (g_place.turn <= -180) g_place.turn += 360;
        in.wheel = 0;
    }
    if (in.wheel) {
        int t = s_tool;
        if (t == T_PIECE && !in.wctrl) {
            float c[3], zero[3] = {0, 0, 0};
            if (!S.pieces.empty() && piece_centre(M, c)) {
                begin_op(in.wshift ? "scale sub-mesh" : "turn sub-mesh");
                transform_pieces(M, zero, in.wshift ? 0.0f : 5.0f * in.wheel, in.wshift ? powf(1.05f, (float)in.wheel) : 1.0f, c,
                                 s_axis);              // Move along: x / y tilt, view plane / z turn
                commit_op();
                invalidate_all();
            } else {
                mstatus("pick a sub-mesh first (click it)");
            }
        } else if (t == T_MOVE && !in.wctrl) {
            begin_op(in.wshift ? "scale" : "rotate");
            transform_selection(M, in.wshift ? 0.0f : 5.0f * in.wheel, in.wshift ? powf(1.05f, (float)in.wheel) : 1.0f);
            renormal_moved();
            commit_op();
            invalidate_all();
        } else {
            float r = s_radius * powf(1.15f, (float)in.wheel);
            s_radius = r < 4 ? 4 : r > 320 ? 320 : r;
            if (g_mwnd) PostMessageA(g_mwnd, WM_MESH_RADIUS, 0, 0);
        }
    }
}

void mesh_draw(const CamFrame &cf)                      // game thread, overlay frame open
{
    if (!g_on) return;
    const float *M = world_matrix();
    if (!M) return;
    draw_ghost(cf, M);
    project(cf, M);
    if (s_wire) {
        size_t n = S.edges.size(), step = n > 30000 ? (n + 29999) / 30000 : 1;
        for (size_t i = 0; i < n; i += step) {
            int a = S.edges[i].first, b = S.edges[i].second;
            if (S.sz[a] < 0 || S.sz[b] < 0) continue;
            bool sel = S.gsel[a] > 0 && S.gsel[b] > 0;
            ov_line(S.sx[a], S.sy[a], S.sx[b], S.sy[b], 0.5f, sel ? 0xC0FFD040 : 0x6040FF90);
        }
    }
    int shown = 0;
    for (int g = 0; g < S.ng && shown < 40000; g++)
        if (S.gsel[g] > 0 && S.sz[g] >= 0) {
            ov_box(S.sx[g] - 1.0f, S.sy[g] - 1.0f, 2.0f, 2.0f, 0xFFFFD040);
            shown++;
        }
    int t = s_tool;
    if (G.valid && t != T_MOVE && t != T_PIECE) {   // brush ring
        float cx = ov_x(G.nx), cy = ov_y(G.ny), R = s_radius;
        uint32_t col = t == T_SELECT ? 0xC0FFFFFF : t == T_GRAB ? 0xC060C0FF : t == T_SMOOTH ? 0xC080FF80
                     : t == T_PAINT || t == T_TEXPAINT ? (0xE0000000 | (s_color & 0xFFFFFF)) : 0xC0FF8060;
        for (int k = 0; k < 40; k++) {
            float a0 = k * 6.2831853f / 40, a1 = (k + 1) * 6.2831853f / 40;
            ov_line(cx + R * cosf(a0), cy + R * sinf(a0), cx + R * cosf(a1), cy + R * sinf(a1), 1.0f, col);
        }
    }
    char b[256];
    _snprintf(b, sizeof b, "MESH  %s  |  tool %s%s  |  %d selected  |  undo %u%s", S.name.c_str(), k_tool_names[t],
              t == T_MOVE ? (s_axis == 0 ? " (view)" : s_axis == 1 ? " (x)" : s_axis == 2 ? " (y)" : " (z)") : "",
              sel_count(), (unsigned)S.undo.size(), S.dirty ? "  |  UNSAVED" : "");
    b[sizeof b - 1] = 0;
    ov_text(b, 12, 14, OV_LEFT, 0xFFFFE080, true);
}

// ------------------------------------------------------------------------------------------------ input (game window)
bool mesh_active() { return g_on; }

void mesh_press(float nx, float ny, bool shift, bool ctrl)
{
    EnterCriticalSection(&g_mcs);
    g_in.press = true, g_in.down = true, g_in.valid = true;
    g_in.px = g_in.nx = nx, g_in.py = g_in.ny = ny;
    g_in.shift = shift, g_in.ctrl = ctrl;
    LeaveCriticalSection(&g_mcs);
}
void mesh_move(float nx, float ny, bool down)
{
    EnterCriticalSection(&g_mcs);
    g_in.nx = nx, g_in.ny = ny, g_in.valid = true;
    if (!down && g_in.down && !g_in.release) g_in.release = true;   // button let go outside the window
    if (!down) g_in.down = false;
    LeaveCriticalSection(&g_mcs);
}
void mesh_release()
{
    EnterCriticalSection(&g_mcs);
    if (g_in.down) g_in.release = true;
    g_in.down = false;
    LeaveCriticalSection(&g_mcs);
}
void mesh_wheel(int notches, bool shift, bool ctrl)
{
    EnterCriticalSection(&g_mcs);
    g_in.wheel += notches;
    g_in.wshift = shift, g_in.wctrl = ctrl;
    LeaveCriticalSection(&g_mcs);
}
bool mesh_key(int vk)                                   // game window keys: true = used
{
    if (vk == 'Z' || vk == 'Y') { post_simple(vk == 'Z' ? C_UNDO : C_REDO); return true; }
    if (vk < VK_NUMPAD0 || vk > VK_DIVIDE) return false;
    EnterCriticalSection(&g_mcs);
    g_in.keys.push_back({vk, (GetKeyState(VK_SHIFT) & 0x8000) != 0});
    LeaveCriticalSection(&g_mcs);
    return true;
}

// numpad: 4 / 6 turn, 8 / 2 tilt about x, 7 / 9 tilt about y, + / - scale (5 degrees / 5 %, Shift 1), 5 reset -
// the merge placement while it is armed, else the picked pieces of Grab sub-mesh
static void numpad_key(int vk, bool fine, const float *M)
{
    float step = fine ? 1.0f : 5.0f, sc = fine ? 1.01f : 1.05f;
    int axis = -1;
    float deg = 0, scale = 1;
    switch (vk) {
    case VK_NUMPAD4: axis = 3, deg = step; break;
    case VK_NUMPAD6: axis = 3, deg = -step; break;
    case VK_NUMPAD8: axis = 1, deg = step; break;
    case VK_NUMPAD2: axis = 1, deg = -step; break;
    case VK_NUMPAD7: axis = 2, deg = step; break;
    case VK_NUMPAD9: axis = 2, deg = -step; break;
    case VK_ADD: scale = sc; break;
    case VK_SUBTRACT: scale = 1 / sc; break;
    case VK_NUMPAD5: break;
    case VK_NUMPAD1: case VK_NUMPAD3: break;            // height (below)
    default: return;
    }
    if (vk == VK_NUMPAD1 || vk == VK_NUMPAD3) {          // down / up: 0.25 units, Shift 0.05
        float dz = (vk == VK_NUMPAD3 ? 1.0f : -1.0f) * (fine ? 0.05f : 0.25f);
        if (g_place_armed) { g_place.lift += dz; return; }
        if (s_tool != T_PIECE || S.pieces.empty()) return;
        float c[3], dw[3] = {0, 0, dz};
        if (!piece_centre(M, c)) return;
        begin_op("raise / lower sub-mesh");
        transform_pieces(M, dw, 0, 1, c);
        commit_op();
        invalidate_all();
        return;
    }
    if (g_place_armed) {
        if (vk == VK_NUMPAD5) g_place.turn = g_place.lean_x = g_place.lean_y = g_place.lift = 0, g_place.scale = 1;
        else if (axis == 3) g_place.turn += deg;
        else if (axis == 1) g_place.lean_x += deg;
        else if (axis == 2) g_place.lean_y += deg;
        else g_place.scale *= scale;
        return;
    }
    if (s_tool != T_PIECE || S.pieces.empty() || vk == VK_NUMPAD5) return;
    float c[3], zero[3] = {0, 0, 0};
    if (!piece_centre(M, c)) return;
    begin_op(axis < 0 ? "scale sub-mesh" : "turn sub-mesh");
    transform_pieces(M, zero, axis < 0 ? 0.0f : deg, scale, c, axis < 0 ? 0 : axis);
    commit_op();
    invalidate_all();
}

// ------------------------------------------------------------------------------------------------ window
enum { IDM_MODEL = 300, IDM_SUBS, IDM_SELSUB, IDM_HIDE, IDM_STOP, IDM_TOOL, IDM_AXIS = IDM_TOOL + T_COUNT, IDM_RADIUS,
       IDM_STRENGTH, IDM_FRONT, IDM_NORMALS, IDM_WIRE, IDM_ALL, IDM_NONE, IDM_GROW, IDM_DX, IDM_DY, IDM_DZ, IDM_MOVE,
       IDM_DEG, IDM_ROT, IDM_PCT, IDM_SCALE, IDM_UNDO, IDM_REDO, IDM_REVERT, IDM_SAVE, IDM_SAVESHARED, IDM_STATUS,
       IDM_COLOR, IDM_SWATCH, IDM_FILL, IDM_TEXS, IDM_TEXREPLACE, IDM_TEXRESTORE, IDM_TEXEXPORT, IDM_DELETE,
       IDM_ADDCOLORS, IDM_MERGE, IDM_MERGEFIT, IDM_MERGEGROUND, IDM_LEVEL, IDM_MERGEUP, IDM_MERGELEVEL, IDM_MERGECLEAR };

struct TexInfo { std::string name; int w, h, levels, fmt; size_t bytes; };
static struct Info {
    bool on, colors;
    std::string model, name;
    std::vector<std::string> subs;
    std::vector<TexInfo> texs;
    int nv, ntri, sel, undo, redo;
    bool dirty;
} g_info;

static void notify_info()                               // game thread: snapshot for the window
{
    Info i;
    i.on = g_on;
    i.model = S.model, i.name = S.name;
    for (auto &b : S.subs) {
        std::string tx;
        for (int t : b.tex) tx += (tx.empty() ? "" : ", ") + S.texs[t].name;
        char l[256];
        _snprintf(l, sizeof l, "%s   %d verts%s   %s", b.name.c_str(), (int)b.verts.size(), b.hidden ? "   [hidden]" : "",
                  tx.c_str());
        l[sizeof l - 1] = 0;
        i.subs.push_back(l);
    }
    for (auto &t : S.texs) i.texs.push_back({t.name, t.w, t.h, t.levels, t.fmt, t.bytes});
    i.colors = false;
    for (auto &v : S.vbs) i.colors |= v.stride == 28 || v.stride == 36;
    i.nv = S.nv, i.ntri = (int)S.tris.size();
    i.sel = g_on ? sel_count() : 0;
    i.undo = (int)S.undo.size(), i.redo = (int)S.redo.size();
    i.dirty = S.dirty;
    EnterCriticalSection(&g_mcs);
    g_info = i;
    LeaveCriticalSection(&g_mcs);
    if (g_mwnd) PostMessageA(g_mwnd, WM_MESH_INFO, 0, 0);
}

// ------------------------------------------------------------------------------------------------ texture import
// image file (PNG / TGA / BMP / JPG via WIC) -> RGBA at the texture's size -> mip chain -> DXT1 / DXT5 levels laid
// out like the Xbox texture (level after level, 4x4 blocks)
static bool load_image(const char *path, std::vector<uint8_t> &rgba, UINT &w, UINT &h)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    wchar_t wp[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *fr = nullptr;
    IWICFormatConverter *cv = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(wp, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192) {
        rgba.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    f->Release();
    return ok;
}

static std::vector<uint8_t> resample(const std::vector<uint8_t> &src, int sw, int sh, int dw, int dh)   // area average
{
    std::vector<uint8_t> out((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (std::max)(x0 + 1, (x + 1) * sw / dw);
            int y0 = y * sh / dh, y1 = (std::max)(y0 + 1, (y + 1) * sh / dh);
            unsigned acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++, n++)
                    for (int c = 0; c < 4; c++) acc[c] += src[((size_t)yy * sw + xx) * 4 + c];
            for (int c = 0; c < 4; c++) out[((size_t)y * dw + x) * 4 + c] = (uint8_t)((acc[c] + n / 2) / (n ? n : 1));
        }
    return out;
}

static uint16_t to565(const float c[3])
{
    auto q = [](float v, int m) { int i = (int)lroundf(v * m / 255.0f); return i < 0 ? 0 : i > m ? m : i; };
    return (uint16_t)(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}
static void from565(uint16_t c, float o[3])
{
    o[0] = (float)((c >> 11) & 31) * 255 / 31, o[1] = (float)((c >> 5) & 63) * 255 / 63, o[2] = (float)(c & 31) * 255 / 31;
}

static void dxt_color_block(const uint8_t px[16][4], uint8_t out[8])    // 4-colour mode, principal-axis endpoints
{
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) for (int c = 0; c < 3; c++) mean[c] += px[i][c] / 16.0f;
    float cov[3][3] = {};
    for (int i = 0; i < 16; i++)
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++) cov[a][b] += (px[i][a] - mean[a]) * (px[i][b] - mean[b]);
    float ax[3] = {1, 1, 1};
    for (int it = 0; it < 4; it++) {
        float n[3];
        for (int a = 0; a < 3; a++) n[a] = cov[a][0] * ax[0] + cov[a][1] * ax[1] + cov[a][2] * ax[2];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
        for (int a = 0; a < 3; a++) ax[a] = n[a] / l;
    }
    int lo = 0, hi = 0;
    float plo = 1e30f, phi = -1e30f;
    for (int i = 0; i < 16; i++) {
        float p = (px[i][0] - mean[0]) * ax[0] + (px[i][1] - mean[1]) * ax[1] + (px[i][2] - mean[2]) * ax[2];
        if (p < plo) plo = p, lo = i;
        if (p > phi) phi = p, hi = i;
    }
    float chi[3] = {(float)px[hi][0], (float)px[hi][1], (float)px[hi][2]};
    float clo[3] = {(float)px[lo][0], (float)px[lo][1], (float)px[lo][2]};
    uint16_t c0 = to565(chi), c1 = to565(clo);
    if (c0 < c1) std::swap(c0, c1);
    float pal[4][3], p0[3], p1[3];
    from565(c0, p0), from565(c1, p1);
    for (int c = 0; c < 3; c++) {
        pal[0][c] = p0[c], pal[1][c] = p1[c];
        pal[2][c] = (2 * p0[c] + p1[c]) / 3, pal[3][c] = (p0[c] + 2 * p1[c]) / 3;
    }
    uint32_t bits = 0;
    if (c0 != c1)                                       // c0 == c1 would be 3-colour mode: index 0 everywhere
        for (int i = 0; i < 16; i++) {
            int best = 0;
            float bd = 1e30f;
            for (int k = 0; k < 4; k++) {
                float d = 0;
                for (int c = 0; c < 3; c++) d += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
                if (d < bd) bd = d, best = k;
            }
            bits |= (uint32_t)best << (2 * i);
        }
    memcpy(out, &c0, 2), memcpy(out + 2, &c1, 2), memcpy(out + 4, &bits, 4);
}

static void dxt_alpha_block(const uint8_t px[16][4], uint8_t out[8])    // 8-value mode (a0 > a1)
{
    int a0 = 0, a1 = 255;
    for (int i = 0; i < 16; i++) a0 = (std::max)(a0, (int)px[i][3]), a1 = (std::min)(a1, (int)px[i][3]);
    if (a0 == a1) { if (a0 < 255) a0++; else a1--; }
    float vals[8];
    static const int wgt[8] = {0, 7, 1, 2, 3, 4, 5, 6};
    for (int k = 0; k < 8; k++) vals[k] = ((7 - wgt[k]) * a0 + wgt[k] * a1) / 7.0f;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        float bd = 1e30f;
        for (int k = 0; k < 8; k++) {
            float d = fabsf(px[i][3] - vals[k]);
            if (d < bd) bd = d, best = k;
        }
        bits |= (uint64_t)best << (3 * i);
    }
    out[0] = (uint8_t)a0, out[1] = (uint8_t)a1;
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

static void encode_levels(std::vector<uint8_t> img, int w, int h, int levels, bool dxt5, std::vector<uint8_t> &out)
{
    out.clear();
    for (int l = 0; l < levels; l++) {
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++) {
                uint8_t px[16][4];
                for (int i = 0; i < 16; i++) {
                    int x = (std::min)(bx * 4 + (i & 3), w - 1), y = (std::min)(by * 4 + (i >> 2), h - 1);
                    memcpy(px[i], &img[((size_t)y * w + x) * 4], 4);
                }
                uint8_t blk[16];
                if (dxt5) {
                    dxt_alpha_block(px, blk);
                    dxt_color_block(px, blk + 8);
                    out.insert(out.end(), blk, blk + 16);
                } else {
                    dxt_color_block(px, blk);
                    out.insert(out.end(), blk, blk + 8);
                }
            }
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        img = resample(img, w, h, nw, nh);
        w = nw, h = nh;
    }
}

static void decode_dxt(const TexData &d, std::vector<uint8_t> &rgba)    // top level -> RGBA
{
    dxt_decode(d.top.data(), d.w, d.h, d.fmt, rgba);
}

static void dxt_decode(const uint8_t *src, int w, int h, int fmt, std::vector<uint8_t> &rgba)
{
    struct { int w, h; } d = {w, h};
    rgba.assign((size_t)d.w * d.h * 4, 255);
    bool dxt5 = fmt == 0x0F;
    const uint8_t *p = src;
    for (int by = 0; by < (d.h + 3) / 4; by++)
        for (int bx = 0; bx < (d.w + 3) / 4; bx++, p += dxt5 ? 16 : 8) {
            uint8_t alpha[16];
            memset(alpha, 255, sizeof alpha);
            const uint8_t *cb = p;
            if (dxt5) {
                int a0 = p[0], a1 = p[1], av[8] = {a0, a1};
                for (int k = 2; k < 8; k++)
                    av[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255;
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++) bits |= (uint64_t)p[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)av[(bits >> (3 * i)) & 7];
                cb = p + 8;
            }
            uint16_t c0, c1;
            uint32_t idx;
            memcpy(&c0, cb, 2), memcpy(&c1, cb + 2, 2), memcpy(&idx, cb + 4, 4);
            float p0[3], p1[3], pal[4][4];
            from565(c0, p0), from565(c1, p1);
            for (int c = 0; c < 3; c++) {
                pal[0][c] = p0[c], pal[1][c] = p1[c];
                if (c0 > c1 || dxt5) pal[2][c] = (2 * p0[c] + p1[c]) / 3, pal[3][c] = (p0[c] + 2 * p1[c]) / 3;
                else pal[2][c] = (p0[c] + p1[c]) / 2, pal[3][c] = 0;
            }
            for (int k = 0; k < 4; k++) pal[k][3] = 255;
            if (!(c0 > c1) && !dxt5) pal[3][3] = 0;
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= d.w || y >= d.h) continue;
                const float *c = pal[(idx >> (2 * i)) & 3];
                uint8_t *o = &rgba[((size_t)y * d.w + x) * 4];
                o[0] = (uint8_t)c[0], o[1] = (uint8_t)c[1], o[2] = (uint8_t)c[2];
                o[3] = dxt5 ? alpha[i] : (uint8_t)c[3];
            }
        }
}

// PNG via WIC as 32bpp BGRA (the PNG encoder's own format - asking for RGBA can come back as BGRA)
static bool write_png(const char *path, const std::vector<uint8_t> &rgba, UINT w, UINT h)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory *f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (FAILED(hr)) { port_log("mesh: export - WIC factory failed %08lX (CoInit %08lX)\n", hr, co); return false; }
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    wchar_t wp[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    int step = 0;
    hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) step++, hr = st->InitializeFromFilename(wp, GENERIC_WRITE);
    if (SUCCEEDED(hr)) step++, hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) step++, hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) step++, hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) step++, hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) step++, hr = fr->SetSize(w, h);
    if (SUCCEEDED(hr)) step++, hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr) && !IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
    if (SUCCEEDED(hr)) step++, hr = fr->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data());
    if (SUCCEEDED(hr)) step++, hr = fr->Commit();
    if (SUCCEEDED(hr)) step++, hr = enc->Commit();
    if (FAILED(hr)) port_log("mesh: export %s failed at step %d: %08lX\n", path, step, hr);
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    f->Release();
    return SUCCEEDED(hr);
}

static void export_texture(TexData *d)                   // editor thread
{
    char path[MAX_PATH];
    std::string base = d->name;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base.erase(dot);
    _snprintf(path, sizeof path, "%s.png", base.c_str());
    path[sizeof path - 1] = 0;
    OPENFILENAMEA of = {};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_mwnd;
    of.lpstrFilter = "PNG image\0*.png\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = "png";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&of)) return;
    std::vector<uint8_t> rgba;
    decode_dxt(*d, rgba);
    char b[300];
    _snprintf(b, sizeof b, write_png(path, rgba, d->w, d->h) ? "exported %s (%dx%d) - edit it, then Replace with image"
                                                                : "could NOT write %s", path, d->w, d->h);
    b[sizeof b - 1] = 0;
    SetWindowTextA(g_mstatus, b);
}

static int selected_tex()
{
    return (int)SendDlgItemMessageA(g_mwnd, IDM_TEXS, LB_GETCURSEL, 0, 0);
}

static void replace_texture()                           // editor thread
{
    int t = selected_tex();
    EnterCriticalSection(&g_mcs);
    TexInfo ti = t >= 0 && t < (int)g_info.texs.size() ? g_info.texs[t] : TexInfo{};
    LeaveCriticalSection(&g_mcs);
    if (t < 0 || ti.name.empty()) { SetWindowTextA(g_mstatus, "pick a texture in the list first"); return; }
    if (!ti.bytes) { SetWindowTextA(g_mstatus, "only DXT1 / DXT5 textures can be replaced so far"); return; }
    char path[MAX_PATH] = "";
    OPENFILENAMEA of = {};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_mwnd;
    of.lpstrFilter = "Images (png, tga, bmp, jpg)\0*.png;*.tga;*.bmp;*.jpg;*.jpeg\0All files\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&of)) return;
    std::vector<uint8_t> rgba;
    UINT iw = 0, ih = 0;
    if (!load_image(path, rgba, iw, ih)) { SetWindowTextA(g_mstatus, "could not read that image"); return; }
    if (ti.fmt == 0x0C)                                 // DXT1 here is opaque
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    Cmd c{};
    c.type = C_TEX_REPLACE;
    c.index = t;
    encode_levels(resample(rgba, iw, ih, ti.w, ti.h), ti.w, ti.h, ti.levels, ti.fmt == 0x0F, c.data);
    if (c.data.size() != ti.bytes) { SetWindowTextA(g_mstatus, "encoded size does not match the texture"); return; }
    post_cmd(c);
    char b[300];
    _snprintf(b, sizeof b, "%s -> %s (%ux%u image, scaled to %dx%d)", path, ti.name.c_str(), iw, ih, ti.w, ti.h);
    b[sizeof b - 1] = 0;
    SetWindowTextA(g_mstatus, b);
}

// ------------------------------------------------------------------------------------------------ window controls
static HWND madd(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_mwnd, (HMENU)(INT_PTR)id, nullptr,
                           nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}
static float mget(int id)
{
    char b[64];
    GetDlgItemTextA(g_mwnd, id, b, sizeof b);
    return (float)atof(b);
}
static void mset(int id, float v)
{
    char b[32];
    _snprintf(b, sizeof b, "%g", v);
    SetDlgItemTextA(g_mwnd, id, b);
}

static std::vector<int> selected_subs()
{
    std::vector<int> out;
    int n = (int)SendMessageA(g_msubs, LB_GETSELCOUNT, 0, 0);
    if (n > 0) {
        out.resize(n);
        SendMessageA(g_msubs, LB_GETSELITEMS, n, (LPARAM)out.data());
    }
    return out;
}

static HBRUSH g_swatch_brush;
static void show_color()
{
    uint32_t c = s_color;
    char b[32];
    _snprintf(b, sizeof b, "%02X %02X %02X  a %02X", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, c >> 24);
    SetDlgItemTextA(g_mwnd, IDM_SWATCH, b);
    if (g_swatch_brush) DeleteObject(g_swatch_brush);
    g_swatch_brush = CreateSolidBrush(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
    InvalidateRect(GetDlgItem(g_mwnd, IDM_SWATCH), nullptr, TRUE);
}

static void pick_color(HWND h)
{
    static COLORREF custom[16];
    uint32_t c = s_color;
    CHOOSECOLORA cc = {};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = h;
    cc.rgbResult = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorA(&cc)) return;
    s_color = (c & 0xFF000000) | (uint32_t)GetRValue(cc.rgbResult) << 16 | (uint32_t)GetGValue(cc.rgbResult) << 8 |
              GetBValue(cc.rgbResult);
    show_color();
}

static LRESULT CALLBACK mesh_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MESH_STATUS: {
        std::string *s = (std::string *)lp;
        SetWindowTextA(g_mstatus, s->c_str());
        delete s;
        return 0;
    }
    case WM_MESH_LEVELRESET: {
        std::string *rel = (std::string *)lp;
        std::string game = port_game_dir(), base = rel->substr(0, rel->size() > 4 ? rel->size() - 4 : rel->size());
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        _snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        stamp[31] = 0;
        int moved = 0;
        for (const char *ext : {".pao", ".ptx"}) {
            std::string from = game + "\\data\\" + base + ext;
            if (GetFileAttributesA(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::string to = game + "\\editor_backups\\" + stamp + "\\" + base + ext;
            for (size_t i = game.size() + 1; i < to.size(); i++)   // the backup folders
                if (to[i] == (char)0x5C) { to[i] = 0; CreateDirectoryA(to.c_str(), nullptr); to[i] = (char)0x5C; }
            if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) moved++;
            else port_log("mesh: reset - could not move %s (%lu)\n", from.c_str(), GetLastError());
        }
        if (moved) {
            port_editor_reload();                       // rescan (the loose copy is gone) + load the level again
            mstatus("%s reset to the original - your version is in editor_backups\\%s", rel->c_str(), stamp);
        } else {
            mstatus("%s has no edited copy - it is already the original", rel->c_str());
        }
        delete rel;
        return 0;
    }
    case WM_MESH_PREVIEWED: {
        PreviewRun *r = (PreviewRun *)lp;
        HANDLE f = CreateFileA(r->out.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(f, nullptr), n = 0;
            r->c.data.resize(sz);
            if (sz) ReadFile(f, r->c.data.data(), sz, &n, nullptr);
            CloseHandle(f);
        }
        if (r->exit != 0 || r->c.data.size() < 8)
            SetWindowTextA(g_mstatus, "preview failed - see paxmerge.log; placing without the wireframe");
        post_cmd(r->c);                                 // arm the placement (with or without the ghost)
        delete r;
        return 0;
    }
    case WM_MESH_MERGED: {
        MergeRun *r = (MergeRun *)lp;
        if (r->exit == 0) {
            mstatus("%s: %s", r->split_json.empty() ? "merged" : "split", r->log.c_str());
            finish_after_merge(r->job, r->own);
        } else {
            mstatus("merge FAILED (%ld): %s - see paxmerge.log next to the exe; the saved copy is unchanged",
                    (long)r->exit, r->log.c_str());
        }
        delete r->job;
        delete r;
        return 0;
    }
    case WM_MESH_SAVED: {
        SaveJob *job = (SaveJob *)lp;
        finish_save(job);
        delete job;
        return 0;
    }
    case WM_MESH_TEXDATA: {
        TexData *d = (TexData *)lp;
        export_texture(d);
        delete d;
        return 0;
    }
    case WM_MESH_PICKED: {
        std::vector<int> *v = (std::vector<int> *)lp;
        SendMessageA(g_msubs, LB_SETSEL, FALSE, -1);
        for (int k : *v) SendMessageA(g_msubs, LB_SETSEL, TRUE, k);
        if (!v->empty()) SendMessageA(g_msubs, LB_SETTOPINDEX, (*v)[0], 0);
        delete v;
        return 0;
    }
    case WM_MESH_RADIUS:
        mset(IDM_RADIUS, floorf(s_radius + 0.5f));
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(h, IDM_SWATCH) && g_swatch_brush) {
            uint32_t c = s_color;
            int lum = (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10;
            SetBkColor((HDC)wp, RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
            SetTextColor((HDC)wp, lum > 128 ? RGB(0, 0, 0) : RGB(255, 255, 255));
            return (LRESULT)g_swatch_brush;
        }
        break;
    case WM_MESH_INFO: {
        EnterCriticalSection(&g_mcs);
        Info i = g_info;
        LeaveCriticalSection(&g_mcs);
        std::vector<int> keep = selected_subs();
        SendMessageA(g_msubs, LB_RESETCONTENT, 0, 0);
        for (auto &s : i.subs) SendMessageA(g_msubs, LB_ADDSTRING, 0, (LPARAM)s.c_str());
        for (int k : keep) SendMessageA(g_msubs, LB_SETSEL, TRUE, k);
        int tsel = selected_tex();
        SendDlgItemMessageA(h, IDM_TEXS, LB_RESETCONTENT, 0, 0);
        for (auto &t : i.texs) {
            char l[200];
            _snprintf(l, sizeof l, "%s   %dx%d %s%s", t.name.c_str(), t.w, t.h,
                      t.fmt == 0x0C ? "DXT1" : t.fmt == 0x0F ? "DXT5" : "other", t.bytes ? "" : "  (not editable yet)");
            l[sizeof l - 1] = 0;
            SendDlgItemMessageA(h, IDM_TEXS, LB_ADDSTRING, 0, (LPARAM)l);
        }
        if (tsel >= 0) SendDlgItemMessageA(h, IDM_TEXS, LB_SETCURSEL, tsel, 0);
        char b[512];
        if (i.on)
            _snprintf(b, sizeof b, "%s  (%s)\r\n%d sub-meshes, %d vertices, %d triangles - %d selected - undo %d / redo %d%s%s",
                      i.model.c_str(), i.name.c_str(), (int)i.subs.size(), i.nv, i.ntri, i.sel, i.undo, i.redo,
                      i.colors ? "" : " - no vertex colours", i.dirty ? " - UNSAVED" : "");
        else
            _snprintf(b, sizeof b, "No mesh open: select a prop in the Props window and press \"Edit this prop's mesh\".");
        b[sizeof b - 1] = 0;
        SetWindowTextA(g_mmodel, b);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= IDM_TOOL && id < IDM_TOOL + T_COUNT && code == BN_CLICKED) s_tool = id - IDM_TOOL;
        else if (id == IDM_AXIS && code == CBN_SELCHANGE) s_axis = (int)SendDlgItemMessageA(h, IDM_AXIS, CB_GETCURSEL, 0, 0);
        else if (id == IDM_RADIUS && code == EN_CHANGE) { float r = mget(IDM_RADIUS); if (r >= 2 && r <= 400) s_radius = r; }
        else if (id == IDM_STRENGTH && code == EN_CHANGE) { float s = mget(IDM_STRENGTH); if (s > 0 && s <= 1) s_strength = s; }
        else if (id == IDM_FRONT) s_front = IsDlgButtonChecked(h, IDM_FRONT) == BST_CHECKED;
        else if (id == IDM_NORMALS) s_renormal = IsDlgButtonChecked(h, IDM_NORMALS) == BST_CHECKED;
        else if (id == IDM_WIRE) s_wire = IsDlgButtonChecked(h, IDM_WIRE) == BST_CHECKED;
        else if (id == IDM_SELSUB || id == IDM_HIDE) {
            Cmd c{};
            c.type = id == IDM_SELSUB ? C_SEL_SUB : C_HIDE_SUB;
            c.list = selected_subs();
            c.add = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (c.list.empty()) SetWindowTextA(g_mstatus, "pick sub-meshes in the list first");
            else post_cmd(c);
        } else if (id == IDM_STOP) post_simple(C_STOP);
        else if (id == IDM_LEVEL) { post_simple(C_LEVEL); SetWindowTextA(g_mstatus, "looking for the level's model..."); }
        else if (id == IDM_ALL) post_simple(C_SEL_ALL);
        else if (id == IDM_NONE) post_simple(C_SEL_NONE);
        else if (id == IDM_GROW) post_simple(C_SEL_GROW);
        else if (id == IDM_DELETE) post_simple(C_DELETE);
        else if (id == IDM_MERGE) {
            char path[MAX_PATH] = "";
            OPENFILENAMEA of = {};
            of.lStructSize = sizeof of;
            of.hwndOwner = h;
            of.lpstrFilter = "glTF binary (*.glb)\0*.glb\0";
            of.lpstrFile = path;
            of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&of)) {
                Cmd c{};
                c.type = C_MERGE;
                c.file = path;                          // (the GLB)
                c.f[0] = mget(IDM_MERGEFIT);
                c.add = IsDlgButtonChecked(h, IDM_MERGEGROUND) == BST_CHECKED;
                static const char *const ups[] = {"+z", "-y", "+y", "-z", "+x", "-x"};
                int ui = (int)SendDlgItemMessageA(h, IDM_MERGEUP, CB_GETCURSEL, 0, 0);
                c.name = ups[ui >= 0 && ui < 6 ? ui : 0];
                c.f[1] = IsDlgButtonChecked(h, IDM_MERGELEVEL) == BST_CHECKED ? 1.0f : 0.0f;
                c.f[2] = IsDlgButtonChecked(h, IDM_MERGECLEAR) == BST_CHECKED ? 1.0f : 0.0f;
                PreviewRun *pr = new PreviewRun{c, "", 0};
                CloseHandle(CreateThread(nullptr, 0, preview_thread, pr, 0, nullptr));
                SetWindowTextA(g_mstatus, "shaping the part for the placement preview...");
            }
        }
        else if (id == IDM_MOVE) post_simple(C_MOVE, mget(IDM_DX), mget(IDM_DY), mget(IDM_DZ));
        else if (id == IDM_ROT) post_simple(C_ROTATE, mget(IDM_DEG));
        else if (id == IDM_SCALE) { float p = mget(IDM_PCT); if (p > 0) post_simple(C_SCALE, p / 100.0f); }
        else if (id == IDM_COLOR) pick_color(h);
        else if (id == IDM_FILL) post_simple(C_FILL);
        else if (id == IDM_ADDCOLORS) {
            if (MessageBoxA(h, "Give this model vertex colours?\n\nThis saves the prop's own copy now (with your current "
                            "edits) and reloads the level; open the mesh again afterwards to paint.", "Gladius Editor",
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                post_simple(C_ADD_COLORS);
        }
        else if (id == IDM_TEXREPLACE) replace_texture();
        else if (id == IDM_TEXEXPORT) {
            Cmd c{};
            c.type = C_TEX_EXPORT;
            c.index = selected_tex();
            post_cmd(c);
        } else if (id == IDM_TEXRESTORE) {
            Cmd c{};
            c.type = C_TEX_RESTORE;
            c.index = selected_tex();
            post_cmd(c);
        } else if (id == IDM_UNDO) post_simple(C_UNDO);
        else if (id == IDM_REDO) post_simple(C_REDO);
        else if (id == IDM_REVERT) post_simple(C_REVERT);
        else if (id == IDM_SAVE) post_simple(C_SAVE);
        else if (id == IDM_SAVESHARED) {
            if (MessageBoxA(h, "Write the edit over the shared model?\n\nEvery prop and every level that uses this model "
                            "changes. \"Save (this prop)\" writes a private copy instead.", "Gladius Editor",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                post_simple(C_SAVE_SHARED);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void mesh_init() { InitializeCriticalSection(&g_mcs); }   // port_editor_init, before any frame

void mesh_create_window(HINSTANCE inst, HWND parent, int x, int y0, int w, int h)   // editor thread
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = mesh_wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "GladiusEditorMesh";
    RegisterClassA(&wc);
    g_mwnd = CreateWindowExA(WS_EX_CONTROLPARENT, wc.lpszClassName, "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, x, y0,
                             w, h, parent, nullptr, inst, nullptr);
    int y = 8;
    g_mmodel = madd("STATIC", "", 0, 10, y, 350, 44, IDM_MODEL); y += 48;
    g_msubs = madd("LISTBOX", "", WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL, 10, y, 350, 120, IDM_SUBS);
    SendMessageA(g_msubs, LB_SETHORIZONTALEXTENT, 700, 0);
    y += 124;
    madd("BUTTON", "Select sub-mesh", BS_PUSHBUTTON, 10, y, 86, 24, IDM_SELSUB);
    madd("BUTTON", "Hide / show", BS_PUSHBUTTON, 100, y, 80, 24, IDM_HIDE);
    madd("BUTTON", "Edit level", BS_PUSHBUTTON, 184, y, 86, 24, IDM_LEVEL);
    madd("BUTTON", "Stop editing", BS_PUSHBUTTON, 274, y, 86, 24, IDM_STOP);
    y += 32;
    madd("STATIC", "Tool (in the game window):", 0, 10, y, 350, 16, -1); y += 18;
    for (int t = 0; t < T_COUNT; t++)
        madd("BUTTON", k_tool_names[t], BS_AUTORADIOBUTTON | (t == 0 ? WS_GROUP : 0), 10 + (t % 3) * 116, y + (t / 3) * 22,
             110, 20, IDM_TOOL + t);
    CheckDlgButton(g_mwnd, IDM_TOOL, BST_CHECKED);
    y += ((T_COUNT + 2) / 3) * 22 + 4;
    madd("STATIC", "Move along", 0, 10, y + 3, 80, 16, -1);
    HWND ax = madd("COMBOBOX", "", CBS_DROPDOWNLIST | WS_GROUP, 92, y, 120, 200, IDM_AXIS);
    const char *axes[] = {"view plane", "world X", "world Y", "world Z"};
    for (auto a : axes) SendMessageA(ax, CB_ADDSTRING, 0, (LPARAM)a);
    SendMessageA(ax, CB_SETCURSEL, 0, 0);
    y += 28;
    madd("STATIC", "Brush radius", 0, 10, y + 3, 80, 16, -1);
    madd("EDIT", "30", WS_BORDER | ES_AUTOHSCROLL, 92, y, 60, 20, IDM_RADIUS);
    madd("STATIC", "Strength (0-1)", 0, 170, y + 3, 90, 16, -1);
    madd("EDIT", "0.3", WS_BORDER | ES_AUTOHSCROLL, 262, y, 60, 20, IDM_STRENGTH);
    y += 26;
    madd("BUTTON", "Visible faces only", BS_AUTOCHECKBOX, 10, y, 116, 20, IDM_FRONT);
    madd("BUTTON", "Fix normals", BS_AUTOCHECKBOX, 130, y, 100, 20, IDM_NORMALS);
    madd("BUTTON", "Wireframe", BS_AUTOCHECKBOX, 236, y, 100, 20, IDM_WIRE);
    CheckDlgButton(g_mwnd, IDM_FRONT, BST_CHECKED);
    CheckDlgButton(g_mwnd, IDM_NORMALS, BST_CHECKED);
    CheckDlgButton(g_mwnd, IDM_WIRE, BST_CHECKED);
    y += 28;
    madd("STATIC", "Paint colour", 0, 10, y + 4, 80, 16, -1);
    madd("STATIC", "", SS_CENTER | SS_SUNKEN, 92, y + 1, 110, 20, IDM_SWATCH);
    madd("BUTTON", "Colour...", BS_PUSHBUTTON, 208, y, 72, 24, IDM_COLOR);
    madd("BUTTON", "Fill sel.", BS_PUSHBUTTON, 286, y, 74, 24, IDM_FILL);
    show_color();
    y += 28;
    madd("BUTTON", "Add vertex colours to this model (save + reload)", BS_PUSHBUTTON, 10, y, 350, 22, IDM_ADDCOLORS);
    y += 30;
    madd("STATIC", "Selection", 0, 10, y + 4, 80, 16, -1);
    madd("BUTTON", "All", BS_PUSHBUTTON, 92, y, 62, 24, IDM_ALL);
    madd("BUTTON", "None", BS_PUSHBUTTON, 158, y, 62, 24, IDM_NONE);
    madd("BUTTON", "Grow", BS_PUSHBUTTON, 224, y, 62, 24, IDM_GROW);
    madd("BUTTON", "Delete faces", BS_PUSHBUTTON, 290, y, 70, 24, IDM_DELETE);
    y += 32;
    madd("STATIC", "Move by", 0, 10, y + 3, 80, 16, -1);
    madd("EDIT", "0", WS_BORDER | ES_AUTOHSCROLL, 92, y, 60, 20, IDM_DX);
    madd("EDIT", "0", WS_BORDER | ES_AUTOHSCROLL, 156, y, 60, 20, IDM_DY);
    madd("EDIT", "0", WS_BORDER | ES_AUTOHSCROLL, 220, y, 60, 20, IDM_DZ);
    madd("BUTTON", "Move", BS_PUSHBUTTON, 288, y - 1, 72, 23, IDM_MOVE);
    y += 26;
    madd("STATIC", "Turn (deg, z)", 0, 10, y + 3, 80, 16, -1);
    madd("EDIT", "15", WS_BORDER | ES_AUTOHSCROLL, 92, y, 60, 20, IDM_DEG);
    madd("BUTTON", "Turn", BS_PUSHBUTTON, 156, y - 1, 60, 23, IDM_ROT);
    madd("EDIT", "110", WS_BORDER | ES_AUTOHSCROLL, 228, y, 52, 20, IDM_PCT);
    madd("BUTTON", "Scale %", BS_PUSHBUTTON, 288, y - 1, 72, 23, IDM_SCALE);
    y += 32;
    madd("STATIC", "Textures:", 0, 10, y, 350, 16, -1); y += 18;
    madd("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, y, 350, 64, IDM_TEXS); y += 66;
    madd("BUTTON", "Export as PNG...", BS_PUSHBUTTON, 10, y, 112, 24, IDM_TEXEXPORT);
    madd("BUTTON", "Replace with image...", BS_PUSHBUTTON, 128, y, 130, 24, IDM_TEXREPLACE);
    madd("BUTTON", "Restore original", BS_PUSHBUTTON, 264, y, 96, 24, IDM_TEXRESTORE);
    y += 34;
    madd("BUTTON", "Merge GLB...", BS_PUSHBUTTON, 10, y, 96, 24, IDM_MERGE);
    madd("STATIC", "fit size", 0, 114, y + 4, 44, 16, -1);
    madd("EDIT", "0", WS_BORDER | ES_AUTOHSCROLL, 160, y + 1, 44, 20, IDM_MERGEFIT);
    madd("BUTTON", "on ground, centred", BS_AUTOCHECKBOX, 212, y + 2, 148, 20, IDM_MERGEGROUND);
    CheckDlgButton(g_mwnd, IDM_MERGEGROUND, BST_CHECKED);
    y += 26;
    madd("STATIC", "GLB up", 0, 10, y + 4, 44, 16, -1);
    HWND upc = madd("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 56, y, 196, 200, IDM_MERGEUP);
    const char *upn[] = {"standard (glTF Y-up)", "Z-up export / scans (-y)", "+y", "upside down", "+x", "-x"};
    for (auto u : upn) SendMessageA(upc, CB_ADDSTRING, 0, (LPARAM)u);
    SendMessageA(upc, CB_SETCURSEL, 0, 0);
    madd("BUTTON", "level (scans)", BS_AUTOCHECKBOX, 260, y + 2, 100, 20, IDM_MERGELEVEL);
    y += 24;
    madd("BUTTON", "clear what it overlaps (existing faces cut away under its footprint)", BS_AUTOCHECKBOX, 10, y, 350, 20,
         IDM_MERGECLEAR);
    y += 28;
    madd("BUTTON", "Undo", BS_PUSHBUTTON, 10, y, 80, 26, IDM_UNDO);
    madd("BUTTON", "Redo", BS_PUSHBUTTON, 96, y, 80, 26, IDM_REDO);
    madd("BUTTON", "Revert all", BS_PUSHBUTTON, 182, y, 84, 26, IDM_REVERT);
    madd("BUTTON", "Save (this prop)", BS_DEFPUSHBUTTON, 272, y, 88, 26, IDM_SAVE);
    y += 30;
    madd("BUTTON", "Save over the shared model (every prop / level using it)", BS_PUSHBUTTON, 10, y, 350, 22, IDM_SAVESHARED);
    y += 30;
    g_mstatus = madd("STATIC", "", 0, 10, y, 350, 32, IDM_STATUS);
    y += 36;
    madd("STATIC",
         "Game window: click / drag = tool. Select: Shift adds, Ctrl removes. Wheel = brush size; with Move: turn, "
         "Shift+wheel scale. Paint uses the colour + strength. Ctrl+Z / Ctrl+Y undo / redo. Save writes this prop's own "
         "copy (data, mesh, custom) and points its .prp at it.",
         0, 10, y, 350, 60, -1);
    PostMessageA(g_mwnd, WM_MESH_INFO, 0, 0);
}

bool mesh_window_message(MSG *m)                        // editor thread
{
    if (!g_mwnd || (m->hwnd != g_mwnd && !IsChild(g_mwnd, m->hwnd))) return false;
    if (m->message == WM_KEYDOWN && (m->wParam == 'Z' || m->wParam == 'Y') && (GetKeyState(VK_CONTROL) & 0x8000)) {
        char cls[16] = "";
        GetClassNameA(m->hwnd, cls, sizeof cls);
        if (_stricmp(cls, "Edit")) {                    // edit boxes keep their own Ctrl+Z
            post_simple(m->wParam == 'Z' ? C_UNDO : C_REDO);
            return true;
        }
    }
    return false;                                       // dialog keys: the frame's IsDialogMessage
}

void mesh_reset_level()                                 // editor thread: the Level page's Reset button
{
    if (MessageBoxA(g_mwnd, "Reset this level to the original?\n\nIts edited .pao / .ptx are moved to the game's "
                    "editor_backups folder and the level reloads from gladius.bec. Unsaved edits are lost.",
                    "Gladius Editor", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    post_simple(C_LEVEL_RESET);
    if (g_mstatus) SetWindowTextA(g_mstatus, "resetting the level...");
}

void mesh_edit_level()                                  // editor thread: the Level page's button
{
    post_simple(C_LEVEL);
    if (g_mstatus) SetWindowTextA(g_mstatus, "looking for the level's model...");
}

void mesh_edit_prop(const std::string &file, int live, const std::string &model, const std::string &name)   // editor thread
{
    if (!g_mwnd) return;
    Cmd c{};
    c.type = C_START;
    c.file = file, c.live = live, c.model = model, c.name = name;
    post_cmd(c);
    SetWindowTextA(g_mstatus, ("opening " + model + "...").c_str());
}
