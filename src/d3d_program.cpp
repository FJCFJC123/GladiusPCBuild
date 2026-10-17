// Engine-program draws: the engine's own vertex/pixel shaders (translated to HLSL), vertex attribute formats (VAF),
// shader constants, constant vertex attributes, immediate mode (push SET_BEGIN_END / INLINE_ARRAY), indexed and
// UP draws, and texture stages bound by header (SetTexture) or raw offset/format (SwitchTexture / push methods).
#include "d3d.h"
#include "d3d_prof.h"
#include <intrin.h>
#include "shader_translate.h"
#include <d3dcompiler.h>
#include <array>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <math.h>
#include <unordered_map>
#include <vector>

extern DWORD g_cur_vs, g_cur_ps;                       // d3d_shader.cpp
extern const BYTE *g_cur_vaf;
void d3d_expand_primitive(DWORD type, UINT count, std::vector<UINT> &idx, D3D11_PRIMITIVE_TOPOLOGY *topo);

// ------------------------------------------------------------------------------------------------ state
static float g_vsc[192][4];                            // vertex shader constants (hardware slots 0..191)
static float g_attr_const[16][4];                      // constant value of attributes that are not streamed
static DWORD g_vsc_gen = 1;                             // bumped on every g_vsc / g_attr_const write (draw_program)
static float g_psc[8][4];                              // pixel shader constants
static DWORD g_const_load;                             // SET_TRANSFORM_CONSTANT_LOAD pointer
struct Stage {
    DWORD data, format, size;
};
static Stage g_stage[4];
struct Stream {
    XResource *vb;
    UINT stride;
};
static Stream g_streams[16];
static bool g_in_begin;
static DWORD g_begin_prim;
static std::vector<DWORD> g_inline;

void d3d_stage_set(int stage, DWORD data, DWORD format, DWORD size)
{
    g_stage[stage] = {data, format, size};
}

static void attr_defaults()
{
    static bool done;
    if (done)
        return;
    done = true;
    for (int i = 0; i < 16; i++)
        g_attr_const[i][0] = g_attr_const[i][1] = g_attr_const[i][2] = 0, g_attr_const[i][3] = 1;
    g_attr_const[3][0] = g_attr_const[3][1] = g_attr_const[3][2] = 1;   // diffuse: white
    g_vsc_gen++;
}

// ------------------------------------------------------------------------------------------------ VAF helpers
struct VafSlot {
    DWORD stream, offset, format, tess;
};
static const VafSlot *vaf_slots(const BYTE *vaf) { return (const VafSlot *)vaf; }

static UINT attr_bytes(DWORD fmt)
{
    int type = fmt & 15, size = (fmt >> 4) & 15;
    if (fmt == 0x02 || !size) return 0;
    switch (type) {
    case 2: return 4 * size;
    case 0: case 4: case 6: return 4;
    case 1: case 5: return (2 * size + 3) & ~3u;
    }
    return 4 * size;
}

static DXGI_FORMAT attr_dxgi(DWORD fmt)
{
    int type = fmt & 15, size = (fmt >> 4) & 15;
    switch (type) {
    case 2: return size == 1 ? DXGI_FORMAT_R32_FLOAT : size == 2 ? DXGI_FORMAT_R32G32_FLOAT
                 : size == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 0: return DXGI_FORMAT_B8G8R8A8_UNORM;         // D3DCOLOR
    case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 6: return DXGI_FORMAT_R32_UINT;               // packed normal, decoded in the shader
    case 1: return size == 1 ? DXGI_FORMAT_R16_SNORM : size == 2 ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16B16A16_SNORM;
    case 5: return size <= 2 ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16B16A16_SINT;
    }
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// ------------------------------------------------------------------------------------------------ pipeline cache
struct VsKey {
    DWORD id, formats[16];
    bool operator<(const VsKey &o) const { return memcmp(this, &o, sizeof *this) < 0; }
};
struct VsEntry {
    ID3D11VertexShader *vs;
    ID3DBlob *code;
};
struct LayoutKey {
    VsKey vs;
    DWORD stream[16], offset[16];
    bool operator<(const LayoutKey &o) const { return memcmp(this, &o, sizeof *this) < 0; }
};
static std::map<VsKey, VsEntry> g_vs_cache;
static std::map<LayoutKey, ID3D11InputLayout *> g_layout_cache;
static std::map<DWORD, ID3D11PixelShader *> g_ps_cache;
static std::map<unsigned long long, ID3D11PixelShader *> g_ps_lit_cache;   // (ps id << 32 | vs id): per-pixel lighting
static std::map<DWORD, VsLighting> g_vs_lighting;     // vs id -> its recognised lighting (ok = false: none)
static ID3D11Buffer *g_cb_vs, *g_cb_ps;

static const VsLighting &vs_lighting(DWORD id)
{
    auto it = g_vs_lighting.find(id);
    if (it != g_vs_lighting.end())
        return it->second;
    VsLighting &l = g_vs_lighting[id];
    const XgShader *sh = xg_shader(id);
    if (sh && analyze_lighting(sh->source, &l)) {
        if (l.ok)
            port_log("shader: vs #%u lighting recognised: %d lights%s%s\n", id, l.nlights,
                     l.color0 ? ", x colour" : "", l.env ? ", sphere map" : "");
        else
            port_log("shader: vs #%u baked lighting recognised (sun relight with a normal stream)\n", id);
    }
    return l;
}
static ID3D11SamplerState *g_samplers[4];

// Sampler per stage from the deferred texture states the engine writes directly (D3D_g_DeferredTextureState
// 0x2C51D0 [stage][32]: 0 ADDRESSU, 1 ADDRESSV, 2 ADDRESSW, 3 MAGFILTER, 4 MINFILTER, 5 MIPFILTER, 8 MAXANISOTROPY)
static std::map<unsigned long long, ID3D11SamplerState *> g_sampler_cache;

static D3D11_TEXTURE_ADDRESS_MODE xb_address(DWORD a)
{
    switch (a) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;      // CLAMP / CLAMPTOEDGE
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

static ID3D11SamplerState *stage_sampler(int stage)
{
    const DWORD *ts = (const DWORD *)0x2C51D0 + stage * 32;
    DWORD au = ts[0] & 7, av = ts[1] & 7, aw = ts[2] & 7, mag = ts[3] & 7, mn = ts[4] & 7, mip = ts[5] & 3;
    unsigned long long key = au | av << 3 | aw << 6 | mag << 9 | mn << 12 | mip << 15;
    static unsigned long long last_key[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    static ID3D11SamplerState *last[4];
    if (key == last_key[stage] && last[stage])
        return last[stage];
    ID3D11SamplerState *&ss = g_sampler_cache[key];
    last_key[stage] = key;
    if (ss)
        return last[stage] = ss;
    last[stage] = nullptr;                              // created below; looked up again next time
    if (ss)
        return ss;
    D3D11_SAMPLER_DESC sd = {};
    bool lmag = mag >= 2, lmin = mn >= 2, lmip = mip >= 2;
    if (mag == 3 || mn == 3) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
    } else {
        sd.Filter = (D3D11_FILTER)((lmin ? 0x10 : 0) | (lmag ? 0x4 : 0) | (lmip ? 0x1 : 0));
    }
    sd.AddressU = xb_address(au);
    sd.AddressV = xb_address(av);
    sd.AddressW = xb_address(aw);
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : 0;                // MIPFILTER NONE: base level only
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    g_d3d.dev->CreateSamplerState(&sd, &ss);
    return ss;
}

static void dump_failed(const char *what, DWORD id, const std::string &hlsl, ID3DBlob *err)
{
    port_log("%s #%u translation failed to compile: %s\n", what, id, err ? (const char *)err->GetBufferPointer() : "?");
}

// Compiled shaders persist in build\shadercache (one .cso per HLSL text + profile, FNV-1a 64 of both): D3DCompile cost
// 100 ms - 1.3 s per shader and every new one appeared mid-frame - the battle intro camera pan reveals the arena,
// characters and effects one by one and stuttered on each first use (2026-10-08 profile). Every compile is logged
// with its translate / compile time and whether the cache had it.
static bool compile_cached(const std::string &hlsl, const char *target, ID3DBlob **code, ID3DBlob **err, bool *hit)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    for (unsigned char c : hlsl) h = (h ^ c) * 0x100000001B3ull;
    for (const char *t = target; *t; t++) h = (h ^ (unsigned char)*t) * 0x100000001B3ull;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, 0x5C);
    strcpy(slash + 1, "shadercache");
    CreateDirectoryA(path, nullptr);
    _snprintf(slash + 1, 64, "shadercache\\%s_%016llx.cso", target, h);
    *hit = false;
    if (FILE *f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, code)) && fread((*code)->GetBufferPointer(), 1, n, f) == (size_t)n) {
            fclose(f);
            *hit = true;
            return true;
        }
        fclose(f);
        if (*code) (*code)->Release(), *code = nullptr;
    }
    if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), target, nullptr, nullptr, "main", target, 0, 0, code, err)))
        return false;
    char tmp[MAX_PATH];                                 // write + rename: a second instance never reads half a file
    _snprintf(tmp, MAX_PATH, "%s.%u.tmp", path, GetCurrentProcessId());
    if (FILE *f = fopen(tmp, "wb")) {
        fwrite((*code)->GetBufferPointer(), 1, (*code)->GetBufferSize(), f);
        fclose(f);
        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            DeleteFileA(tmp);
    }
    return true;
}

