// Xbox D3D8 -> D3D11: render state, textures and fixed-function (FVF) draws. Simple render states and the engine's
// push buffers both speak NV2A methods; everything lands in one NV2A state table (g_nv), from which cached D3D11
// blend / depth / rasterizer objects are built at draw time.
#include "d3d.h"
#include "d3d_prof.h"
#include <d3dcompiler.h>
#include <map>
#include <set>
#include <vector>
#include <unordered_map>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

extern DWORD g_cur_vs;                                 // d3d_shader.cpp (0xFFFFFFFF = fixed-function / FVF mode)
bool d3d_program_draw_up(DWORD prim, UINT count, const BYTE *data, UINT stride);   // d3d_program.cpp

// ------------------------------------------------------------------------------------------------ NV2A state
enum {
    NV_ALPHA_TEST_ENABLE = 0x300, NV_BLEND_ENABLE = 0x304, NV_CULL_FACE_ENABLE = 0x308, NV_DEPTH_TEST_ENABLE = 0x30C,
    NV_DITHER_ENABLE = 0x310, NV_LIGHTING_ENABLE = 0x314, NV_STENCIL_TEST_ENABLE = 0x32C,
    NV_POLY_OFFSET_FILL_ENABLE = 0x338, NV_ALPHA_FUNC = 0x33C, NV_ALPHA_REF = 0x340, NV_BLEND_SFACTOR = 0x344,
    NV_BLEND_DFACTOR = 0x348, NV_BLEND_COLOR = 0x34C, NV_BLEND_EQUATION = 0x350, NV_DEPTH_FUNC = 0x354,
    NV_COLOR_MASK = 0x358, NV_DEPTH_MASK = 0x35C, NV_SHADE_MODE = 0x37C, NV_POLY_OFFSET_SCALE = 0x384,
    NV_POLY_OFFSET_BIAS = 0x388, NV_FRONT_POLYGON_MODE = 0x38C, NV_CULL_FACE = 0x39C, NV_FRONT_FACE = 0x3A0,
};
static DWORD g_nv[0x2000 / 4];                         // last value written per method (state methods only)

static void nv_defaults()
{
    g_nv[NV_DEPTH_TEST_ENABLE / 4] = 1;
    g_nv[NV_DEPTH_FUNC / 4] = 0x203;                   // GL_LEQUAL
    g_nv[NV_DEPTH_MASK / 4] = 1;
    g_nv[NV_BLEND_SFACTOR / 4] = 1;                    // GL_ONE
    g_nv[NV_BLEND_DFACTOR / 4] = 0;                    // GL_ZERO
    g_nv[NV_BLEND_EQUATION / 4] = 0x8006;              // GL_FUNC_ADD
    g_nv[NV_ALPHA_FUNC / 4] = 0x207;                   // GL_ALWAYS
    g_nv[NV_COLOR_MASK / 4] = 0x01010101;
    g_nv[NV_CULL_FACE / 4] = 0x405;                    // GL_BACK
    g_nv[NV_FRONT_FACE / 4] = 0x901;                   // GL_CCW
}
static int g_nv_defaults_applied = (nv_defaults(), 1);  // at load: before the game's first state writes

void d3d_program_method(DWORD method, const DWORD *data, DWORD count);

void d3d_nv_method(DWORD method, const DWORD *data, DWORD count)
{
    d3d_program_method(method, data, count);
    // only single-value state methods are tracked here; arrays (constants, inline vertices) are handled by the
    // push decoder itself
    if (method < 0x2000 && count >= 1)
        g_nv[method / 4] = data[0];
}

// non-NV2A shim state
static bool g_yuv_enable;
static DWORD g_fvf;                                    // current FVF (vertex shader handle with bit 0 clear)
static XPixelContainer *g_textures[4];

// ------------------------------------------------------------------------------------------------ D3D11 objects
static ID3D11DepthStencilView *g_dsv;
static ID3D11VertexShader *g_vs_rhw;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_layout;
static ID3D11Buffer *g_vb, *g_cb_vs, *g_cb_ps;
static ID3D11SamplerState *g_sampler;
static const UINT VB_BYTES = 4 << 20;
static UINT g_vb_pos;

struct CanonVertex {                                   // every FVF vertex is expanded to this on the CPU
    float pos[4];
    DWORD color;
    float uv0[2], uv1[2];
};

static const char *k_hlsl = R"(
cbuffer VS : register(b0) { float4 screen; };            // x = 2/w, y = -2/h (game back-buffer pixels -> NDC)
cbuffer PS : register(b1) { float4 flags; float4 alpha; float4 uvscale; }; // flags.x textured; alpha.x test .y ref .z func
                                                         // uvscale.xy: 1/size for linear textures (texel coordinates)
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct VSIn { float4 pos : POSITION; float4 col : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct PSIn { float4 pos : SV_Position; float4 col : COLOR0; float2 uv0 : TEXCOORD0; };
PSIn vs_rhw(VSIn i)
{
    PSIn o;
    float w = i.pos.w != 0 ? 1.0 / i.pos.w : 1.0;
    o.pos = float4((i.pos.x * screen.x - 1.0) * w, (i.pos.y * screen.y + 1.0) * w, i.pos.z * w, w);
    o.col = i.col;
    o.uv0 = i.uv0;
    return o;
}
bool alpha_pass(float a)
{
    int f = (int)alpha.z;  // GL func - 0x200: 0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always
    float r = alpha.y;
    return f == 7 || (f == 1 && a < r) || (f == 2 && a == r) || (f == 3 && a <= r) || (f == 4 && a > r) ||
           (f == 5 && a != r) || (f == 6 && a >= r);
}
float4 ps(PSIn i) : SV_Target
{
    float4 c = i.col;
    if (flags.x > 0)
        c *= t0.Sample(s0, i.uv0 * uvscale.xy);
    if (alpha.x > 0 && !alpha_pass(c.a))
        discard;
    return c;
}
)";

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(k_hlsl, strlen(k_hlsl), "gladiusport", nullptr, nullptr, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        port_log("HLSL %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

static bool g_ready;

static bool create_depth_target()
{
    if (g_dsv)
        return true;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_d3d.scene_width;
    td.Height = g_d3d.scene_height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *ds = nullptr;
    HRESULT hr = g_d3d.dev->CreateTexture2D(&td, nullptr, &ds);
    if (FAILED(hr) || !ds) {
        port_log("renderer: depth %ux%u create failed %08X\n", td.Width, td.Height, hr);
        return false;
    }
    hr = g_d3d.dev->CreateDepthStencilView(ds, nullptr, &g_dsv);
    ds->Release();
    if (FAILED(hr) || !g_dsv) {
        port_log("renderer: depth view %ux%u create failed %08X\n", td.Width, td.Height, hr);
        return false;
    }
    port_log("renderer: depth %ux%u ready\n", td.Width, td.Height);
    return true;
}

static void render_init()
{
    if (g_ready || !g_d3d.dev)
        return;
    ID3D11Device *dev = g_d3d.dev;
    ID3DBlob *vs = compile("vs_rhw", "vs_4_0"), *ps = compile("ps", "ps_4_0");
    if (!vs || !ps)
        return;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs_rhw);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(il, 4, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout);
    vs->Release();
    ps->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_vb);
    bd.ByteWidth = 16;                                  // constant buffers: DEFAULT (updated with UpdateSubresource)
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_cb_vs);
    bd.ByteWidth = 48;
    dev->CreateBuffer(&bd, nullptr, &g_cb_ps);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = g_d3d.texture_filter == 2 ? D3D11_FILTER_ANISOTROPIC
              : g_d3d.texture_filter == 0 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = g_d3d.texture_filter == 2 ? g_d3d.anisotropy : 1;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_sampler);

    if (!create_depth_target())
        return;
    g_ready = true;
    port_log("renderer: shaders and buffers ready\n");
}