// [debug] DumpShaders=1: build\shaders\<vs|ps>_<source hash>.txt = engine source + our HLSL, written on first use
static void dump_shader(const char *kind, DWORD id, const std::string &src, const std::string &hlsl)
{
    static int on = -1;
    if (on < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        on = GetPrivateProfileIntA("debug", "DumpShaders", 0, ini);
    }
    if (!on)
        return;
    DWORD h = 0x811C9DC5;
    for (unsigned char ch : src) h = (h ^ ch) * 0x01000193;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, 0x5C);
    strcpy(slash + 1, "shaders");
    CreateDirectoryA(path, nullptr);
    _snprintf(slash + 1, 64, "shaders\\%s_%08x.txt", kind, h);
    if (FILE *f = fopen(path, "wb")) {
        fprintf(f, "// %s #%u first used frame %u\n// ---- engine source\n%s\n// ---- HLSL\n%s", kind, id, g_d3d.frame,
                src.c_str(), hlsl.c_str());
        fclose(f);
    }
}

static double ms_since(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}

static VsEntry *get_vs(const VsKey &k)
{
    auto it = g_vs_cache.find(k);
    if (it != g_vs_cache.end())
        return it->second.vs ? &it->second : nullptr;
    VsEntry &e = g_vs_cache[k];
    ProfScope pz(PZ_COMPILE);
    prof_count(PC_COMPILES);
    const XgShader *sh = xg_shader(k.id);
    if (!sh)
        return nullptr;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    std::vector<std::string> warn;
    std::string hlsl = translate_vs(sh->source, k.formats, &warn, &vs_lighting(k.id));
    double tr = ms_since(t0);
    dump_shader("vs", k.id, sh->source, hlsl);
    ID3DBlob *err = nullptr;
    bool hit;
    if (!compile_cached(hlsl, "vs_4_0", &e.code, &err, &hit)) {
        dump_failed("vertex shader", k.id, hlsl, err);
        return nullptr;
    }
    g_d3d.dev->CreateVertexShader(e.code->GetBufferPointer(), e.code->GetBufferSize(), nullptr, &e.vs);
    port_log("shader: vs #%u frame %u translate %.1f ms, %s %.1f ms (%u bytes HLSL)\n", k.id, g_d3d.frame, tr,
             hit ? "cache hit" : "COMPILE", ms_since(t0) - tr, (UINT)hlsl.size());
    return &e;
}

static ID3D11PixelShader *get_ps(DWORD id)
{
    auto it = g_ps_cache.find(id);
    if (it != g_ps_cache.end())
        return it->second;
    ID3D11PixelShader *&ps = g_ps_cache[id];
    ProfScope pz(PZ_COMPILE);
    prof_count(PC_COMPILES);
    const XgShader *sh = xg_shader(id);
    if (!sh)
        return nullptr;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    std::vector<std::string> warn;
    std::string hlsl = translate_ps(sh->source, &warn);
    double tr = ms_since(t0);
    dump_shader("ps", id, sh->source, hlsl);
    ID3DBlob *code = nullptr, *err = nullptr;
    bool hit;
    if (!compile_cached(hlsl, "ps_4_0", &code, &err, &hit)) {
        dump_failed("pixel shader", id, hlsl, err);
        return nullptr;
    }
    g_d3d.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    code->Release();
    port_log("shader: ps #%u frame %u translate %.1f ms, %s %.1f ms (%u bytes HLSL)\n", id, g_d3d.frame, tr,
             hit ? "cache hit" : "COMPILE", ms_since(t0) - tr, (UINT)hlsl.size());
    return ps;
}

// pixel shader evaluating vertex shader vs_id's lighting per pixel
static ID3D11PixelShader *get_ps_lit(DWORD id, DWORD vs_id)
{
    unsigned long long key = (unsigned long long)id << 32 | vs_id;
    auto it = g_ps_lit_cache.find(key);
    if (it != g_ps_lit_cache.end())
        return it->second;
    ID3D11PixelShader *&ps = g_ps_lit_cache[key];
    ProfScope pz(PZ_COMPILE);
    const XgShader *sh = xg_shader(id);
    if (!sh)
        return nullptr;
    std::vector<std::string> warn;
    std::string hlsl = translate_ps(sh->source, &warn, &vs_lighting(vs_id));
    dump_shader("psl", id, sh->source, hlsl);
    ID3DBlob *code = nullptr, *err = nullptr;
    bool hit;
    if (!compile_cached(hlsl, "ps_4_0", &code, &err, &hit)) {
        dump_failed("per-pixel lighting pixel shader", id, hlsl, err);
        return nullptr;
    }
    g_d3d.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    code->Release();
    port_log("shader: ps #%u lit for vs #%u (%s)\n", id, vs_id, hit ? "cache hit" : "COMPILE");
    return ps;
}

static ID3D11InputLayout *get_layout(const LayoutKey &k, VsEntry *vs)
{
    auto it = g_layout_cache.find(k);
    if (it != g_layout_cache.end())
        return it->second;
    ProfScope pz(PZ_COMPILE);
    D3D11_INPUT_ELEMENT_DESC el[16];
    UINT n = 0;
    for (int s = 0; s < 16; s++) {
        DWORD f = k.vs.formats[s];
        if ((f & 0xFF) == 0x02 || !f)
            continue;
        el[n++] = {"V", (UINT)s, attr_dxgi(f), k.stream[s], k.offset[s], D3D11_INPUT_PER_VERTEX_DATA, 0};
    }
    ID3D11InputLayout *&l = g_layout_cache[k];
    HRESULT hr = g_d3d.dev->CreateInputLayout(el, n, vs->code->GetBufferPointer(), vs->code->GetBufferSize(), &l);
    if (FAILED(hr))
        port_log("input layout for shader #%u failed %08X\n", k.vs.id, hr);
    return l;
}

// Constant buffer contents: uploaded only when they differ from what the buffer already holds. Per-draw
// UpdateSubresource of the full 3.3 KB vertex block cost every draw a driver copy and a GPU dependency.
static void cb_put(ID3D11Buffer *b, float *shadow, bool *valid, const float *data, UINT bytes)
{
    if (*valid && !memcmp(shadow, data, bytes))
        return;
    D3D11_MAPPED_SUBRESOURCE m;
    prof_count(PC_CB_UPLOADS);
    if (SUCCEEDED(g_d3d.ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        g_d3d.ctx->Unmap(b, 0);
        memcpy(shadow, data, bytes);
        *valid = true;
    }
}

// Transient data (UP / inline vertices, expanded indices) is appended to a ring buffer (NO_OVERWRITE) instead of
// re-discarding one buffer per draw; only a wrap discards. One Map per draw writes all of its pieces, so a wrap can
// never discard a piece written earlier for the same draw.
struct Ring {
    ID3D11Buffer *buf;
    UINT cap, pos, bind;
};
static Ring g_vring = {nullptr, 0, 0, D3D11_BIND_VERTEX_BUFFER}, g_iring = {nullptr, 0, 0, D3D11_BIND_INDEX_BUFFER};

static BYTE *ring_map(Ring &r, UINT bytes, UINT *offset)
{
    if (!r.buf || bytes > r.cap) {
        if (r.buf)
            r.buf->Release();
        r.buf = nullptr;
        r.cap = bytes < (4u << 20) ? 4u << 20 : bytes * 2;
        r.pos = r.cap;                                  // first Map discards
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = r.cap;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = r.bind;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3d.dev->CreateBuffer(&bd, nullptr, &r.buf)))
            return nullptr;
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (r.pos + bytes > r.cap)
        r.pos = 0, mode = D3D11_MAP_WRITE_DISCARD;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_d3d.ctx->Map(r.buf, 0, mode, 0, &m)))
        return nullptr;
    *offset = r.pos;
    r.pos += (bytes + 15) & ~15u;
    return (BYTE *)m.pData + *offset;
}