// Bound D3D11 state is tracked so draws only set what changed; d3d_state_invalidate forces a full rebind (after the
// overlay or a Present unbinds things).
static struct {
    bool targets, blend, depth, raster;
    bool soft;                                          // targets bound for a soft-particle draw
    D3D11_BLEND_DESC bd;
    float factor[4];
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_RECT scissor;
} g_bound;

void d3d_state_invalidate()
{
    g_bound.targets = g_bound.blend = g_bound.depth = g_bound.raster = false;
}

void d3d_scene_targets_changed()
{
    if (g_dsv) {
        g_dsv->Release();
        g_dsv = nullptr;
    }
    create_depth_target();
    d3d_state_invalidate();
}

// Scene colour + G-buffer depth (target 1); a soft-particle draw reads the G-buffer instead (texture t4), so it is
// left out of the targets for that draw.
static void bind_targets(bool soft = false)
{
    soft = soft && g_d3d.gbuf_srv;
    if (g_bound.targets && g_bound.soft == soft)
        return;
    g_bound.targets = true;
    g_bound.soft = soft;
    ID3D11RenderTargetView *rt[4] = {g_d3d.rtv, g_d3d.gbuf_rtv, g_d3d.gnrm_rtv, g_d3d.gmv_rtv};
    UINT n = soft || !g_d3d.gbuf_rtv ? 1 : !g_d3d.gnrm_rtv ? 2 : g_d3d.gmv_rtv ? 4 : 3;
    g_d3d.ctx->OMSetRenderTargets(n, rt, g_dsv);
    if (soft)
        g_d3d.ctx->PSSetShaderResources(4, 1, &g_d3d.gbuf_srv);
    D3D11_VIEWPORT vp = {0, 0, (float)g_d3d.scene_width, (float)g_d3d.scene_height, 0, 1};
    g_d3d.ctx->RSSetViewports(1, &vp);
}

// ------------------------------------------------------------------------------------------------ state objects
static D3D11_BLEND gl_blend(DWORD f)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return D3D11_BLEND_SRC_COLOR;
    case 0x301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return D3D11_BLEND_DEST_COLOR;
    case 0x307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}
static D3D11_COMPARISON_FUNC gl_func(DWORD f)
{
    return (D3D11_COMPARISON_FUNC)((f & 7) + 1);       // GL 0x200..0x207 == D3D11 1..8 (same order)
}

template <class Desc, class Obj> struct StateCache {
    std::map<std::string, Obj *> map;
};
static std::map<std::string, ID3D11BlendState *> g_blend_cache;
static std::map<std::string, ID3D11DepthStencilState *> g_depth_cache;
static std::map<std::string, ID3D11RasterizerState *> g_raster_cache;

// SetScissors: one inclusive rectangle in render-target pixels (exclusive / multi-rect scissoring is not used)
static bool g_scissor;
static D3D11_RECT g_scissor_rect;

static void apply_scissor(D3D11_RASTERIZER_DESC *rd)
{
    if (!g_scissor)
        return;
    rd->ScissorEnable = TRUE;
    float sx = (float)g_d3d.scene_width / g_d3d.width, sy = (float)g_d3d.scene_height / g_d3d.height;
    D3D11_RECT r = {(LONG)(g_scissor_rect.left * sx), (LONG)(g_scissor_rect.top * sy),
                    (LONG)(g_scissor_rect.right * sx + 0.5f), (LONG)(g_scissor_rect.bottom * sy + 0.5f)};
    if (g_bound.raster && g_bound.rd.ScissorEnable && !memcmp(&r, &g_bound.scissor, sizeof r))
        return;
    g_bound.scissor = r;
    g_d3d.ctx->RSSetScissorRects(1, &r);
}

static void apply_state(bool pretransformed)
{
    ID3D11Device *dev = g_d3d.dev;
    ID3D11DeviceContext *ctx = g_d3d.ctx;

    D3D11_BLEND_DESC bd = {};
    auto &rt = bd.RenderTarget[0];
    rt.BlendEnable = g_nv[NV_BLEND_ENABLE / 4] != 0;
    rt.SrcBlend = gl_blend(g_nv[NV_BLEND_SFACTOR / 4]);
    rt.DestBlend = gl_blend(g_nv[NV_BLEND_DFACTOR / 4]);
    rt.BlendOp = g_nv[NV_BLEND_EQUATION / 4] == 0x800A ? D3D11_BLEND_OP_SUBTRACT
               : g_nv[NV_BLEND_EQUATION / 4] == 0x800B ? D3D11_BLEND_OP_REV_SUBTRACT
               : g_nv[NV_BLEND_EQUATION / 4] == 0x8007 ? D3D11_BLEND_OP_MIN
               : g_nv[NV_BLEND_EQUATION / 4] == 0x8008 ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    DWORD cm = g_nv[NV_COLOR_MASK / 4];
    rt.RenderTargetWriteMask = ((cm >> 16) & 1 ? 1 : 0) | ((cm >> 8) & 1 ? 2 : 0) | (cm & 1 ? 4 : 0) | (cm >> 24 ? 8 : 0);
    // G-buffer depth: opaque engine-program draws only (no blending, depth written) - not FX, glass or 2D
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[1].RenderTargetWriteMask =
        !pretransformed && !rt.BlendEnable && g_nv[NV_DEPTH_MASK / 4] && g_nv[NV_DEPTH_TEST_ENABLE / 4] && rt.RenderTargetWriteMask
            ? D3D11_COLOR_WRITE_ENABLE_RED : 0;
    bd.RenderTarget[2].RenderTargetWriteMask = bd.RenderTarget[1].RenderTargetWriteMask ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    // motion vectors: every opaque program draw, depth written or not (sky domes, flat engine-drawn 2D: their own
    // motion, zero for a static panel) - only blended draws leave the pixel's motion to what is under them
    bd.RenderTarget[3].RenderTargetWriteMask = !pretransformed && !rt.BlendEnable && rt.RenderTargetWriteMask
                                                   ? D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN : 0;
    DWORD bc = g_nv[NV_BLEND_COLOR / 4];
    float factor[4] = {((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f};
    if (!g_bound.blend || memcmp(&bd, &g_bound.bd, sizeof bd) || memcmp(factor, g_bound.factor, sizeof factor)) {
        std::string key((const char *)&bd, sizeof bd);
        ID3D11BlendState *&bs = g_blend_cache[key];
        if (!bs)
            dev->CreateBlendState(&bd, &bs);
        ctx->OMSetBlendState(bs, factor, 0xFFFFFFFF);
        prof_count(PC_STATE_CHANGES);
        g_bound.blend = true;
        g_bound.bd = bd;
        memcpy(g_bound.factor, factor, sizeof factor);
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = g_nv[NV_DEPTH_TEST_ENABLE / 4] != 0;
    dd.DepthWriteMask = g_nv[NV_DEPTH_MASK / 4] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = gl_func(g_nv[NV_DEPTH_FUNC / 4]);
    if (!g_bound.depth || memcmp(&dd, &g_bound.dd, sizeof dd)) {
        std::string key((const char *)&dd, sizeof dd);
        ID3D11DepthStencilState *&ds = g_depth_cache[key];
        if (!ds)
            dev->CreateDepthStencilState(&dd, &ds);
        ctx->OMSetDepthStencilState(ds, 0);
        prof_count(PC_STATE_CHANGES);
        g_bound.depth = true;
        g_bound.dd = dd;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = (!pretransformed && g_nv[NV_CULL_FACE_ENABLE / 4])
                ? (g_nv[NV_CULL_FACE / 4] == 0x404 ? D3D11_CULL_FRONT : D3D11_CULL_BACK) : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = g_nv[NV_FRONT_FACE / 4] == 0x901;
    rd.DepthClipEnable = TRUE;
    if (g_nv[NV_POLY_OFFSET_FILL_ENABLE / 4]) {
        float bias = *(float *)&g_nv[NV_POLY_OFFSET_BIAS / 4], scale = *(float *)&g_nv[NV_POLY_OFFSET_SCALE / 4];
        rd.DepthBias = (INT)bias;
        rd.SlopeScaledDepthBias = scale;
    }
    apply_scissor(&rd);
    if (!g_bound.raster || memcmp(&rd, &g_bound.rd, sizeof rd)) {
        std::string key((const char *)&rd, sizeof rd);
        ID3D11RasterizerState *&rs = g_raster_cache[key];
        if (!rs)
            dev->CreateRasterizerState(&rd, &rs);
        ctx->RSSetState(rs);
        prof_count(PC_STATE_CHANGES);
        g_bound.raster = true;
        g_bound.rd = rd;
    }
}

// ------------------------------------------------------------------------------------------------ textures
struct TexEntry {
    ID3D11ShaderResourceView *srv;
    DWORD data, format, size;
    bool dirty;
    bool hd_pending;                                            // HD replacement still loading (tex_replace.cpp)
    DWORD sum, checked_frame;                                   // content sample, frame it was last verified
};
struct TexKey {
    DWORD data, format, size;
    bool operator<(const TexKey &o) const
    {
        return data != o.data ? data < o.data : format != o.format ? format < o.format : size < o.size;
    }
};
static void tex_dims(const XPixelContainer *t, UINT *w, UINT *h)
{
    if (t->Size) {
        *w = (t->Size & 0xFFF) + 1;
        *h = ((t->Size >> 12) & 0xFFF) + 1;
    } else {
        *w = 1u << ((t->Format >> 20) & 0xF);
        *h = 1u << ((t->Format >> 24) & 0xF);
    }
}
static std::map<TexKey, TexEntry> g_tex_cache;

void d3d_textures_all_dirty()
{
    D3DLock lock;
    for (auto &kv : g_tex_cache)
        kv.second.dirty = true;
}

void d3d_texture_dirty(XPixelContainer *t)
{
    auto it = g_tex_cache.find({t->Data, t->Format, t->Size});
    if (it != g_tex_cache.end())
        it->second.dirty = true;
}

static UINT swizzle_offset(UINT x, UINT y, UINT w, UINT h)
{
    UINT off = 0, bit = 1;
    for (UINT i = 1; i < w || i < h; i <<= 1) {
        if (i < w) {
            if (x & i)
                off |= bit;
            bit <<= 1;
        }
        if (i < h) {
            if (y & i)
                off |= bit;
            bit <<= 1;
        }
    }
    return off;
}

static inline BYTE clamp8(int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }

// one 32-bit BGRA texel from any non-DXT Xbox format
static DWORD texel_bgra(DWORD fmt, const BYTE *p)
{
    switch (fmt) {
    case 0x06: case 0x12: return *(const DWORD *)p;                                     // A8R8G8B8
    case 0x07: case 0x1E: return *(const DWORD *)p | 0xFF000000;                        // X8R8G8B8
    case 0x3A: case 0x3F: { DWORD v = *(const DWORD *)p;                                // A8B8G8R8
        return (v & 0xFF00FF00) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16); }
    case 0x04: case 0x1D: { WORD v = *(const WORD *)p;                                  // A4R4G4B4
        return ((v >> 12) * 17) << 24 | (((v >> 8) & 15) * 17) << 16 | (((v >> 4) & 15) * 17) << 8 | (v & 15) * 17; }
    case 0x02: case 0x10: { WORD v = *(const WORD *)p;                                  // A1R5G5B5
        return (v & 0x8000 ? 0xFF000000 : 0) | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 |
               (v & 31) * 255 / 31; }
    case 0x03: case 0x1C: { WORD v = *(const WORD *)p;                                  // X1R5G5B5
        return 0xFF000000 | (((v >> 10) & 31) * 255 / 31) << 16 | (((v >> 5) & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31; }
    case 0x05: case 0x11: { WORD v = *(const WORD *)p;                                  // R5G6B5
        return 0xFF000000 | ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31; }
    case 0x00: case 0x13: { BYTE l = *p; return 0xFF000000 | l << 16 | l << 8 | l; }    // L8
    case 0x01: case 0x1B: { BYTE l = *p; return (DWORD)l << 24 | l << 16 | l << 8 | l; } // AL8
    case 0x19: case 0x1F: return (DWORD)*p << 24 | 0xFFFFFF;                            // A8
    case 0x1A: case 0x20: { BYTE l = p[0], a = p[1]; return (DWORD)a << 24 | l << 16 | l << 8 | l; }  // A8L8
    }
    return 0xFFFF00FF;
}

// Movie frames (SFD, YUY2): the frame textures follow the screen size but the decoder fills only the 640x480 corner the
// movie quad reads. Only that extent is converted, into the existing texture.
static UINT g_yuv_use_w, g_yuv_use_h;                   // texel extent the current draw reads (0 = whole texture)

static void yuv_extent(UINT w, UINT h, UINT *cw, UINT *ch)
{
    *cw = g_yuv_use_w && g_yuv_use_w < w ? (g_yuv_use_w + 1) & ~1u : w;
    *ch = g_yuv_use_h && g_yuv_use_h < h ? g_yuv_use_h : h;
    if (*cw > w)
        *cw = w;
}

static void yuv_convert(DWORD fmt, const BYTE *src, UINT pitch, UINT cw, UINT ch, DWORD *out, UINT out_w)
{
    for (UINT y = 0; y < ch; y++) {
        const BYTE *row = src + y * pitch;
        DWORD *o = out + y * out_w;
        for (UINT x = 0; x < cw; x += 2) {
            const BYTE *q = row + x * 2;
            int y0, y1, u, v;
            if (fmt == 0x24) y0 = q[0], u = q[1], y1 = q[2], v = q[3];
            else u = q[0], y0 = q[1], v = q[2], y1 = q[3];
            int d = u - 128, e = v - 128;
            int rv = 409 * e + 128, gv = -100 * d - 208 * e + 128, bv = 516 * d + 128;
            for (int k = 0; k < 2 && x + k < cw; k++) {
                int c = 298 * ((k ? y1 : y0) - 16);
                o[x + k] = 0xFF000000 | clamp8((c + rv) >> 8) << 16 | clamp8((c + gv) >> 8) << 8 | clamp8((c + bv) >> 8);
            }
        }
    }
}

// changed movie frame -> the existing texture (only the extent the draw reads)
static bool yuv_update(ID3D11ShaderResourceView *srv, DWORD data, DWORD format, DWORD size)
{
    DWORD fmt = (format >> 8) & 0xFF;
    if ((fmt != 0x24 && fmt != 0x25) || !size || !srv)
        return false;
    UINT w = (size & 0xFFF) + 1, h = ((size >> 12) & 0xFFF) + 1, pitch = ((size >> 24) + 1) * 64, cw, ch;
    yuv_extent(w, h, &cw, &ch);
    static std::vector<DWORD> buf;
    buf.resize((size_t)cw * ch);
    yuv_convert(fmt, (const BYTE *)data, pitch, cw, ch, buf.data(), cw);
    ID3D11Resource *res = nullptr;
    srv->GetResource(&res);
    if (!res)
        return false;
    D3D11_BOX box = {0, 0, 0, cw, ch, 1};
    g_d3d.ctx->UpdateSubresource(res, 0, &box, buf.data(), cw * 4, 0);
    res->Release();
    return true;
}

static ID3D11ShaderResourceView *upload(XPixelContainer *t)
{
    DWORD fmt = (t->Format >> 8) & 0xFF;
    UINT w, h, levels = (t->Format >> 16) & 0xF, pitch;
    bool linear = t->Size != 0;
    if (linear) {
        w = (t->Size & 0xFFF) + 1;
        h = ((t->Size >> 12) & 0xFFF) + 1;
        pitch = ((t->Size >> 24) + 1) * 64;
        levels = 1;
    } else {
        w = 1u << ((t->Format >> 20) & 0xF);
        h = 1u << ((t->Format >> 24) & 0xF);
        pitch = 0;
    }
    if (!levels)
        levels = 1;
    const BYTE *src = (const BYTE *)t->Data;
    if (!src)
        return nullptr;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    std::vector<std::vector<DWORD>> converted;

    bool dxt = fmt == 0x0C || fmt == 0x0E || fmt == 0x0F;
    if (dxt) {
        td.Format = fmt == 0x0C ? DXGI_FORMAT_BC1_UNORM : fmt == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        UINT block = fmt == 0x0C ? 8 : 16;
        // D3D11 needs every BC level to be at least 4x4 blocks in the descriptor sense: stop at 4x4
        UINT lw = w, lh = h, n = 0;
        const BYTE *p = src;
        for (; n < levels; n++) {
            D3D11_SUBRESOURCE_DATA sd = {p, ((lw + 3) / 4) * block, 0};
            init.push_back(sd);
            p += ((lw + 3) / 4) * ((lh + 3) / 4) * block;
            if (lw <= 4 && lh <= 4) {
                n++;
                break;
            }
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        td.MipLevels = n;
    } else if (fmt == 0x24 || fmt == 0x25) {                // YUY2 / UYVY (linear) -> RGBA (BT.601)
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.MipLevels = 1;
        td.Usage = D3D11_USAGE_DEFAULT;                     // movie frames: updated in place (d3d_texture_srv)
        converted.emplace_back(w * h);
        UINT cw, ch;
        yuv_extent(w, h, &cw, &ch);
        yuv_convert(fmt, src, pitch, cw, ch, converted.back().data(), w);
        init.push_back({converted.back().data(), w * 4, 0});
    } else {                                                // everything else -> BGRA8 (unswizzled)
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        XFormatInfoPublic fi = d3d_xformat(fmt);
        UINT bpp = fi.bpp / 8;
        UINT lw = w, lh = h;
        const BYTE *p = src;
        for (UINT l = 0; l < levels; l++) {
            converted.emplace_back(lw * lh);
            DWORD *out = converted.back().data();
            for (UINT y = 0; y < lh; y++)
                for (UINT x = 0; x < lw; x++) {
                    UINT off = linear ? y * pitch + x * bpp : swizzle_offset(x, y, lw, lh) * bpp;
                    out[y * lw + x] = texel_bgra(fmt, p + off);
                }
            init.push_back({out, lw * 4, 0});
            p += linear ? pitch * lh : lw * lh * bpp;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        td.MipLevels = levels;
    }
    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = g_d3d.dev->CreateTexture2D(&td, init.data(), &tex);
    if (FAILED(hr) || !tex) {
        port_log("texture %p: create failed %08X (fmt %02X %ux%u levels %u)\n", t, hr, fmt, w, h, td.MipLevels);
        return nullptr;
    }
    ID3D11ShaderResourceView *srv = nullptr;
    hr = g_d3d.dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) {
        port_log("texture %p: SRV create failed %08X (fmt %02X %ux%u levels %u)\n", t, hr, fmt, w, h, td.MipLevels);
        return nullptr;
    }
    static int logged;
    if (logged++ < 200 || d3d_tracing()) {
        unsigned sum = 0, n = 0;                               // rough content check: mean of sampled source bytes
        UINT bytes = linear ? pitch * h : w * h * d3d_xformat(fmt).bpp / 8;      // level 0 size
        for (UINT i = 0; i < bytes && n < 4096; i += 997, n++)
            sum += src[i];
        port_log("texture %p: fmt %02X %ux%u levels %u %s data %08X mean byte %u\n", t, fmt, w, h, td.MipLevels,
                 linear ? "linear" : "swizzled", t->Data, n ? sum / n : 0);
    }
    return srv;
}

// Texture by content (Data address, Format, Size), for SetTexture and raw push binds. Texture memory is pooled and
// reused without a Lock, so once per frame a bound texture's level-0 bytes are sampled against the uploaded content.
static DWORD tex_sample_sum(DWORD data, DWORD format, DWORD size)
{
    DWORD fmt = (format >> 8) & 0xFF;
    UINT bytes;
    if (size)
        bytes = ((size >> 24) + 1) * 64 * (((size >> 12) & 0xFFF) + 1);
    else {
        UINT w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
        bool dxt = fmt == 0x0C || fmt == 0x0E || fmt == 0x0F;
        bytes = dxt ? ((w + 3) / 4) * ((h + 3) / 4) * (fmt == 0x0C ? 8 : 16) : w * h * d3d_xformat(fmt).bpp / 8;
    }
    const DWORD *p = (const DWORD *)data;
    UINT n = bytes / 4;
    if (!n)
        return 0;
    DWORD sum = 0x811C9DC5;
    UINT step = n > 512 ? n / 512 : 1;
    for (UINT i = 0; i < n; i += step)
        sum = (sum ^ p[i]) * 0x01000193;
    return (sum ^ p[n - 1]) * 0x01000193;
}

DWORD d3d_tex_sample_sum(DWORD data, DWORD format, DWORD size) { return tex_sample_sum(data, format, size); }

// one level-0 texel of a swizzled / DXT texture as BGRA (tex_replace.cpp checks HD images against the original)
DWORD d3d_texel(DWORD data, DWORD format, UINT x, UINT y)
{
    DWORD fmt = (format >> 8) & 0xFF;
    UINT w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
    const BYTE *src = (const BYTE *)data;
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        UINT bw = (w + 3) / 4, bsize = fmt == 0x0C ? 8 : 16;
        const BYTE *blk = src + ((y / 4) * bw + x / 4) * bsize;
        UINT i = (y & 3) * 4 + (x & 3);
        const BYTE *cb = fmt == 0x0C ? blk : blk + 8;
        WORD c0 = *(const WORD *)cb, c1 = *(const WORD *)(cb + 2);
        UINT sel = (*(const DWORD *)(cb + 4) >> (i * 2)) & 3;
        int r[4], g[4], b[4];
        r[0] = (c0 >> 11) * 255 / 31, g[0] = ((c0 >> 5) & 63) * 255 / 63, b[0] = (c0 & 31) * 255 / 31;
        r[1] = (c1 >> 11) * 255 / 31, g[1] = ((c1 >> 5) & 63) * 255 / 63, b[1] = (c1 & 31) * 255 / 31;
        DWORD a = 255;
        if (fmt != 0x0C || c0 > c1) {
            r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, b[2] = (2 * b[0] + b[1]) / 3;
            r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, b[3] = (b[0] + 2 * b[1]) / 3;
        } else {
            r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, b[2] = (b[0] + b[1]) / 2;
            r[3] = g[3] = b[3] = 0;
            if (sel == 3)
                a = 0;
        }
        if (fmt == 0x0E) {                                      // DXT3: explicit 4-bit alpha
            a = ((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17;
        } else if (fmt == 0x0F) {                               // DXT5: interpolated alpha
            UINT a0 = blk[0], a1 = blk[1];
            unsigned long long bits = 0;
            for (int k = 0; k < 6; k++)
                bits |= (unsigned long long)blk[2 + k] << (8 * k);
            UINT ai = (UINT)(bits >> (i * 3)) & 7;
            if (ai == 0) a = a0;
            else if (ai == 1) a = a1;
            else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
            else a = ai == 6 ? 0 : ai == 7 ? 255 : ((6 - ai) * a0 + (ai - 1) * a1) / 5;
        }
        return a << 24 | (DWORD)r[sel] << 16 | (DWORD)g[sel] << 8 | (DWORD)b[sel];
    }
    UINT bpp = d3d_xformat(fmt).bpp / 8;
    if (!bpp)
        return 0;
    return texel_bgra(fmt, src + swizzle_offset(x, y, w, h) * bpp);
}

// [debug] traced frame: write each bound DXT texture's level 0 as build\tex\<data>_<format>.dds
static void tex_dump_dds(DWORD data, DWORD format)
{
    DWORD fmt = (format >> 8) & 0xFF;
    if (fmt != 0x0C && fmt != 0x0E && fmt != 0x0F)
        return;
    UINT w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
    UINT bytes = ((w + 3) / 4) * ((h + 3) / 4) * (fmt == 0x0C ? 8 : 16);
    char path[MAX_PATH];
    CreateDirectoryA("tex", nullptr);
    sprintf(path, "tex/%08X_%08X.dds", data, format);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    DWORD hdr[32] = {};
    hdr[0] = 0x20534444;                                          // "DDS "
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;                 // caps, height, width, pixelformat, linearsize
    hdr[3] = h;
    hdr[4] = w;
    hdr[5] = bytes;
    hdr[19] = 32;
    hdr[20] = 0x4;                                                // DDPF_FOURCC
    hdr[21] = fmt == 0x0C ? 0x31545844 : fmt == 0x0E ? 0x33545844 : 0x35545844;
    hdr[27] = 0x1000;
    fwrite(hdr, 4, 32, f);
    UINT total = 0, levels = (format >> 16) & 0xF, lw = w, lh = h;  // level 0 + the rest of the chain after it
    for (UINT l = 0; l < (levels ? levels : 1); l++) {
        total += ((lw + 3) / 4) * ((lh + 3) / 4) * (fmt == 0x0C ? 8 : 16);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    fwrite((const void *)data, 1, total, f);
    fclose(f);
}

ID3D11ShaderResourceView *d3d_texture_srv(DWORD data, DWORD format, DWORD size)
{
    if (!data)
        return nullptr;
    ProfScope pz(PZ_TEXTURE);
    if (d3d_tracing() && !size)
        tex_dump_dds(data, format);
    TexEntry &e = g_tex_cache[{data, format, size}];
    if (e.srv && !e.dirty) {
        if (e.checked_frame == g_d3d.frame)
            return e.srv;
        e.checked_frame = g_d3d.frame;
        prof_count(PC_TEX_CHECKS);
        DWORD sum = tex_sample_sum(data, format, size);
        if (sum == e.sum) {
            if (e.hd_pending) {                                 // HD image finished loading: swap it in
                bool pending = false;
                if (ID3D11ShaderResourceView *hd = tex_replace_srv(data, format, size, &pending)) {
                    e.srv->Release();
                    e.srv = hd;
                }
                e.hd_pending = pending;
            }
            return e.srv;
        }
        static int changes;
        if (changes++ < 50 || d3d_tracing())
            port_log("texture %08X fmt %08X: content changed, re-upload\n", data, format);
    }
    if (e.srv && !e.hd_pending) {                               // movie frame: rewrite the texture in place
        ProfScope pu(PZ_TEXUPLOAD);
        if (yuv_update(e.srv, data, format, size)) {
            prof_count(PC_TEX_UPLOADS);
            e.dirty = false;
            e.sum = tex_sample_sum(data, format, size);
            e.checked_frame = g_d3d.frame;
            return e.srv;
        }
    }
    if (e.srv)
        e.srv->Release();
    XPixelContainer t = {};
    t.Data = data;
    t.Format = format;
    t.Size = size;
    {
        ProfScope pu(PZ_TEXUPLOAD);
        prof_count(PC_TEX_UPLOADS);
        UINT w = size ? (size & 0xFFF) + 1 : 1u << ((format >> 20) & 0xF);
        UINT h = size ? ((size >> 12) & 0xFFF) + 1 : 1u << ((format >> 24) & 0xF);
        prof_count(PC_TEX_BYTES, (unsigned long long)w * h * 4);   // decoded size (approx.)
        e.hd_pending = false;
        e.srv = tex_replace_srv(data, format, size, &e.hd_pending);   // HD pack (tex_replace.cpp)
        if (!e.srv)
            e.srv = upload(&t);
    }
    e.data = data;
    e.format = format;
    e.size = size;
    e.dirty = false;
    e.sum = tex_sample_sum(data, format, size);
    e.checked_frame = g_d3d.frame;
    return e.srv;
}

static ID3D11ShaderResourceView *texture_srv(XPixelContainer *t)
{
    return t ? d3d_texture_srv(t->Data, t->Format, t->Size) : nullptr;
}

// ------------------------------------------------------------------------------------------------ FVF draws
static UINT fvf_expand(DWORD fvf, const BYTE *v, CanonVertex *out)
{
    const BYTE *p = v;
    memset(out, 0, sizeof *out);
    out->color = 0xFFFFFFFF;
    switch (fvf & 0x0E) {
    case 0x04: memcpy(out->pos, p, 16); p += 16; break;                      // XYZRHW
    case 0x02: memcpy(out->pos, p, 12); out->pos[3] = 1; p += 12; break;     // XYZ (transformed later)
    default: memcpy(out->pos, p, 12); out->pos[3] = 1; p += 12 + 4 * (((fvf & 0x0E) - 4) / 2); break;  // XYZBn
    }
    if (fvf & 0x10) p += 12;                                                  // normal
    if (fvf & 0x20) p += 4;                                                   // point size
    if (fvf & 0x40) { out->color = *(const DWORD *)p; p += 4; }               // diffuse
    if (fvf & 0x80) p += 4;                                                   // specular
    UINT ntex = (fvf >> 8) & 0xF;
    for (UINT i = 0; i < ntex; i++) {
        UINT fmt = (fvf >> (16 + i * 2)) & 3;
        UINT n = fmt == 0 ? 2 : fmt == 1 ? 3 : fmt == 2 ? 4 : 1;
        if (i < 2)
            memcpy(i ? out->uv1 : out->uv0, p, (n < 2 ? n : 2) * 4);
        p += n * 4;
    }
    return (UINT)(p - v);
}

// Xbox primitive -> triangle/line/point list indices (UP draws are expanded on the CPU)
static void expand_primitive(DWORD type, UINT count, std::vector<UINT> &idx, D3D11_PRIMITIVE_TOPOLOGY *topo)
{
    idx.clear();
    *topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (type) {
    case 1: *topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; for (UINT i = 0; i < count; i++) idx.push_back(i); break;
    case 2: *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; for (UINT i = 0; i + 1 < count; i += 2) idx.push_back(i), idx.push_back(i + 1); break;
    case 3: case 4:
        *topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for (UINT i = 0; i + 1 < count; i++) idx.push_back(i), idx.push_back(i + 1);
        if (type == 3 && count > 2) idx.push_back(count - 1), idx.push_back(0);
        break;
    case 5: for (UINT i = 0; i + 2 < count; i += 3) idx.push_back(i), idx.push_back(i + 1), idx.push_back(i + 2); break;
    case 6:
        for (UINT i = 0; i + 2 < count; i++)
            if (i & 1) idx.push_back(i + 1), idx.push_back(i), idx.push_back(i + 2);
            else idx.push_back(i), idx.push_back(i + 1), idx.push_back(i + 2);
        break;
    case 7: case 10: for (UINT i = 1; i + 1 < count; i++) idx.push_back(0), idx.push_back(i), idx.push_back(i + 1); break;
    case 8:
        for (UINT i = 0; i + 3 < count; i += 4)
            idx.push_back(i), idx.push_back(i + 1), idx.push_back(i + 2), idx.push_back(i), idx.push_back(i + 2), idx.push_back(i + 3);
        break;
    case 9:
        for (UINT i = 0; i + 3 < count; i += 2)
            idx.push_back(i), idx.push_back(i + 1), idx.push_back(i + 3), idx.push_back(i), idx.push_back(i + 3), idx.push_back(i + 2);
        break;
    }
}

// The engine's particle renderer fx_Render (0x1AF8E0) for 3D effects (FX_Render3d 0x851A9 and the main pass 0x1CD3B3;
// 0x851EF is the 2D menu pass). Its draws are the soft-particle candidates.
void d3d_ssao_apply();                                  // d3d_ssao.cpp
bool d3d_trace_frame();
static void __cdecl fx_render_3d(int arg)
{
    if (d3d_trace_frame())
        port_log("order: ---- fx_render_3d (arg %d)\n", arg);
    {
        ProfScope pz(PZ_SHAPPLY);
        d3d_shadow_apply();                             // opaque scene done: sun shadows, ambient occlusion -
    }
    {
        ProfScope pz(PZ_POST);
        d3d_ssao_apply();                               // before particles and the 2D layer
    }
    g_d3d.in_fx = true;
    ((void(__cdecl *)(int))0x1AF8E0)(arg);
    g_d3d.in_fx = false;
    d3d_taa_mark_3d();
}

void d3d_install_fx_hooks()
{
    d3d_shadow_install_cull();                          // d3d_shadow.cpp: off-screen casters still get drawn
    char ini[MAX_PATH];                                 // [debug] NoFxHooks=1: leave the engine's calls alone (bisect)
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    if (GetPrivateProfileIntA("debug", "NoFxHooks", 0, ini)) {
        port_log("renderer: fx_Render hooks OFF ([debug] NoFxHooks) - no soft particles / shadows / SSAO\n");
        return;
    }
    for (DWORD site : {0x851A9u, 0x1CD3B3u}) {
        BYTE *p = (BYTE *)site;
        if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != 0x1AF8E0) {
            port_log("renderer: fx_Render call at %06X unexpected, not hooked\n", site);
            continue;
        }
        *(int32_t *)(p + 1) = (int32_t)((DWORD)fx_render_3d - (site + 5));
    }
}

// [debug] TraceOrder=N: log every draw of frame N in order (3D -> 2D boundary for TAA)
bool d3d_trace_frame()
{
    static int at = -2;
    if (at == -2) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        at = GetPrivateProfileIntA("debug", "TraceOrder", -1, ini);
    }
    return at >= 0 && g_d3d.frame == (DWORD)at;
}

void d3d_begin_draw(bool pretransformed, bool soft)
{
    if (d3d_trace_frame()) {
        static unsigned n;
        port_log("order: #%u vs %d pre %d ztest %d zwrite %d blend %d (%X/%X) in_fx %d\n", n++,
                 (int)g_cur_vs, (int)pretransformed, (int)(g_nv[0x30C / 4] != 0), (int)(g_nv[0x35C / 4] != 0),
                 (int)(g_nv[0x304 / 4] != 0), g_nv[0x344 / 4], g_nv[0x348 / 4], (int)g_d3d.in_fx);
    }
    d3d_taa_before_draw(g_nv[NV_DEPTH_TEST_ENABLE / 4] != 0);   // TAA resolve before the 2D layer
    ProfScope pz(PZ_STATE);
    render_init();
    bind_targets(soft);
    apply_state(pretransformed);
}
bool d3d_ready() { render_init(); return g_ready; }
DWORD d3d_nv(DWORD method) { return g_nv[method / 4]; }

void d3d_expand_primitive(DWORD type, UINT count, std::vector<UINT> &idx, D3D11_PRIMITIVE_TOPOLOGY *topo)
{
    expand_primitive(type, count, idx, topo);
}

static void draw_canonical(const std::vector<CanonVertex> &verts, DWORD prim, UINT count)
{
    ProfScope pz(PZ_FIXED);
    prof_count(PC_DRAW_FIXED);
    static std::vector<UINT> idx;                       // reused (all D3D entry points run under the D3D lock)
    D3D11_PRIMITIVE_TOPOLOGY topo;
    expand_primitive(prim, count, idx, &topo);
    if (idx.empty())
        return;
    UINT bytes = (UINT)idx.size() * sizeof(CanonVertex);
    if (bytes > VB_BYTES)
        return;
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vb_pos + bytes > VB_BYTES)
        g_vb_pos = 0, mode = D3D11_MAP_WRITE_DISCARD;
    ctx->Map(g_vb, 0, mode, 0, &m);
    CanonVertex *dst = (CanonVertex *)((BYTE *)m.pData + g_vb_pos);
    for (size_t i = 0; i < idx.size(); i++)
        dst[i] = verts[idx[i]];
    ctx->Unmap(g_vb, 0);
    UINT stride = sizeof(CanonVertex), offset = g_vb_pos;
    g_vb_pos += bytes;

    bind_targets();
    apply_state(true);
    static bool vs_cb_set;
    if (!vs_cb_set) {                                   // constant: game back-buffer size
        vs_cb_set = true;
        float vs_cb[4] = {2.0f / g_d3d.width, -2.0f / g_d3d.height, 0, 0};
        ctx->UpdateSubresource(g_cb_vs, 0, nullptr, vs_cb, 0, 0);
    }
    if (g_yuv_enable) {                                 // movie: the texel extent the quad reads (yuv_extent)
        float mu = 0, mv = 0;
        for (UINT i = 0; i < count; i++) {
            mu = verts[i].uv0[0] > mu ? verts[i].uv0[0] : mu;
            mv = verts[i].uv0[1] > mv ? verts[i].uv0[1] : mv;
        }
        if (mu > 2 && mv > 2) {                         // texel UVs (normalized ones: convert it all)
            g_yuv_use_w = (UINT)mu + 2;                 // + the bilinear neighbour
            g_yuv_use_h = (UINT)mv + 2;
        }
    }
    ID3D11ShaderResourceView *srv = texture_srv(g_textures[0]);
    g_yuv_use_w = g_yuv_use_h = 0;
    float sx = 1, sy = 1;
    if (srv && g_textures[0]->Size) {                   // linear texture: Xbox uses unnormalized texel coordinates
        UINT tw, th;
        tex_dims(g_textures[0], &tw, &th);
        sx = 1.0f / tw;
        sy = 1.0f / th;
        static int movie_logs;                          // [diagnostic] movie quad (YUV) geometry
        if (g_yuv_enable && movie_logs < 6) {
            movie_logs++;
            port_log("movie quad: tex %ux%u, %u verts: pos (%.1f %.1f) (%.1f %.1f) uv (%.1f %.1f) (%.1f %.1f)\n", tw, th,
                     count, verts[0].pos[0], verts[0].pos[1], verts[count > 2 ? 2 : 0].pos[0],
                     verts[count > 2 ? 2 : 0].pos[1], verts[0].uv0[0], verts[0].uv0[1],
                     verts[count > 2 ? 2 : 0].uv0[0], verts[count > 2 ? 2 : 0].uv0[1]);
        }
    }
    float ps_cb[12] = {srv ? 1.0f : 0.0f, g_yuv_enable ? 1.0f : 0.0f, 0, 0,
                       g_nv[NV_ALPHA_TEST_ENABLE / 4] ? 1.0f : 0.0f, (g_nv[NV_ALPHA_REF / 4] & 0xFF) / 255.0f,
                       (float)(g_nv[NV_ALPHA_FUNC / 4] & 7), 0, sx, sy, 0, 0};
    static float last_ps_cb[12];
    static bool ps_cb_set;
    if (!ps_cb_set || memcmp(ps_cb, last_ps_cb, sizeof ps_cb)) {
        ps_cb_set = true;
        memcpy(last_ps_cb, ps_cb, sizeof ps_cb);
        ctx->UpdateSubresource(g_cb_ps, 0, nullptr, ps_cb, 0, 0);
    }
    g_d3d.draws++;
    prof_gpu_segment(g_nv[NV_BLEND_ENABLE / 4] != 0);
    ctx->IASetInputLayout(g_layout);
    ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(topo);
    ctx->VSSetShader(g_vs_rhw, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb_vs);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_cb_ps);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    ctx->Draw((UINT)idx.size(), 0);
}

// ------------------------------------------------------------------------------------------------ entry points
extern "C" {

void __fastcall d3d_D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderState_Simple(%08X, %08X)", method, value);
    static std::set<unsigned long long> seen;                 // [debug] every distinct state write once
    if (seen.size() < 300 && seen.insert((unsigned long long)method << 32 | value).second)
        port_log("RS simple %04X = %08X (frame %u)\n", method & 0x1FFC, value, g_d3d.frame);
    d3d_nv_method(method & 0x1FFC, &value, 1);
}

// Generic render-state entry (by D3DRS index), dispatched like the Xbox runtime using its tables in the XBE: simple
// states -> NV2A method, deferred states -> D3D_g_RenderState + dirty bits, complex states -> our SetRenderState_*.
enum : DWORD {
    XBE_RS_SIMPLE_METHODS = 0x342FC0,                     // D3DSIMPLERENDERSTATEENCODE[0x5C]
    XBE_RS_DEFERRED_DIRTY = 0x342D90,                     // dirty bit per deferred state
    XBE_RS_COMPLEX_FUNCS = 0x2BA330,                      // setter per complex state (indexed by state)
    XBE_RENDER_STATE = 0x2C53D0,                          // D3D_g_RenderState[]
    XBE_STATE_DIRTY = 0x2C51C8,
};
void __stdcall d3d_D3DDevice_SetRenderStateNotInline(DWORD state, DWORD value)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderStateNotInline(%u, %08X)", state, value);
    if (state < 0x5C) {
        d3d_D3DDevice_SetRenderState_Simple(((const DWORD *)XBE_RS_SIMPLE_METHODS)[state], value);
        ((DWORD *)XBE_RENDER_STATE)[state] = value;
    } else if (state < 0x88) {
        *(DWORD *)XBE_STATE_DIRTY |= ((const DWORD *)XBE_RS_DEFERRED_DIRTY)[state];
        ((DWORD *)XBE_RENDER_STATE)[state] = value;
    } else if (state < 0xA6) {                          // table ends at RopZRead / DoNotCullUncompressed
        ((void(__stdcall *)(DWORD))((const DWORD *)XBE_RS_COMPLEX_FUNCS)[state])(value);
    }
}

void __stdcall d3d_D3DDevice_SetRenderState_FogColor(DWORD argb)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderState_FogColor(%08X)", argb);
    ((DWORD *)XBE_RENDER_STATE)[0x8A] = argb;           // D3DRS_FOGCOLOR
    g_nv[0x2A8 / 4] = (argb & 0xFF00FF00) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);   // NV2A: R in byte 0
}

struct XRect { LONG x1, y1, x2, y2; };                   // D3DRECT
DWORD __stdcall d3d_D3DDevice_SetScissors(DWORD count, BOOL exclusive, const XRect *rects)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetScissors(%u, %d, %d %d %d %d)", count, exclusive, count ? rects->x1 : 0, count ? rects->y1 : 0,
              count ? rects->x2 : 0, count ? rects->y2 : 0);
    g_scissor = count && rects && !exclusive;
    if (g_scissor)
        g_scissor_rect = {rects->x1, rects->y1, rects->x2, rects->y2};
    return 0;
}