// Vertex stream cache. Indexed draws reference big shared vertex buffers (the world uses a few with ~20k
// vertices); re-uploading [0, max index] per draw cost ~200 MB/frame. Instead each source address keeps a GPU
// copy mirrored in 64 KB chunks: a chunk is hashed at most once per frame and re-uploaded only when its bytes
// changed. Chunks are always verified over the buffer's full known extent (the largest range any draw used), not
// the current draw's range, and a Lock only drops the "verified this frame" mark: before 2026-10-08 a Lock reset the
// chunk lengths and per-draw ranges kept changing the last chunk's length, and either one forced an upload of
// unchanged bytes - AltahrunRuins re-uploaded 26.8 MB of vertices every frame (14 ms CPU, plus GPU copy time).
static const UINT VB_CHUNK = 64 * 1024;
struct VbChunk {
    DWORD frame, len, hash_a, hash_b;
};
struct VbEntry {
    ID3D11Buffer *buf;
    UINT cap, extent;                                   // GPU buffer size; largest range any draw has used
    std::vector<VbChunk> chunks;
};
static std::unordered_map<DWORD, VbEntry> g_vb_cache;

void d3d_vb_invalidate_all()
{
    D3DLock lock;
    for (auto &kv : g_vb_cache)                         // game memory replaced: re-verify (and re-upload) all
        for (VbChunk &k : kv.second.chunks)
            k.frame = 0xFFFFFFFF, k.len = 0;
}

// the game locked a vertex buffer for writing: drop the verified state of its chunks (same-frame rewrites)
void d3d_vb_invalidate(DWORD data)
{
    auto it = g_vb_cache.find(data);
    if (it != g_vb_cache.end())                         // re-hash before the next use; upload only if bytes changed
        for (VbChunk &k : it->second.chunks)
            k.frame = 0xFFFFFFFF;
}

static void chunk_hash(const BYTE *p, UINT len, DWORD *ha, DWORD *hb)
{
    DWORD a = 0x811C9DC5 ^ len, b = 0x9747B28C;
    const DWORD *d = (const DWORD *)p;
    UINT n = len / 8;
    for (UINT i = 0; i < n; i++) {
        a = (a ^ d[i * 2]) * 0x9E3779B1;
        b = (b ^ d[i * 2 + 1]) * 0x85EBCA77;
    }
    for (UINT i = n * 8; i < len; i++)
        a = (a ^ p[i]) * 0x01000193;
    *ha = a;
    *hb = b;
}

static ID3D11Buffer *cached_stream(const BYTE *src, UINT bytes)
{
    VbEntry &e = g_vb_cache[(DWORD)src];
    if (!e.buf || e.cap < bytes) {                      // (re)create: everything must be uploaded again
        if (e.buf)
            e.buf->Release();
        e.cap = bytes < 4096 ? 4096 : (bytes + bytes / 2 + 4095) & ~4095u;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = e.cap;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        e.buf = nullptr;
        g_d3d.dev->CreateBuffer(&bd, nullptr, &e.buf);
        e.chunks.assign((e.cap + VB_CHUNK - 1) / VB_CHUNK, VbChunk{0xFFFFFFFF, 0, 0, 0});
    }
    if (!e.buf)
        return nullptr;
    if (bytes > e.extent)
        e.extent = bytes;
    for (UINT c = 0, off = 0; off < bytes; c++, off += VB_CHUNK) {
        UINT len = e.extent - off < VB_CHUNK ? e.extent - off : VB_CHUNK;   // the chunk as far as the buffer is known
        VbChunk &k = e.chunks[c];
        if (k.frame == g_d3d.frame && k.len == len)
            continue;                                   // verified this frame
        DWORD ha, hb;
        {
            ProfScope pz(PZ_VBHASH);
            prof_count(PC_VB_HASHED, len);
            chunk_hash(src + off, len, &ha, &hb);
        }
        if (k.len != len || k.hash_a != ha || k.hash_b != hb) {
            ProfScope pz(PZ_VBUPLOAD);
            prof_count(PC_VB_UPLOADED, len);
            if (k.len && k.len != len)
                prof_count(PC_VB_LEN_UPLOADS);
            D3D11_BOX box = {off, 0, 0, off + len, 1, 1};
            g_d3d.ctx->UpdateSubresource(e.buf, 0, &box, src + off, 0, 0);
            k.len = len, k.hash_a = ha, k.hash_b = hb;
        }
        k.frame = g_d3d.frame;
    }
    return e.buf;
}

static void init_program_objects()
{
    if (g_cb_vs)
        return;
    attr_defaults();
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;                       // written with Map(DISCARD) by cb_put, when changed
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = (192 + 16 + 2) * 16;                   // c[192], attr_const[16], rt_size, material
    g_d3d.dev->CreateBuffer(&bd, nullptr, &g_cb_vs);
    bd.ByteWidth = (8 + 1 + 1 + 4 + 1 + 1 + 1 + 5 + 1 + 3 + 1 + 8 + 1) * 16;  // ... fx params, fx lights, material
    g_d3d.dev->CreateBuffer(&bd, nullptr, &g_cb_ps);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    for (int i = 0; i < 4; i++)
        g_d3d.dev->CreateSamplerState(&sd, &g_samplers[i]);
}

// ------------------------------------------------------------------------------------------------ materials
// Surface effects by texture asset name ([video] Materials = strength, 0 = off): 1 water (ripples + moving glints),
// 2 lava (flowing UVs, glows above white for HDR / bloom), 3 glowing props (braziers, lanterns: flicker), 4 foliage
// cards (cut-out .cc leaves / bushes / palms / plants: gentle sway), 5 cloth (banners, flags: wider sway).
// Glowing ones also skip the sun shadow (G-buffer depth negative). Names: the named HD pack (2026-10-10 survey).
static int material_of_key(const std::string &k)
{
    if (k.empty())
        return 0;
    auto has = [&](const char *s) { return k.find(s) != std::string::npos; };
    auto starts = [&](const char *s) { return k.compare(0, strlen(s), s) == 0; };
    int m = 0;
    bool gear = has("affinity") || has("titan") || has("beast") || has("armor") || has("skin") || has("helm") ||
                has("crown") || has("shield") || has("staff");
    bool ui = starts("hud") || starts("icon") || starts("ui_") || starts("a_") || starts("sky") || starts("font");
    bool cc = k.size() > 3 && !k.compare(k.size() - 3, 3, ".cc");
    if (ui || gear)
        m = 0;
    else if (has("water") || has("river") || has("ocean"))
        m = 1;
    else if (has("lava") || has("magma"))
        m = 2;
    else if (has("brazier") || has("lantern") || has("torch") || has("candle") || has("nugget_glow"))
        m = 3;
    else if (has("banner") || has("flag"))
        m = 5;
    else if (cc && (has("leaf") || has("leaves") || has("bush") || has("palm") || has("fern") || has("plant") ||
                    has("flower") || has("ivy") || has("vine") || has("tree") || has("grass") || has("reed")))
        m = 4;
    static const char *names[] = {"", "water", "lava", "glow", "foliage", "cloth"};
    if (m)
        port_log("material: '%s' -> %s\n", k.c_str(), names[m]);
    return m;
}

// Per-draw texture facts by (Data, Format): asset name, material, shadow role. tex_key() locks the HD loader and
// copies a string; two calls per draw (material + shadow caster name) cost ~1.5 ms a frame on the world map
// (2026-10-10). Entries are re-derived when the name registry changes (g_tex_names_gen).
extern volatile LONG g_tex_names_gen;                   // tex_replace.cpp
struct TexInfo {
    LONG gen;
    int material;
    bool blob_shadow;                                   // the engine's blob shadow ("shadow"): not drawn with real shadows
    bool no_cast;                                       // sky / sun: never casts
    bool logged;                                        // [diagnostic] "shadow caster" line written
    const char *name;                                   // interned ("" = unknown)
};
static const TexInfo &tex_info(DWORD data, DWORD format)
{
    static std::unordered_map<unsigned long long, TexInfo> cache;
    static std::set<std::string> names;                 // interned names (node addresses stay valid)
    TexInfo &t = cache[(unsigned long long)data << 32 | format];
    LONG gen = g_tex_names_gen;
    if (t.gen != gen) {
        std::string k = tex_key(data, format);
        bool logged = t.name && k == t.name && t.logged;
        t.gen = gen;
        t.name = names.insert(k).first->c_str();
        t.material = material_of_key(k);
        t.blob_shadow = k == "shadow";
        t.no_cast = !k.compare(0, 3, "sky") || !k.compare(0, 4, "sun_");
        t.logged = logged;
    }
    return t;
}

// ------------------------------------------------------------------------------------------------ effect lights
// Additive particle draws (fire, sparks, glows - drawn inside the engine's particle renderer) are light sources:
// position = their vertices' centroid in view space (P^-1 * WVP), colour = average vertex colour x the texture's
// average colour (sampled once per texture), weight = brightness x size. Nearby ones merge; the four strongest of a
// frame light the next frame's lit characters and relit arena surfaces ([video] FxLights = intensity, 0 = off).
struct FxLight {
    float pos[3], col[3], w;
};
static std::vector<FxLight> g_fx_frame;
static FxLight g_fx_active[4];
static int g_fx_count;
static DWORD g_fx_frame_no = 0xFFFFFFFF;
static std::map<DWORD, std::array<float, 3>> g_tex_avg;

void port_screenshot(const char *path);   // d3d_core.cpp

static void fx_roll()
{
    if (g_fx_frame_no == g_d3d.frame)
        return;
    g_fx_frame_no = g_d3d.frame;
    std::sort(g_fx_frame.begin(), g_fx_frame.end(), [](const FxLight &a, const FxLight &b) { return a.w > b.w; });
    g_fx_count = (int)(g_fx_frame.size() < 4 ? g_fx_frame.size() : 4);
    for (int k = 0; k < g_fx_count; k++)
        g_fx_active[k] = g_fx_frame[k];
    static DWORD logged;
    static int shots = -1;                              // [debug] FxLightShots=N: screenshot strong effect lights
    if (shots < 0)
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        shots = GetPrivateProfileIntA("debug", "FxLightShots", 0, ini);
    }
    static DWORD shot_at;
    if (shots > 0 && g_fx_count && g_fx_active[0].w > 6 && g_d3d.frame - shot_at > 90) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        _snprintf(strrchr(path, 0x5C) + 1, 64, "shot_fx_%u.bmp", g_d3d.frame);
        port_screenshot(path);
        shot_at = g_d3d.frame, shots--;
    }
    if (g_fx_count && g_d3d.frame - logged >= 1800 && (logged = g_d3d.frame, true))
        port_log("fxlights: %d of %u, top (%.1f %.1f %.1f) col (%.2f %.2f %.2f) w %.2f\n", g_fx_count,
                 (unsigned)g_fx_frame.size(), g_fx_active[0].pos[0], g_fx_active[0].pos[1], g_fx_active[0].pos[2],
                 g_fx_active[0].col[0], g_fx_active[0].col[1], g_fx_active[0].col[2], g_fx_active[0].w);
    g_fx_frame.clear();
}

static std::array<float, 3> tex_average(DWORD data, DWORD format, DWORD size)
{
    if (!data || size)                                  // linear: skip (white)
        return {1, 1, 1};
    auto it = g_tex_avg.find(data ^ format);
    if (it != g_tex_avg.end())
        return it->second;
    UINT w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
    float s[3] = {}, n = 0;
    for (UINT y = 0; y < 4; y++)
        for (UINT x = 0; x < 4; x++) {
            DWORD c = d3d_texel(data, format, (x * 2 + 1) * w / 8, (y * 2 + 1) * h / 8);
            float a = (c >> 24) / 255.0f;
            s[0] += ((c >> 16) & 255) / 255.0f * a, s[1] += ((c >> 8) & 255) / 255.0f * a, s[2] += (c & 255) / 255.0f * a;
            n += 1;
        }
    std::array<float, 3> r = {s[0] / n, s[1] / n, s[2] / n};
    g_tex_avg[data ^ format] = r;
    return r;
}