void __stdcall d3d_D3DDevice_SetRenderState_ZEnable(DWORD v)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderState_ZEnable(%u)", v);
    g_nv[NV_DEPTH_TEST_ENABLE / 4] = v != 0;
}

void __stdcall d3d_D3DDevice_SetRenderState_CullMode(DWORD v)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderState_CullMode(%X)", v);
    g_nv[NV_CULL_FACE_ENABLE / 4] = v != 0;            // Xbox D3DCULL: 0 none, 0x900 CW, 0x901 CCW
    if (v)
        g_nv[NV_FRONT_FACE / 4] = v == 0x900 ? 0x901 : 0x900;   // cull CW -> front = CCW
}

void __stdcall d3d_D3DDevice_SetRenderState_YuvEnable(DWORD v)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetRenderState_YuvEnable(%u)", v);
    g_yuv_enable = v != 0;
}

void __stdcall d3d_D3DDevice_SetVertexShader(DWORD handle)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetVertexShader(%X)", handle);
    if (!(handle & 1)) {
        g_fvf = handle;
        g_cur_vs = 0xFFFFFFFF;                          // fixed-function (FVF) mode until the next program select
    }
}

void __stdcall d3d_D3DDevice_SetTexture(DWORD stage, XPixelContainer *tex)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("SetTexture(%u, %p)", stage, tex);
    if (stage < 4) {
        g_textures[stage] = tex;
        d3d_stage_set(stage, tex ? tex->Data : 0, tex ? tex->Format : 0, tex ? tex->Size : 0);
    }
}

void __stdcall d3d_D3DDevice_DrawVerticesUP(DWORD prim, UINT count, const BYTE *data, UINT stride)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("DrawVerticesUP(prim %u, %u verts, stride %u, fvf %X)", prim, count, stride, g_fvf);
    render_init();
    if (!g_ready || !data || !count)
        return;
    if (d3d_program_draw_up(prim, count, data, stride))
        return;
    static std::vector<CanonVertex> verts;
    verts.resize(count);
    for (UINT i = 0; i < count; i++) {
        fvf_expand(g_fvf, data + i * stride, &verts[i]);
        if (i < 4)
            d3d_trace("  v%u pos %.1f %.1f %.3f %.3f  col %08X  uv %.3f %.3f", i, verts[i].pos[0], verts[i].pos[1],
                      verts[i].pos[2], verts[i].pos[3], verts[i].color, verts[i].uv0[0], verts[i].uv0[1]);
    }
    draw_canonical(verts, prim, count);
}

HRESULT __stdcall d3d_D3DDevice_Clear(DWORD count, const void *rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
    D3DLock lock;
    d3d_drain();
    d3d_trace("Clear(flags %X, color %08X, z %f)", flags, color, z);
    render_init();
    if (!g_ready)
        return S_OK;
    if (flags & 0xF0) {
        float c[4] = {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f,
                      (color >> 24) / 255.f};
        g_d3d.ctx->ClearRenderTargetView(g_d3d.rtv, c);
    }
    if ((flags & 1) && g_d3d.gbuf_rtv) {                // G-buffer depth: "nothing here" = far
        float far_[4] = {65504.0f, 0, 0, 0};
        g_d3d.ctx->ClearRenderTargetView(g_d3d.gbuf_rtv, far_);
        if (g_d3d.gnrm_rtv) {
            float none[4] = {};
            g_d3d.ctx->ClearRenderTargetView(g_d3d.gnrm_rtv, none);
        }
        if (g_d3d.gmv_rtv) {                            // motion: unknown (TAA falls back to the camera)
            float unknown[4] = {60000.0f, 0, 0, 0};
            g_d3d.ctx->ClearRenderTargetView(g_d3d.gmv_rtv, unknown);
        }
    }
    if (flags & 3)
        g_d3d.ctx->ClearDepthStencilView(g_dsv, ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0),
                                         z, (UINT8)stencil);
    return S_OK;
}

}