static void fx_light_collect(const BYTE *const streams[16], const UINT strides[16], UINT vertex_count,
                             const VafSlot *slots, const LayoutKey &lk, const float *vsc)
{
    fx_roll();
    DWORD pf = lk.vs.formats[0];
    if ((pf & 15) != 2 || ((pf >> 4) & 15) < 3 || !streams[slots[0].stream] || !vertex_count)
        return;                                         // position: float3+ in v0
    int ci = (lk.vs.formats[3] & 0xFF) == 0x40 ? 3 : (lk.vs.formats[2] & 0xFF) == 0x40 ? 2 : -1;   // D3DCOLOR
    UINT n = vertex_count < 512 ? vertex_count : 512;
    float c[3] = {}, col[3] = {};
    for (UINT v = 0; v < n; v++) {
        const float *p = (const float *)(streams[slots[0].stream] + strides[slots[0].stream] * v + slots[0].offset);
        c[0] += p[0], c[1] += p[1], c[2] += p[2];
        if (ci >= 0) {
            DWORD d = *(const DWORD *)(streams[slots[ci].stream] + strides[slots[ci].stream] * v + slots[ci].offset);
            float a = (d >> 24) / 255.0f;
            col[0] += ((d >> 16) & 255) / 255.0f * a, col[1] += ((d >> 8) & 255) / 255.0f * a, col[2] += (d & 255) / 255.0f * a;
        }
    }
    for (int k = 0; k < 3; k++)
        c[k] /= n, col[k] = ci >= 0 ? col[k] / n : 1;
    auto t = tex_average(g_stage[0].data, g_stage[0].format, g_stage[0].size);
    for (int k = 0; k < 3; k++)
        col[k] *= t[k];
    float lum = col[0] * 0.3f + col[1] * 0.59f + col[2] * 0.11f;
    if (lum < 0.005f)
        return;
    float mx = col[0] > col[1] ? col[0] : col[1];       // hue only (brightness goes into the weight)
    mx = mx > col[2] ? mx : col[2];
    for (int k = 0; k < 3; k++)
        col[k] /= mx;
    float A[16], M[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            A[j * 4 + i] = vsc[(80 + 96 + i) * 4 + j];
    if (!d3d_object_to_view(A, M))
        return;
    float vp[4];                                        // homogeneous: screen-space draws feed clip coordinates
    for (int r = 0; r < 4; r++)
        vp[r] = M[r * 4] * c[0] + M[r * 4 + 1] * c[1] + M[r * 4 + 2] * c[2] + M[r * 4 + 3];
    if (fabsf(vp[3]) < 1e-6f)
        return;
    for (int r = 0; r < 3; r++)
        vp[r] /= vp[3];
    if (vp[2] > -0.5f)                                  // behind / at the camera (engine view: -z forward)
        return;
    float w = lum * sqrtf((float)n / 4);
    for (FxLight &l : g_fx_frame) {                     // merge with a light close by
        float dx = l.pos[0] - vp[0], dy = l.pos[1] - vp[1], dz = l.pos[2] - vp[2];
        if (dx * dx + dy * dy + dz * dz < 2.25f) {
            float tw = l.w + w;
            for (int k = 0; k < 3; k++)
                l.pos[k] = (l.pos[k] * l.w + vp[k] * w) / tw, l.col[k] = (l.col[k] * l.w + col[k] * w) / tw;
            l.w = tw;
            return;
        }
    }
    if (g_fx_frame.size() < 64)
        g_fx_frame.push_back({{vp[0], vp[1], vp[2]}, {col[0], col[1], col[2]}, w});
}

// fx params slot (x already holds the bump height): y = light count, z = intensity; then the lights
static void fx_lights_put(float *p)
{
    fx_roll();
    p[1] = (float)g_fx_count;
    p[2] = g_d3d.fx_lights;
    for (int k = 0; k < g_fx_count; k++) {
        const FxLight &l = g_fx_active[k];
        float s = l.w > 4 ? 1.0f : 0.4f + l.w * 0.15f;   // brighter / bigger effects reach further
        p[4 + k * 8 + 0] = l.pos[0], p[4 + k * 8 + 1] = l.pos[1], p[4 + k * 8 + 2] = l.pos[2];
        p[4 + k * 8 + 3] = 1.5f + 1.5f * s;              // radius (game units; a gladiator is about 2)
        p[4 + k * 8 + 4] = l.col[0] * s, p[4 + k * 8 + 5] = l.col[1] * s, p[4 + k * 8 + 6] = l.col[2] * s;
    }
}

// ------------------------------------------------------------------------------------------------ draw
// verts: per stream raw data pointers/strides; indices: element order to draw (already primitive-expanded)
static void draw_program(const BYTE *const streams[16], const UINT strides[16], UINT vertex_count,
                         const std::vector<UINT> &indices, D3D11_PRIMITIVE_TOPOLOGY topo, bool cached = false,
                         ID3D11Buffer *static_ib = nullptr)
{
    if (!d3d_ready() || !g_cur_vaf || g_cur_vs == 0xFFFFFFFF || indices.empty())
        return;
    ProfScope pz_program(PZ_PROGRAM);
    init_program_objects();
    const VafSlot *slots = vaf_slots(g_cur_vaf);
    LayoutKey lk = {};
    lk.vs.id = g_cur_vs;
    for (int s = 0; s < 16; s++) {
        lk.vs.formats[s] = slots[s].format & 0xFF;
        if (!attr_bytes(lk.vs.formats[s]) || !streams[slots[s].stream])
            lk.vs.formats[s] = 0x02;                    // not streamed: constant attribute
        lk.stream[s] = slots[s].stream;
        lk.offset[s] = slots[s].offset;
    }
    VsEntry *vs;
    ID3D11PixelShader *ps;
    bool relight = false;
    ID3D11InputLayout *layout = nullptr;
    // same programs + layout as the previous draw (runs of scenery): reuse its lookups
    static LayoutKey last_lk;
    static DWORD last_ps = 0xFFFFFFFE;
    static float last_flags[2];
    static VsEntry *last_vs;
    static ID3D11PixelShader *last_psh;
    static ID3D11InputLayout *last_layout;
    static bool last_relight;
    float flags[2] = {(float)g_d3d.pixel_lighting, g_d3d.sun_relight};
    if (last_ps == g_cur_ps && !memcmp(flags, last_flags, sizeof flags) && !memcmp(&lk, &last_lk, sizeof lk)) {
        vs = last_vs, ps = last_psh, layout = last_layout, relight = last_relight;
    } else {
        ProfScope pz(PZ_LOOKUP);
        vs = get_vs(lk.vs);
        ps = g_cur_ps != 0xFFFFFFFF ? get_ps(g_cur_ps) : nullptr;
        const VsLighting &vl = vs_lighting(g_cur_vs);
        int vn = vl.relight ? atoi(vl.vnormal.c_str() + 1) : -1;
        relight = ps && g_d3d.pixel_lighting && g_d3d.sun_relight > 0 && vn >= 0 && vn < 16 &&
                  lk.vs.formats[vn] != 0x02;            // baked surface with a normal stream: sun term
        if (relight)
            if (ID3D11PixelShader *pl = get_ps_lit(g_cur_ps, g_cur_vs))
                ps = pl;
        if (ps && g_d3d.pixel_lighting && vl.ok)        // [video] Lighting=pixel (F8)
            if (ID3D11PixelShader *pl = get_ps_lit(g_cur_ps, g_cur_vs))
                ps = pl;
        if (vs && ps)
            layout = get_layout(lk, vs);
        last_lk = lk, last_ps = g_cur_ps, memcpy(last_flags, flags, sizeof flags);
        last_vs = vs, last_psh = ps, last_layout = layout, last_relight = relight;
    }
    if (!vs || !ps || !layout)
        return;
    ID3D11DeviceContext *ctx = g_d3d.ctx;

    // vertex streams
    ID3D11Buffer *bufs[16] = {};
    UINT strd[16] = {}, offs[16] = {};
    bool used[16] = {};                                 // streams other draws left bound may be stale/freed
    for (int a = 0; a < 16; a++)
        if (lk.vs.formats[a] != 0x02)
            used[slots[a].stream & 15] = true;
    UINT transient = 0;                                 // transient (UP / inline) bytes, all in one ring Map
    for (int s = 0; s < 16; s++) {
        if (!used[s] || !streams[s] || !strides[s])
            continue;
        UINT bytes = strides[s] * vertex_count;
        if (cached)                                     // vertex buffer memory: GPU copy cached by address
            bufs[s] = cached_stream(streams[s], bytes);
        else
            transient += (bytes + 15) & ~15u;
        strd[s] = strides[s];
    }
    ProfScope pz_trans(PZ_TRANSIENT);
    prof_count(PC_TRANSIENT, transient + (UINT)indices.size() * 4);
    if (transient) {
        UINT base;
        BYTE *dst = ring_map(g_vring, transient, &base);
        if (!dst)
            return;
        for (int s = 0, at = 0; s < 16; s++) {
            if (!used[s] || !streams[s] || !strides[s])
                continue;
            UINT bytes = strides[s] * vertex_count;
            memcpy(dst + at, streams[s], bytes);
            bufs[s] = g_vring.buf;
            offs[s] = base + at;
            at += (bytes + 15) & ~15u;
        }
        ctx->Unmap(g_vring.buf, 0);
    }
    UINT ioff = 0;
    if (!static_ib) {                                   // transient index list -> the ring
        BYTE *idst = ring_map(g_iring, (UINT)indices.size() * 4, &ioff);
        if (!idst)
            return;
        memcpy(idst, indices.data(), indices.size() * 4);
        ctx->Unmap(g_iring.buf, 0);
    }
    pz_trans.stop();
    ProfScope pz_const(PZ_CONST);

    // constants
    static float vsc[(192 + 16 + 2) * 4], vsc_bound[(192 + 16 + 2) * 4];
    static bool vsc_valid;
    static DWORD vsc_gen;
    // the 3.4 KB block is copied (and compared) only when the game wrote constants since the last draw; per draw only
    // the port's own rows (c92 camera, 208 screen / jitter, 209 material) can differ
    bool vsc_fresh = vsc_gen != g_vsc_gen;
    if (vsc_fresh) {
        vsc_gen = g_vsc_gen;
        memcpy(vsc, g_vsc, sizeof g_vsc);
        memcpy(vsc + 192 * 4, g_attr_const, sizeof g_attr_const);
    } else {
        memcpy(&vsc[(92 + 96) * 4], g_vsc[92 + 96], 16);  // c92: a lit draw may have replaced it
    }
    // shader screen-space extent: the Xbox screen space the engine's programs write positions in (back buffer x the
    // game's own supersample factors), NOT the scene target - the viewport maps that space onto a bigger target.
    // Using the scene size (render scale > 1) squeezed every program draw into the top-left (2026-10-08).
    vsc[208 * 4 + 0] = (float)(g_d3d.width * g_d3d.game_ss_x);
    vsc[208 * 4 + 1] = (float)(g_d3d.height * g_d3d.game_ss_y);
    d3d_taa_jitter(&vsc[208 * 4 + 2]);                  // TAA: this frame's sub-pixel offset (d3d_taa.cpp)
    TexInfo *ti = g_stage[0].data ? (TexInfo *)&tex_info(g_stage[0].data, g_stage[0].format) : nullptr;
    int mat = g_d3d.materials > 0 && ti ? ti->material : 0;
    DWORD pf0 = g_cur_vaf ? vaf_slots(g_cur_vaf)[0].format : 0;
    if (mat >= 4 && ((pf0 & 15) != 2 || ((pf0 >> 4) & 15) < 3))
        mat = 0;                                        // sway needs a float position in v0
    static float frame_sec;                             // one time value per frame (constant dedupe)
    static DWORD sec_frame = 0xFFFFFFFF;
    if (sec_frame != g_d3d.frame)
        sec_frame = g_d3d.frame, frame_sec = (float)(GetTickCount64() % 600000) / 1000.0f;
    float msec = mat ? frame_sec : 0;
    vsc[209 * 4 + 0] = (float)mat, vsc[209 * 4 + 1] = msec;
    vsc[209 * 4 + 2] = mat == 4 ? 0.03f * g_d3d.materials : mat == 5 ? 0.05f * g_d3d.materials : 0;
    // per-pixel lighting: the camera in this draw's own vertex space -> c92 (VS_CAMERA_POSITIONDIRECTION_LOCAL, unused
    // by the game). The camera is where clip w is 0 with x = y = 0: C = WVP^-1 * (0, 0, 1, 0), whatever the space.
    ID3D11ShaderResourceView *shadow_srv = nullptr;    // lit draws: last frame's shadow map at t5 / s5
    ID3D11SamplerState *shadow_cmp = nullptr;
    float shadow_rows[16], shadow_texel = 0, shadow_strength = 0;
    bool shadow_px = false;
    if (g_d3d.pixel_lighting && vs_lighting(g_cur_vs).ok) {
        ProfScope pz(PZ_LIGHT);
        float A[16], Ai[16];
        for (int i = 0; i < 4; i++)                     // WVP constants are rows of the row-vector matrix
            for (int j = 0; j < 4; j++)
                A[j * 4 + i] = vsc[(80 + 96 + i) * 4 + j];
        if (d3d_invert4(A, Ai) && fabsf(Ai[14]) > 1e-9f) {
            float *c92 = &vsc[(92 + 96) * 4];
            c92[0] = Ai[2] / Ai[14], c92[1] = Ai[6] / Ai[14], c92[2] = Ai[10] / Ai[14], c92[3] = 1;
        }
        shadow_px = d3d_shadow_lookup(A, shadow_rows, &shadow_srv, &shadow_cmp, &shadow_texel, &shadow_strength);
    }
    if (vsc_fresh || !vsc_valid || memcmp(&vsc[(92 + 96) * 4], &vsc_bound[(92 + 96) * 4], 16) ||
        memcmp(&vsc[208 * 4], &vsc_bound[208 * 4], 2 * 16))
        cb_put(g_cb_vs, vsc_bound, &vsc_valid, vsc, sizeof vsc);
    float psc[36 * 4] = {};
    static float psc_bound[36 * 4];
    static bool psc_valid;
    memcpy(psc, g_psc, sizeof g_psc);
    DWORD fc = d3d_nv(0x2A8);                          // NV097_SET_FOG_COLOR
    psc[8 * 4 + 0] = ((fc) & 255) / 255.f, psc[8 * 4 + 1] = ((fc >> 8) & 255) / 255.f;
    psc[8 * 4 + 2] = ((fc >> 16) & 255) / 255.f;
    psc[9 * 4 + 0] = d3d_nv(0x300) ? 1.0f : 0.0f;      // alpha test enable / ref / func
    psc[9 * 4 + 1] = (d3d_nv(0x340) & 0xFF) / 255.0f;
    psc[9 * 4 + 2] = (float)(d3d_nv(0x33C) & 7);
    const DWORD *rs = (const DWORD *)0x2C53D0;          // D3D_g_RenderState: deferred fog states 0x5C..0x60
    if (rs[0x5C]) {                                     // FOGENABLE: mode (0 vertex, 1 exp, 2 exp2, 3 linear) + 1
        psc[14 * 4 + 0] = (float)(rs[0x5D] + 1);
        memcpy(&psc[14 * 4 + 1], &rs[0x5E], 4);         // FOGSTART
        memcpy(&psc[14 * 4 + 2], &rs[0x5F], 4);         // FOGEND
        memcpy(&psc[14 * 4 + 3], &rs[0x60], 4);         // FOGDENSITY
    }
    // soft particles: blended, depth-tested, not depth-writing 3D draws fade against the G-buffer depth;
    // x = on, y = 1 / range, z = 1: fade alpha (alpha blending) / 2: fade colour too (additive)
    bool soft = g_d3d.soft_particles && g_d3d.in_fx && g_d3d.gbuf_srv && d3d_nv(0x304) && d3d_nv(0x30C) && !d3d_nv(0x35C);
    if (soft) {
        DWORD sf = d3d_nv(0x344);
        psc[15 * 4 + 0] = 1;
        psc[15 * 4 + 1] = 1.0f / g_d3d.soft_range;
        psc[15 * 4 + 2] = sf == 0x302 || sf == 0x308 ? 1.0f : 2.0f;
        static unsigned soft_draws;
        if (soft_draws++ % 5000 == 0)
            port_log("soft particles: draw %u (frame %u, blend %X/%X)\n", soft_draws, g_d3d.frame, sf, d3d_nv(0x348));
    }
    psc[35 * 4 + 0] = (float)mat, psc[35 * 4 + 1] = msec, psc[35 * 4 + 2] = mat ? g_d3d.materials : 0;
    psc[16 * 4 + 0] = g_d3d.specular;                   // per-pixel extras (only read by the lit variants)
    psc[16 * 4 + 1] = g_d3d.spec_power > 1 ? g_d3d.spec_power : 1;
    psc[16 * 4 + 2] = g_d3d.rim;
    psc[16 * 4 + 3] = g_d3d.specular > 0 || g_d3d.rim > 0 ? 1.0f : 0.0f;
    ProfScope pz_light(PZ_LIGHT);
    if (relight) {                                      // sun in this mesh's own space
        float A[16], d[3];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                A[j * 4 + i] = vsc[(80 + 96 + i) * 4 + j];
        if (d3d_sun_in_object(A, d))
            psc[22 * 4 + 0] = d[0], psc[22 * 4 + 1] = d[1], psc[22 * 4 + 2] = d[2], psc[22 * 4 + 3] = g_d3d.sun_relight;
    }
    if (relight || g_d3d.pixel_lighting && vs_lighting(g_cur_vs).ok) {   // normals -> view space for the G-buffer
        float A[16], M[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                A[j * 4 + i] = vsc[(80 + 96 + i) * 4 + j];
        if (d3d_object_to_view(A, M))
            for (int r = 0; r < 3; r++)
                memcpy(&psc[(23 + r) * 4], &M[r * 4], 16);      // rotation + translation
        psc[26 * 4 + 0] = g_d3d.bump;
        fx_lights_put(&psc[26 * 4]);                    // count, intensity + the lights
    }
    pz_light.stop();
    if (shadow_px) {
        memcpy(&psc[17 * 4], shadow_rows, sizeof shadow_rows);
        psc[21 * 4 + 0] = 1, psc[21 * 4 + 1] = shadow_strength, psc[21 * 4 + 2] = shadow_texel;
    }
    pz_const.stop();                                    // texture lookups have their own zone
    ID3D11ShaderResourceView *srv[4] = {};
    for (int t = 0; t < 4; t++) {
        psc[(10 + t) * 4 + 0] = psc[(10 + t) * 4 + 1] = 1;
        Stage &st = g_stage[t];
        if (!st.data)
            continue;
        srv[t] = d3d_texture_srv(st.data, st.format, st.size);
        if (st.size) {                                  // linear textures: texel coordinates
            psc[(10 + t) * 4 + 0] = 1.0f / ((st.size & 0xFFF) + 1);
            psc[(10 + t) * 4 + 1] = 1.0f / (((st.size >> 12) & 0xFFF) + 1);
        }
    }
    {
        ProfScope pz(PZ_CONST);
        cb_put(g_cb_ps, psc_bound, &psc_valid, psc, sizeof psc);
    }

    if (d3d_tracing())
        d3d_trace("  draw vs %u ps %u verts %u idx %u | blend %u %X/%X atest %u func %X ref %X | zt %u zw %u cull %u | "
                  "t0 %08X fmt %08X t1 %08X fmt %08X",
                  g_cur_vs, g_cur_ps, vertex_count, (UINT)indices.size(), d3d_nv(0x304), d3d_nv(0x344), d3d_nv(0x348),
                  d3d_nv(0x300), d3d_nv(0x33C), d3d_nv(0x340), d3d_nv(0x30C), d3d_nv(0x35C), d3d_nv(0x308),
                  g_stage[0].data, g_stage[0].format, g_stage[1].data, g_stage[1].format);
    if (d3d_tracing()) {
        const DWORD *ts = (const DWORD *)0x2C51D0;     // D3D_g_DeferredTextureState[stage][32]
        d3d_trace("    tss0 %X %X %X | %X %X %X | %X %X %X %X %X %X | %X %X %X %X %X %X %X %X %X %X", ts[0], ts[1], ts[2],
                  ts[3], ts[4], ts[5], ts[6], ts[7], ts[8], ts[9], ts[10], ts[11], ts[12], ts[13], ts[14], ts[15],
                  ts[16], ts[17], ts[18], ts[19], ts[20], ts[21]);
    }
    if (d3d_tracing() && g_cur_vs == 0) {              // [debug] transform inputs (disabled)
        auto row = [](int r) {
            d3d_trace("    c%-3d %g %g %g %g", r, g_vsc[r][0], g_vsc[r][1], g_vsc[r][2], g_vsc[r][3]);
        };
        for (int r : {0, 1, 2, 176, 177, 178, 179, 185, 190, 191})
            row(r);
        UINT i0 = indices[0];
        for (int s = 0; s < 16; s++) {
            DWORD f = lk.vs.formats[s];
            if (f == 0x02)
                continue;
            const BYTE *p = streams[slots[s].stream] + strides[slots[s].stream] * i0 + slots[s].offset;
            d3d_trace("    vtx %u v%d fmt %02X stride %u: %08X %08X %08X", i0, s, f, strides[slots[s].stream],
                      ((const DWORD *)p)[0], ((const DWORD *)p)[1], ((const DWORD *)p)[2]);
        }
    }
    static DWORD skip[8] = {1};                         // [debug] SkipTex=addr,...: leave out draws using them
    if (skip[0] == 1) {
        skip[0] = 0;
        char ini[MAX_PATH], val[128];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        GetPrivateProfileStringA("debug", "SkipTex", "", val, sizeof val, ini);
        int n = 0;
        for (char *t = strtok(val, ", "); t && n < 8; t = strtok(nullptr, ", "))
            skip[n++] = strtoul(t, nullptr, 16);
    }
    for (int k = 0; k < 8 && skip[k]; k++)
        if (g_stage[0].data == skip[k])
            return;
    // real sun shadows replace the engine's blob shadows; sky / sun never cast
    if (ti && ti->blob_shadow && d3d_shadows_on())
        return;
    d3d_begin_draw(false, soft);
    ID3D11SamplerState *samplers[4];
    {
        ProfScope pz(PZ_STATE);
        for (int t = 0; t < 4; t++)
            samplers[t] = stage_sampler(t);
    }
    ProfScope pz_submit(PZ_SUBMIT);
    prof_gpu_segment(d3d_nv(0x304) != 0);
    ctx->IASetInputLayout(layout);
    ctx->IASetVertexBuffers(0, 16, bufs, strd, offs);
    ctx->IASetIndexBuffer(static_ib ? static_ib : g_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(topo);
    ctx->VSSetShader(vs->vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb_vs);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer *pcb[2] = {g_cb_vs, g_cb_ps};          // b0: vertex constants (per-pixel lighting)
    ctx->PSSetConstantBuffers(0, 2, pcb);
    ctx->PSSetShaderResources(0, 4, srv);
    ctx->PSSetSamplers(0, 4, samplers);
    if (shadow_px) {
        ctx->PSSetShaderResources(5, 1, &shadow_srv);
        ctx->PSSetSamplers(5, 1, &shadow_cmp);
    }
    ctx->DrawIndexed((UINT)indices.size(), ioff / 4, 0);
    g_d3d.draws++;
    if (g_d3d.in_fx && g_d3d.fx_lights > 0 && d3d_nv(0x304) && d3d_nv(0x348) == 1)   // additive effect: a light
        fx_light_collect(streams, strides, vertex_count, slots, lk, vsc);
    pz_submit.stop();
    if (d3d_shadows_on()) {
        ProfScope pz(PZ_SHREC);
        const VsLighting &lit = vs_lighting(g_cur_vs);
        if (lit.ok)
            d3d_shadow_light(vsc, lit.first, lit.nlights, lit.wv);
        bool opaque = !d3d_nv(0x304) && d3d_nv(0x35C) && d3d_nv(0x30C);
        if (cached && opaque && !(ti && ti->no_cast)) {
            if (ti && !ti->logged) {                    // [diagnostic] caster textures, first time each
                ti->logged = true;
                port_log("shadow caster: '%s' vs #%u, %u indices, view z %.2f\n", ti->name, g_cur_vs,
                         (UINT)indices.size(), vsc[(76 + 96 + 2) * 4 + 3]);
            }
            d3d_shadow_record(vs->vs, layout, bufs, strd, offs, topo, indices, vsc,
                              psc[9 * 4 + 0] > 0 ? srv[0] : nullptr, psc[9 * 4 + 1], ti ? ti->name : "", lit.ok,
                              static_ib, vsc_gen);
        }
    }
}

static void draw_immediate()
{
    if (!g_cur_vaf || g_inline.empty())
        return;
    const VafSlot *slots = vaf_slots(g_cur_vaf);
    // inline array layout: enabled attributes packed in slot order (dword aligned)
    static BYTE fake_vaf[256];
    memcpy(fake_vaf, g_cur_vaf, 256);
    VafSlot *fs = (VafSlot *)fake_vaf;
    UINT stride = 0;
    for (int s = 0; s < 16; s++) {
        UINT b = attr_bytes(slots[s].format & 0xFF);
        fs[s].stream = 0;
        fs[s].offset = stride;
        stride += b;
    }
    if (!stride)
        return;
    UINT count = (UINT)(g_inline.size() * 4 / stride);
    static std::vector<UINT> idx;                       // reused (D3D entry points run under the D3D lock)
    D3D11_PRIMITIVE_TOPOLOGY topo;
    prof_count(PC_DRAW_INLINE);
    {
        ProfScope pz(PZ_INDEX);
        d3d_expand_primitive(g_begin_prim, count, idx, &topo);
        prof_count(PC_INDICES, idx.size());
    }
    const BYTE *streams[16] = {(const BYTE *)g_inline.data()};
    UINT strides[16] = {stride};
    const BYTE *saved = g_cur_vaf;
    g_cur_vaf = fake_vaf;
    draw_program(streams, strides, count, idx, topo);
    g_cur_vaf = saved;
}

// push-buffer methods relevant to program draws (called for every decoded method)
void d3d_program_method(DWORD method, const DWORD *data, DWORD count)
{
    if (method == 0x17FC) {                            // SET_BEGIN_END
        if (data[0]) {
            g_in_begin = true;
            g_begin_prim = data[0];
            g_inline.clear();
        } else if (g_in_begin) {
            g_in_begin = false;
            draw_immediate();
        }
    } else if (method == 0x1818) {                     // INLINE_ARRAY
        g_inline.insert(g_inline.end(), data, data + count);
    } else if (method == 0x1EA4) {                     // SET_TRANSFORM_CONSTANT_LOAD
        g_const_load = data[0];
    } else if (method >= 0x0B80 && method < 0x0C00) {  // SET_TRANSFORM_CONSTANT window
        DWORD first = (method - 0x0B80) / 4;
        for (DWORD k = 0; k < count; k++) {
            DWORD pos = first + k, slot = g_const_load + pos / 4;
            if (slot < 192)
                memcpy(&g_vsc[slot][pos % 4], &data[k], 4);
            g_vsc_gen++;
            if ((slot == 189 || slot == 191) && pos % 4 == 3) {   // 2D screen mapping c93 / c95, via push
                static int n;
                if (n++ < 40)
                    port_log("2d map (push): c%u = %g %g %g %g\n", slot - 96, g_vsc[slot][0], g_vsc[slot][1],
                             g_vsc[slot][2], g_vsc[slot][3]);
            }
        }
        g_const_load += (first + count) / 4;
    } else if (method >= 0x1940 && method < 0x1980) {  // SET_VERTEX_DATA4UB(slot)
        attr_defaults();
        DWORD slot = (method - 0x1940) / 4, v = data[0];
        for (int c = 0; c < 4; c++)
            g_attr_const[slot][c] = ((v >> (8 * c)) & 255) / 255.0f;
        g_vsc_gen++;
    } else if (method >= 0x1A00 && method < 0x1B00) {  // SET_VERTEX_DATA4F(slot)
        attr_defaults();
        DWORD slot = (method - 0x1A00) / 16, comp = (method - 0x1A00) / 4 % 4;
        for (DWORD k = 0; k < count && comp + k < 4; k++)
            memcpy(&g_attr_const[slot][comp + k], &data[k], 4);
        g_vsc_gen++;
    } else if (method >= 0x1880 && method < 0x1900) {  // SET_VERTEX_DATA2F(slot)
        attr_defaults();
        DWORD slot = (method - 0x1880) / 8;
        memcpy(&g_attr_const[slot][0], data, 4 * (count < 2 ? count : 2));
        g_attr_const[slot][2] = 0, g_attr_const[slot][3] = 1;
        g_vsc_gen++;
    } else if (method >= 0x1B00 && method < 0x1C00) {  // texture stage methods
        int stage = (method - 0x1B00) / 0x40;
        DWORD reg = (method - 0x1B00) % 0x40;
        for (DWORD k = 0; k < count; k++, reg += 4) {
            if (reg == 0x00) g_stage[stage].data = data[k];
            else if (reg == 0x04) g_stage[stage].format = data[k];
            else if (reg == 0x1C) {                     // IMAGE_RECT: width << 16 | height (linear)
                DWORD w = data[k] >> 16, h = data[k] & 0xFFFF;
                g_stage[stage].size = w && h ? (w - 1) | ((h - 1) << 12) | (((w * 4 + 63) / 64 - 1) << 24) : 0;
            }
        }
    }
}

// Indexed draws: the game's 16-bit index lists (strips / fans / lists) expanded to 32-bit triangle lists once and
// kept on the GPU, keyed by (address, count, primitive) and checked by a hash of the game's indices each draw.
// Expanding and re-uploading every list every draw was ~1.3 ms + ~1.7 MB a frame on the world map (2026-10-10),
// and the shadow pass copied them all again; it now draws from these buffers too.
struct IbEntry {
    ID3D11Buffer *ib;
    std::vector<UINT> idx;                              // the expanded list (CPU copy: shadow caster identity)
    DWORD prim, hash, frame;
    UINT count, maxv;
    D3D11_PRIMITIVE_TOPOLOGY topo;
};
static std::unordered_map<unsigned long long, IbEntry> g_ib_cache;

static DWORD index_hash(const WORD *p, UINT count)
{
    DWORD h = 0x811C9DC5 ^ count;
    const DWORD *d = (const DWORD *)p;
    for (UINT i = 0, n = count / 2; i < n; i++)
        h = (h ^ d[i]) * 0x01000193;
    if (count & 1)
        h = (h ^ p[count - 1]) * 0x01000193;
    return h;
}

static IbEntry *index_list(DWORD prim, UINT count, const WORD *indices)
{
    IbEntry &e = g_ib_cache[(unsigned long long)(uintptr_t)indices << 32 | count];
    DWORD h = index_hash(indices, count);
    if (!e.ib || e.hash != h || e.prim != prim || e.count != count) {
        if (e.ib)
            e.ib->Release();
        e.ib = nullptr;
        d3d_expand_primitive(prim, count, e.idx, &e.topo);
        e.maxv = 0;
        for (auto &o : e.idx) {
            o = indices[o];
            e.maxv = o > e.maxv ? o : e.maxv;
        }
        e.prim = prim, e.count = count, e.hash = h;
        prof_count(PC_INDICES, e.idx.size());
        if (e.idx.empty())
            return nullptr;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)e.idx.size() * 4;
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {e.idx.data(), 0, 0};
        if (FAILED(g_d3d.dev->CreateBuffer(&bd, &sd, &e.ib)))
            return nullptr;
    }
    e.frame = g_d3d.frame;
    static DWORD sweep;                                 // drop lists unused for ~10 s (streamed / freed meshes)
    if (g_d3d.frame - sweep >= 600) {
        sweep = g_d3d.frame;
        for (auto it = g_ib_cache.begin(); it != g_ib_cache.end();)
            if (&it->second != &e && g_d3d.frame - it->second.frame > 600) {
                if (it->second.ib)
                    it->second.ib->Release();
                it = g_ib_cache.erase(it);
            } else
                ++it;
    }
    return e.ib ? &e : nullptr;
}

// ------------------------------------------------------------------------------------------------ entry points
extern "C" {

void __fastcall d3d_D3DDevice_SetVertexShaderConstant1Fast(DWORD reg, const float *data)
{
    D3DLock lock;
    d3d_drain();
    if (reg < 192)
        memcpy(g_vsc[reg], data, 16);
    g_vsc_gen++;
    if (reg == 189 || reg == 191) {                    // 2D screen mapping c93 / c95 (screenspace UI shader)
        static int n;
        if (n++ < 40)
            port_log("2d map: c%u = %g %g %g %g from %p\n", reg - 96, data[0], data[1], data[2], data[3],
                     _ReturnAddress());
    }
}

// reg = hardware slot (D3D register + 96), dwords = 4 * constant count (the original streams that many floats)
void __fastcall d3d_D3DDevice_SetVertexShaderConstantNotInlineFast(DWORD reg, const float *data, DWORD dwords)
{
    D3DLock lock;
    d3d_drain();
    for (DWORD i = 0; i < dwords / 4 && reg + i < 192; i++) {
        memcpy(g_vsc[reg + i], data + 4 * i, 16);
        g_vsc_gen++;
        if (reg + i == 189 || reg + i == 191) {        // 2D screen mapping c93 / c95 (screenspace UI shader)
            static int n;
            const float *v = data + 4 * i;
            if (n++ < 40)
                port_log("2d map: c%u = %g %g %g %g from %p (block of %u)\n", reg + i - 96, v[0], v[1], v[2], v[3],
                         _ReturnAddress(), dwords / 4);
        }
    }
}

void __stdcall d3d_D3DDevice_SetPixelShaderConstant(DWORD reg, const float *data, DWORD count)
{
    D3DLock lock;
    d3d_drain();
    for (DWORD i = 0; i < count && reg + i < 8; i++)
        memcpy(g_psc[reg + i], data + 4 * i, 16);
}

void __fastcall d3d_D3DDevice_SwitchTexture(DWORD method, DWORD data, DWORD format)
{
    D3DLock lock;
    d3d_drain();
    int stage = ((method & 0x1FFC) - 0x1B00) / 0x40;
    if (stage >= 0 && stage < 4) {
        g_stage[stage].data = data;
        g_stage[stage].format = format;
        g_stage[stage].size = 0;
    }
    d3d_trace("SwitchTexture(stage %d, %08X, %08X)", stage, data, format);
}

void __stdcall d3d_D3DDevice_SetStreamSource(DWORD stream, XResource *vb, UINT stride)
{
    D3DLock lock;
    d3d_drain();
    if (stream < 16)
        g_streams[stream] = {vb, stride};
}

void __stdcall d3d_D3DDevice_DrawIndexedVertices(DWORD prim, UINT count, const WORD *indices)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("DrawIndexedVertices(prim %u, %u indices, vs %d ps %d) from %p", prim, count, (int)g_cur_vs,
              (int)g_cur_ps, _ReturnAddress());
    if (!indices || !count)
        return;
    prof_count(PC_DRAW_INDEXED);
    IbEntry *ie;
    {
        ProfScope pz(PZ_INDEX);
        ie = index_list(prim, count, indices);
    }
    if (!ie)
        return;
    const BYTE *streams[16] = {};
    UINT strides[16] = {};
    for (int s = 0; s < 16; s++)
        if (g_streams[s].vb) {
            streams[s] = (const BYTE *)g_streams[s].vb->Data;
            strides[s] = g_streams[s].stride;
        }
    draw_program(streams, strides, ie->maxv + 1, ie->idx, ie->topo, true, ie->ib);
}

}

// UP draw while an engine program is selected (stream 0 = caller memory)
bool d3d_program_draw_up(DWORD prim, UINT count, const BYTE *data, UINT stride)
{
    if (g_cur_vs == 0xFFFFFFFF || !g_cur_vaf)
        return false;
    static std::vector<UINT> idx;                       // reused (D3D entry points run under the D3D lock)
    D3D11_PRIMITIVE_TOPOLOGY topo;
    prof_count(PC_DRAW_UP);
    {
        ProfScope pz(PZ_INDEX);
        d3d_expand_primitive(prim, count, idx, &topo);
        prof_count(PC_INDICES, idx.size());
    }
    const BYTE *streams[16] = {data};
    UINT strides[16] = {stride};
    draw_program(streams, strides, count, idx, topo);
    return true;
}
