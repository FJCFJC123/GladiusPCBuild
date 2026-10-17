// Sun shadows ([video] Shadows=1, ShadowStrength, ShadowDistance, ShadowMapSize; [debug] ShowShadow=1).
// Gladius has blob shadows only. Here, during the frame, every opaque engine-program draw that casts (cached vertex
// buffers; not sky / sun / blob) is recorded: shader, layout, buffers, its indices and its vertex constants. When the
// engine enters its 3D particle renderer (opaque scene complete - d3d_render.cpp fx_render_3d) the draws are replayed
// from the sun into a depth map, then one full-screen pass darkens the sun-facing pixels the map says are occluded.
// Conventions come from the constants themselves (Xbox slot n = HLSL c[n + 96]):
//   WVP c80..83: rows of the row-vector matrix (clip = sum r4.i * c[80+i]); WV c76..79: rows of the column-vector
//   matrix (view.k = dot(c[76+k], pos)). Projection P = WVP * WV^-1 per frame, so positions rebuilt from the
//   G-buffer depth (clip w) land in the engine's own view space - no handedness assumptions.
//   Screen-space programs map clip -> pixels with c94 / c95 and our translator maps pixels back over rt_size: the
//   replay sets them so the round trip yields the light's clip space.
// Sun: brightest light of the engine's light list, read during lit draws and turned into view space with that
// draw's WV (the list is per object, in object space).
#include "d3d.h"
#include "d3d_prof.h"
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <math.h>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

enum { VSC = 192 + 16 + 2 };                            // float4s in the vertex constant block (d3d_program.cpp)
struct Cast {
    ID3D11VertexShader *vs;
    ID3D11InputLayout *layout;
    ID3D11Buffer *bufs[16];
    UINT strd[16], offs[16];
    D3D11_PRIMITIVE_TOPOLOGY topo;
    UINT first, count;                                  // in g_idx (ib == nullptr) or in ib
    ID3D11Buffer *ib;                                   // the draw's cached index buffer (referenced)
    unsigned long long idx_hash;                        // first / last 64 indices (cast_key)
    UINT consts;                                        // index into g_consts (VSC float4s)
    ID3D11ShaderResourceView *tex0;                     // alpha-tested casters (leaves, fences)
    float aref;
    bool scene;                                         // drawn with the main scene camera (set in apply)
    const char *name;                                   // texture asset name (diagnostics; interned, d3d_program.cpp)
    bool dynamic;                                       // lit (character / unit) draw: never cached
};
static std::vector<Cast> g_casts;
static std::vector<UINT> g_idx;
static std::vector<float> g_consts;
static UINT g_last_consts;                              // block of the last recorded cast (d3d_shadow_record)
static DWORD g_last_gen;
static bool g_init, g_on;
static bool g_use_cache;                                // [video] ShadowCache: replay remembered off-screen scenery
static float g_cull_widen = 2.0f, g_cull_back = 30.0f, g_cull_sun = 25.0f;  // [video] ShadowCullWiden / Back / Sun
static float g_strength = 0.55f, g_distance = 100.0f, g_min_elev = 50.0f;
static UINT g_size = 4096;
static int g_show;
static float g_sun[3];                                  // light travel direction, view space (this frame)
static float g_up[3] = {0, 1, 0};                       // world up, view space (from a lit object's axes)
static float g_sun_obj[3], g_sun_wvp[16];               // brightest light: direction in its draw's input space + WVP
static float g_prevLP[16];                              // last frame's view -> light clip (L * P^-1)
static float g_prevP[16], g_prevD[3];                   // last frame's projection and sun (view space, travel)
static bool g_prev_valid;
static DWORD g_prev_frame;
static float g_cam_P[16], g_cam_R[16];                  // this frame's projection and world -> view (TAA)
static DWORD g_cam_frame = 0xFFFFFFFF;

bool d3d_camera_this_frame(float P[16], float R[16])
{
    if (g_cam_frame != g_d3d.frame)
        return false;
    memcpy(P, g_cam_P, sizeof g_cam_P);
    memcpy(R, g_cam_R, sizeof g_cam_R);
    return true;
}
static void wvp_of(const float *vsc, float m[16]);
static void mul4(const float a[16], const float b[16], float o[16]);
static float g_sun_lum;
static DWORD g_sun_frame = 0xFFFFFFFF, g_done_frame = 0xFFFFFFFF;

static ID3D11Texture2D *g_map;
static ID3D11DepthStencilView *g_dsv;
static ID3D11ShaderResourceView *g_srv;
static ID3D11Buffer *g_cb_vs, *g_cb_ps, *g_cb_comp, *g_ib;
static UINT g_ib_cap;
static ID3D11PixelShader *g_ps_cast, *g_ps_comp;
static ID3D11VertexShader *g_vs_full;
static ID3D11RasterizerState *g_rs_cast, *g_rs_full;
static ID3D11DepthStencilState *g_ds_cast, *g_ds_off;
static ID3D11BlendState *g_bs_none, *g_bs_mul, *g_bs_copy;
static ID3D11SamplerState *g_cmp, *g_smp;

static const char *k_hlsl = R"(
struct PIn { float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0;
             float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };
Texture2D tex0 : register(t0);
SamplerState smp : register(s0);
cbuffer CastCB : register(b1) { float4 cast; };        // x: alpha test on, y: ref
void cast_ps(PIn i)
{
    if (cast.x > 0 && tex0.Sample(smp, i.t0.xy).a < cast.y) discard;
}

Texture2D<float> gdepth : register(t0);
Texture2D<float> smap : register(t1);
Texture2D gnorm : register(t2);                         // G-buffer normals (view space), alpha 1 = present
SamplerComparisonState cmp : register(s1);
cbuffer CompCB : register(b0) {
    float4 P[4];        // engine projection (column-vector rows)
    float4 L[4];        // view -> light clip
    float4 sun;         // xyz: light travel direction (view space), w: strength
    float4 info;        // full w, h, 1/map size, show
    float4 bias;        // x: constant, y: slope - both in light-depth units (game units / box depth); z: distance
};
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V full_vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 view_pos(float2 uv, float w)                    // invert P for a pixel with clip w
{
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float z = (w - P[3].w) / P[3].z;
    float x = (ndc.x * w - P[0].z * z - P[0].w) / P[0].x;
    float y = (ndc.y * w - P[1].z * z - P[1].w) / P[1].y;
    return float3(x, y, z);
}
float d_at(float2 uv) { return abs(gdepth.Load(int3(clamp(uv, 0, 0.9999) * info.xy, 0))); }   // < 0: lit draw
float4 comp_ps(V i) : SV_Target
{
    if (gdepth.Load(int3(clamp(i.uv, 0, 0.9999) * info.xy, 0)) < 0) return 1;   // characters shadow themselves
    float w = d_at(i.uv);
    if (w > 60000 || w <= 0) return 1;
    float3 Pv = view_pos(i.uv, w);
    float2 px = 1.0 / info.xy;
    float3 Pr = view_pos(i.uv + float2(px.x, 0), d_at(i.uv + float2(px.x, 0)));
    float3 Pl = view_pos(i.uv - float2(px.x, 0), d_at(i.uv - float2(px.x, 0)));
    float3 Pd = view_pos(i.uv + float2(0, px.y), d_at(i.uv + float2(0, px.y)));
    float3 Pu = view_pos(i.uv - float2(0, px.y), d_at(i.uv - float2(0, px.y)));
    float3 dx = abs(Pr.z - Pv.z) < abs(Pv.z - Pl.z) ? Pr - Pv : Pv - Pl;
    float3 dy = abs(Pd.z - Pv.z) < abs(Pv.z - Pu.z) ? Pd - Pv : Pv - Pu;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, -Pv) < 0) N = -N;                       // towards the camera
    float4 gn = gnorm.Load(int3(clamp(i.uv, 0, 0.9999) * info.xy, 0));
    if (gn.w > 0.5) N = normalize(gn.xyz * 2 - 1);      // the surface's own normal where the draw wrote one
    float ndl = dot(N, -sun.xyz);
    float facing = smoothstep(0.08, 0.35, ndl);        // only surfaces the sun lights can be shadowed
    float4 lp = float4(dot(L[0], float4(Pv, 1)), dot(L[1], float4(Pv, 1)), dot(L[2], float4(Pv, 1)), 1);
    float2 suv = lp.xy * float2(0.5, -0.5) + 0.5;
    if (any(suv < 0) || any(suv > 1) || lp.z > 1) return 1;
    float zr = lp.z - bias.x - bias.y * sqrt(saturate(1 - ndl * ndl)) / max(ndl, 0.1);   // receiver bias
    static const float2 pd[16] = {float2(-0.94, -0.40), float2(0.95, -0.77), float2(-0.09, -0.93), float2(0.34, 0.29),
        float2(-0.92, 0.46), float2(-0.38, 0.65), float2(0.19, -0.27), float2(0.79, 0.19), float2(-0.54, -0.87),
        float2(0.44, -0.69), float2(0.98, 0.76), float2(0.73, 0.70), float2(-0.10, 0.97), float2(0.31, -0.17),
        float2(-0.23, 0.43), float2(-0.72, -0.08)};
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    float2 cs = float2(cos(ang), sin(ang));
    float vis = 0;
    [unroll] for (int k = 0; k < 16; k++) {
        float2 o = float2(pd[k].x * cs.x - pd[k].y * cs.y, pd[k].x * cs.y + pd[k].y * cs.x) * 2.5;
        vis += smap.SampleCmpLevelZero(cmp, suv + o * info.z, zr);
    }
    vis /= 16;
    float dist_fade = saturate((bias.z - w) / (0.25 * bias.z));   // fade out over the last quarter of the distance
    float s = (1 - vis) * facing * dist_fade * sun.w;
    if (info.w > 0) return float4(1 - s, 1 - s, 1 - s, 1);
    return float4((1 - s).xxx, 1);
}
)";

static bool init()
{
    if (g_init)
        return g_on;
    g_init = true;
    char ini[MAX_PATH], num[32];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    g_d3d.shadows = GetPrivateProfileIntA("video", "Shadows", 0, ini) != 0;   // start value; F5 toggles
    g_on = true;
    GetPrivateProfileStringA("video", "ShadowStrength", "0.55", num, sizeof num, ini);
    g_strength = (float)atof(num);
    GetPrivateProfileStringA("video", "ShadowDistance", "100", num, sizeof num, ini);
    g_distance = (float)atof(num);
    g_size = (UINT)GetPrivateProfileIntA("video", "ShadowMapSize", 4096, ini);
    g_use_cache = GetPrivateProfileIntA("video", "ShadowCache", 0, ini) != 0;
    GetPrivateProfileStringA("video", "ShadowCullWiden", "2.0", num, sizeof num, ini);
    g_cull_widen = (float)atof(num);
    GetPrivateProfileStringA("video", "ShadowCullBack", "30", num, sizeof num, ini);
    g_cull_back = (float)atof(num);
    GetPrivateProfileStringA("video", "ShadowCullSun", "25", num, sizeof num, ini);
    g_cull_sun = (float)atof(num);
    GetPrivateProfileStringA("video", "ShadowMinElevation", "50", num, sizeof num, ini);
    g_min_elev = (float)atof(num);
    if (g_size < 512 || g_size > 8192) g_size = 4096;
    g_show = GetPrivateProfileIntA("debug", "ShowShadow", 0, ini);
    ID3D11Device *dev = g_d3d.dev;
    auto compile = [&](const char *entry, const char *target) -> ID3DBlob * {
        ID3DBlob *code = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(k_hlsl, strlen(k_hlsl), "shadow", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
            port_log("shadows: %s compile failed: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
            return nullptr;
        }
        return code;
    };
    ID3DBlob *b0 = compile("cast_ps", "ps_4_0"), *b1 = compile("comp_ps", "ps_4_0"), *b2 = compile("full_vs", "vs_4_0");
    if (!b0 || !b1 || !b2) {
        g_on = false;
        return false;
    }
    dev->CreatePixelShader(b0->GetBufferPointer(), b0->GetBufferSize(), nullptr, &g_ps_cast);
    dev->CreatePixelShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, &g_ps_comp);
    dev->CreateVertexShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &g_vs_full);
    b0->Release(), b1->Release(), b2->Release();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = g_size;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_map))) {
        g_on = false;
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(g_map, &dv, &g_dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(g_map, &sv, &g_srv);

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = VSC * 16;
    dev->CreateBuffer(&bd, nullptr, &g_cb_vs);
    bd.ByteWidth = 16;
    dev->CreateBuffer(&bd, nullptr, &g_cb_ps);
    bd.ByteWidth = 11 * 16;
    dev->CreateBuffer(&bd, nullptr, &g_cb_comp);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;                         // casters behind the light box still cast
    rd.DepthBias = 600;
    rd.SlopeScaledDepthBias = 2.0f;
    rd.DepthBiasClamp = 0.01f;
    dev->CreateRasterizerState(&rd, &g_rs_cast);
    rd.DepthBias = 0, rd.SlopeScaledDepthBias = 0, rd.DepthBiasClamp = 0, rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs_full);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dd, &g_ds_cast);
    dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds_off);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = 0;
    dev->CreateBlendState(&bl, &g_bs_none);
    bl.RenderTarget[0].RenderTargetWriteMask = 15;
    dev->CreateBlendState(&bl, &g_bs_copy);
    bl.RenderTarget[0].BlendEnable = TRUE;               // scene colour *= shadow factor
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = 7;
    dev->CreateBlendState(&bl, &g_bs_mul);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_cmp);
    sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_smp);
    port_log("shadows: %s, map %u, distance %.0f, strength %.2f, min sun elevation %.0f\n", g_d3d.shadows ? "on" : "off (F5)", g_size, g_distance, g_strength, g_min_elev);
    return true;
}

// ------------------------------------------------------------------------------------------------ culling
// The engine only draws what its camera frustum keeps, so scenery just off-screen - a wall behind the camera, the
// Stadium Dreas canopy above the unit camera - cast no shadow into view. Camera_Update (0x1ABD90) builds the
// culling planes with FUN_001AB550 (fastcall cam; one call, 0x1ABDA5) from the camera's position +0x130,
// orientation rows +0x140 / +0x150 / +0x160 (backward - the engine looks down -z), near / far +0x274 / +0x278 and half extents at the near
// plane +0x27C / +0x280 - before it builds the view matrix from the same fields. Here, for that one call, the
// half extents are widened and the apex pulled back along the view direction, then everything is restored: the
// planes keep more of the arena, the projection and view stay the game's own. 3D cameras only (not the 2D menu
// camera s2dCamera 0x3D3050), only while shadows are on.
// Planes (FUN_001AB550 output): cam+0x1C0 and cam+0x200, two blocks of 4 planes stored x[4] y[4] z[4] d[4]; the
// 4th of each block is a dummy (0, 0, 0, 1) that keeps everything. All dummies = no frustum culling.
static int g_no_cull = -1;                              // [debug] NoCull=1

static void planes_keep_all(BYTE *cam)
{
    for (int b = 0; b < 2; b++) {
        float *blk = (float *)(cam + 0x1C0 + b * 0x40);
        for (int i = 0; i < 4; i++)
            blk[i] = blk[4 + i] = blk[8 + i] = 0, blk[12 + i] = 1;
    }
}

static void __fastcall cull_planes(BYTE *cam)
{
    typedef void(__fastcall * PlanesFn)(BYTE *);
    if (g_no_cull < 0) {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        g_no_cull = GetPrivateProfileIntA("debug", "NoCull", 0, ini);
    }
    if (g_no_cull && cam != (BYTE *)0x3D3050) {
        ((PlanesFn)0x1AB550)(cam);
        planes_keep_all(cam);
        return;
    }
    if (!g_d3d.shadows || !g_on || cam == (BYTE *)0x3D3050 || g_cull_widen <= 1.0f && g_cull_back <= 0.0f) {
        ((PlanesFn)0x1AB550)(cam);
        return;
    }
    float *pos = (float *)(cam + 0x130), *fwd = (float *)(cam + 0x160);   // fwd: backward axis
    float *hw = (float *)(cam + 0x27C), *hh = (float *)(cam + 0x280), *fr = (float *)(cam + 0x278);
    float save[6] = {pos[0], pos[1], pos[2], *hw, *hh, *fr};
    // Toward the sun too: casters of visible ground sit up the sun ray from it (tree canopies above the Stadium
    // Dreas wall). Apex moved back by B + S and sunward by S: with the widened extents (x2, half-angle tan >= 0.43)
    // the planes still hold the whole view frustum (as long as S <= 2 tan * B) and its copy shifted S toward the
    // sun - so, convex, every caster within S along the ray from anything visible. Sun from last frame's shadow
    // pass (view space, travel direction) -> world with the camera rows (+0x140 right, +0x150 up, +0x160 back).
    float sun = 0, sw[3] = {};
    if (g_cull_sun > 0 && g_prev_valid && g_d3d.frame - g_prev_frame <= 2) {
        const float *rows[3] = {(float *)(cam + 0x140), (float *)(cam + 0x150), (float *)(cam + 0x160)};
        for (int i = 0; i < 3; i++) {
            const float *r = rows[i];
            float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (l > 1e-6f)
                for (int k = 0; k < 3; k++)
                    sw[k] -= g_prevD[i] * r[k] / l;     // minus: towards the sun
        }
        float l = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
        if (l > 1e-6f) {
            for (float &v : sw) v /= l;
            sun = g_cull_sun;
        }
        static DWORD logged;
        if (g_d3d.frame - logged >= 1800 && (logged = g_d3d.frame, true))
            port_log("shadows: cull toward sun (world) %.2f %.2f %.2f, %.0f units\n", sw[0], sw[1], sw[2], sun);
    }
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (fl > 1e-6f)
        for (int k = 0; k < 3; k++)
            pos[k] += fwd[k] / fl * (g_cull_back + sun) + sw[k] * sun;   // row +0x160 = BACKWARD axis (tested)
    *hw *= g_cull_widen;
    *hh *= g_cull_widen;
    *fr += g_cull_back + 2 * sun;
    ((PlanesFn)0x1AB550)(cam);
    pos[0] = save[0], pos[1] = save[1], pos[2] = save[2];
    *hw = save[3], *hh = save[4], *fr = save[5];
}

void d3d_shadow_install_cull()
{
    BYTE *p = (BYTE *)0x1ABDA5;
    if (p[0] != 0xE8 || 0x1ABDA5 + 5 + *(int32_t *)(p + 1) != 0x1AB550) {
        port_log("shadows: culling-plane call at 1ABDA5 unexpected - not widened\n");
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)cull_planes - (0x1ABDA5 + 5));
}

void d3d_shadow_init() { init(); }
bool d3d_shadows_on() { return init() && g_d3d.shadows; }

// ------------------------------------------------------------------------------------------------ recording
void d3d_shadow_record(ID3D11VertexShader *vs, ID3D11InputLayout *layout, ID3D11Buffer *const bufs[16],
                       const UINT strd[16], const UINT offs[16], D3D11_PRIMITIVE_TOPOLOGY topo,
                       const std::vector<UINT> &indices, const float *vsc, ID3D11ShaderResourceView *tex0, float aref,
                       const char *name, bool dynamic, ID3D11Buffer *ib, DWORD consts_gen)
{
    if (!d3d_shadows_on() || g_done_frame == g_d3d.frame)
        return;
    Cast c = {};
    c.vs = vs;
    c.layout = layout;
    for (int s = 0; s < 16; s++) {
        c.bufs[s] = bufs[s];
        if (bufs[s])
            bufs[s]->AddRef();
        c.strd[s] = strd[s], c.offs[s] = offs[s];
    }
    c.topo = topo;
    c.count = (UINT)indices.size();
    if (ib) {                                           // the draw's own cached index buffer (d3d_program.cpp)
        c.ib = ib;
        ib->AddRef();
        c.first = 0;
    } else {
        c.first = (UINT)g_idx.size();
        g_idx.insert(g_idx.end(), indices.begin(), indices.end());
    }
    {                                                   // identity of the index list (cast_key): its first / last 64
        unsigned long long h = 0xCBF29CE484222325ull;
        UINT n = c.count < 64 ? c.count : 64;
        for (UINT i = 0; i < n; i++)
            h = (h ^ indices[i]) * 0x100000001B3ull;
        for (UINT i = c.count > 64 ? c.count - 64 : n; i < c.count; i++)
            h = (h ^ indices[i]) * 0x100000001B3ull;
        c.idx_hash = h;
    }
    // Runs of draws with constants the game did not touch in between (same consts_gen; the port's per-draw rows c92 /
    // c209 equal) share one stored block: the replay then shares their placement maths and constant window too
    const float *prev = g_consts.empty() ? nullptr : &g_consts[g_last_consts * 4];
    if (prev && consts_gen == g_last_gen && !memcmp(prev + (92 + 96) * 4, vsc + (92 + 96) * 4, 16) &&
        !memcmp(prev + 209 * 4, vsc + 209 * 4, 16)) {
        c.consts = g_last_consts;
    } else {
        c.consts = g_last_consts = (UINT)(g_consts.size() / 4);
        g_last_gen = consts_gen;
        g_consts.insert(g_consts.end(), vsc, vsc + VSC * 4);
    }
    c.tex0 = tex0;
    if (tex0)
        tex0->AddRef();
    c.aref = aref;
    c.name = name;
    c.dynamic = dynamic;
    g_casts.push_back(c);
}

// the light list of a lit draw: the brightest directional light, in view space, becomes this frame's sun
void d3d_shadow_light(const float *vsc, int first, int nlights, int wv)
{
    (void)wv;
    if (!init())
        return;
    auto C = [&](int slot) { return vsc + (slot + 96 < 0 ? 0 : slot + 96 > 191 ? 191 : slot + 96) * 4; };
    int a = (int)floor(C(first)[3] + 0.5f);
    for (int l = 0; l < nlights; l++) {
        a = (int)floor(C(a)[3] + 0.5f);
        const float *col = C(a), *dir = C(a + 1);
        float lum = col[0] * 0.3f + col[1] * 0.59f + col[2] * 0.11f;
        if (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] < 1e-12f)
            continue;
        if (g_sun_frame != g_d3d.frame || lum > g_sun_lum) {   // brightest light of the frame = the sun
            g_sun_frame = g_d3d.frame;
            g_sun_lum = lum;
            memcpy(g_sun_obj, dir, sizeof g_sun_obj);
            wvp_of(vsc, g_sun_wvp);
        }
    }
}

static void clear_casts()
{
    for (Cast &c : g_casts) {
        if (c.ib) c.ib->Release();
        for (ID3D11Buffer *b : c.bufs)
            if (b) b->Release();
        if (c.tex0) c.tex0->Release();
    }
    g_casts.clear();
    g_idx.clear();
    g_consts.clear();
}

void d3d_shadow_frame_end() { clear_casts(); }

// ------------------------------------------------------------------------------------------------ apply
static bool invert4(const float m[16], float out[16])   // general 4x4 inverse (row-major)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f)
        return false;
    for (int i = 0; i < 16; i++)
        out[i] = inv[i] / det;
    return true;
}

bool d3d_invert4(const float m[16], float out[16]) { return invert4(m, out); }

// object -> view of a draw (column-vector WVP) with last frame's projection: P^-1 * WVP
bool d3d_object_to_view(const float wvp[16], float M[16])
{
    float Pi[16];
    if (!g_prev_valid || g_d3d.frame - g_prev_frame > 2 || !invert4(g_prevP, Pi))
        return false;
    mul4(Pi, wvp, M);
    return true;
}

// Arena relight: the sun (travel direction) in a draw's own vertex space: WVP^-1 * P * (sun_view, 0)
bool d3d_sun_in_object(const float wvp[16], float out[3])
{
    if (!g_prev_valid || g_d3d.frame - g_prev_frame > 2)
        return false;
    float Ai[16];
    if (!invert4(wvp, Ai))
        return false;
    float t[4];
    for (int r = 0; r < 4; r++)
        t[r] = g_prevP[r * 4] * g_prevD[0] + g_prevP[r * 4 + 1] * g_prevD[1] + g_prevP[r * 4 + 2] * g_prevD[2];
    float o[3];
    for (int r = 0; r < 3; r++)
        o[r] = Ai[r * 4] * t[0] + Ai[r * 4 + 1] * t[1] + Ai[r * 4 + 2] * t[2] + Ai[r * 4 + 3] * t[3];
    float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    if (l < 1e-9f)
        return false;
    out[0] = o[0] / l, out[1] = o[1] / l, out[2] = o[2] / l;
    return true;
}

// Character shadowing: surface (draw input space) -> last frame's shadow map, for a draw with column-vector WVP.
// The map / matrices are one frame old (characters are drawn before this frame's map) but consistent.
bool d3d_shadow_lookup(const float wvp[16], float S[16], ID3D11ShaderResourceView **srv, ID3D11SamplerState **cmp,
                       float *texel, float *strength)
{
    if (!d3d_shadows_on() || !g_prev_valid || g_d3d.frame - g_prev_frame > 2 || !g_srv)
        return false;
    mul4(g_prevLP, wvp, S);
    *srv = g_srv, *cmp = g_cmp, *texel = 1.0f / g_size, *strength = g_strength;
    return true;
}

// column-vector matrices (row-major storage) from a draw's constants
static void wv_of(const float *vsc, float m[16])
{
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 4; i++)
            m[k * 4 + i] = vsc[(76 + 96 + k) * 4 + i];
}
static void wvp_of(const float *vsc, float m[16])     // stored as rows of the row-vector matrix: transpose
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[j * 4 + i] = vsc[(80 + 96 + i) * 4 + j];
}
static void mul4(const float a[16], const float b[16], float o[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

// the engine projection from a draw: P = WVP * WV^-1 (must look like a perspective matrix)
static bool projection(const float *vsc, float P[16])
{
    float wv[16], wvp[16], inv[16];
    wv_of(vsc, wv);
    wvp_of(vsc, wvp);
    if (!invert4(wv, inv))
        return false;
    mul4(wvp, inv, P);
    return fabsf(P[0]) > 1e-4f && fabsf(P[5]) > 1e-4f && fabsf(P[14]) > 1e-4f && fabsf(P[12]) < 1e-3f &&
           fabsf(P[13]) < 1e-3f;
}


// ------------------------------------------------------------------------------------------------ caster cache
// The game culls what the camera does not see, so a wall just off-screen stopped casting onto the visible ground
// (big shadows popped in and out as the camera / cursor moved - Palace Ibliis, Stadium Dreas). Every caster is
// kept by identity (shader, layout, buffers, its indices) with its object -> world matrix; one whose matrix and
// per-object constants (bones, materials, lights) stayed the same over several frames is static and keeps being
// replayed while off-screen. Animated characters change their bones every frame: never cached. Cleared when 3D
// stops (loading between arenas); entries that never settled go after 30 frames unseen.

struct Cached {
    ID3D11VertexShader *vs;
    ID3D11InputLayout *layout;
    ID3D11Buffer *bufs[16];
    UINT strd[16], offs[16];
    D3D11_PRIMITIVE_TOPOLOGY topo;
    std::vector<UINT> idx;                              // (ib == nullptr)
    ID3D11Buffer *ib;                                   // the index buffer it was drawn with (referenced)
    UINT count;
    std::vector<float> consts;
    ID3D11ShaderResourceView *tex0;
    float aref;
    float M[16];                                        // object -> world
    float Rinv[16];                                     // world <- view when consts were captured
    bool view_bones;                                    // its matrices (bones) already include the camera
    int stable;                                         // frames in a row unchanged
    DWORD seen;
    std::string name;                                   // texture (diagnostics)
    int changes;                                        // times it changed (diagnostics)
};
static std::unordered_map<unsigned long long, Cached> g_cache;
static DWORD g_last_apply;
static float g_sun_world[3], g_up_world[3];
static bool g_have_world_sun;
struct Replay {                                         // one draw into the shadow map
    ID3D11VertexShader *const *vs;
    ID3D11InputLayout *const *layout;
    ID3D11Buffer *const *bufs;
    const UINT *strd, *offs;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    UINT first, count;
    const float *consts;
    ID3D11ShaderResourceView *tex0;
    float aref;
    ID3D11Buffer *ib;                                   // nullptr: the per-frame g_ib
    float ov[16];                                       // object -> view
    Cached *cached;
};

static void cached_release(Cached &e)
{
    if (e.ib) e.ib->Release();
    for (ID3D11Buffer *b : e.bufs)
        if (b) b->Release();
    if (e.tex0) e.tex0->Release();
}

static void cache_clear()
{
    for (auto &kv : g_cache)
        cached_release(kv.second);
    g_cache.clear();
    g_have_world_sun = false;
}

static unsigned long long cast_key(const Cast &c)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    auto mix = [&](unsigned long long v) { h = (h ^ v) * 0x100000001B3ull; };
    mix((unsigned long long)(uintptr_t)c.vs);
    mix((unsigned long long)(uintptr_t)c.layout);
    for (int s = 0; s < 16; s++)
        mix((unsigned long long)(uintptr_t)c.bufs[s] ^ ((unsigned long long)c.offs[s] << 32));
    mix(c.count);
    mix(c.idx_hash);
    return h;
}

static void dir_xform(const float m[16], const float v[3], float out[3])   // direction through a matrix, normalised
{
    for (int k = 0; k < 3; k++)
        out[k] = m[k * 4] * v[0] + m[k * 4 + 1] * v[1] + m[k * 4 + 2] * v[2];
    float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 1e-9f)
        for (int k = 0; k < 3; k++)
            out[k] /= l;
}

void d3d_shadow_apply()
{
    if (g_done_frame == g_d3d.frame)
        return;
    g_done_frame = g_d3d.frame;
    if (!d3d_shadows_on() || g_casts.empty() || !g_d3d.gbuf_srv)
        return;
    prof_gpu_pass(GP_SHADOW);
    if (g_d3d.frame - g_last_apply > 60)                // 3D stopped for a while (loading, menus): new arena
        cache_clear();
    g_last_apply = g_d3d.frame;
    // The scene projection = the one most casters were drawn with (the arena's draws all share WV and WVP; a
    // skinned character's WV is its world-view while its bones already output world space, so its own
    // WVP * WV^-1 is not a projection - those are only voters for other, minor groups).
    float P[16];
    std::map<long long, std::pair<int, size_t>> votes;  // quantised projection -> (count, a cast using it)
    std::vector<long long> key(g_casts.size(), 0);
    for (size_t k = 0; k < g_casts.size(); k++) {
        if (k && g_casts[k].consts == g_casts[k - 1].consts) {   // shared block: same projection
            key[k] = key[k - 1];
            if (key[k] != -1)
                votes[key[k]].first++;
            continue;
        }
        float pk[16];
        if (!projection(&g_consts[g_casts[k].consts * 4], pk)) {
            key[k] = -1;
            continue;
        }
        long long q = (long long)lroundf(pk[0] * 500) * 1000003LL * 1000003LL + (long long)lroundf(pk[10] * 500) * 1000003LL +
                      lroundf(pk[11] * 50);
        key[k] = q;
        auto &v = votes[q];
        if (!v.first++)
            v.second = k;
    }
    if (votes.empty())
        return;
    long long main_key = 0;
    int best = -1;
    for (auto &kv : votes)
        if (kv.second.first > best)
            best = kv.second.first, main_key = kv.first;
    projection(&g_consts[g_casts[votes[main_key].second].consts * 4], P);
    float Pinv[16];
    if (!invert4(P, Pinv))
        return;
    // Reference frame R (world -> view), taken from this frame's own draws so it matches what was drawn exactly:
    // each draw's placement is its object -> view (P^-1 * WVP), or - when that is the identity - its first bone
    // matrix (Palace Ibliis' arena bakes the camera into its bone rows; AltahrunRuins' uses WV = view). The arena's
    // static pieces all sit in the arena frame, so the most common placement is the view matrix. Fallback: the
    // engine camera's view (cam_activecam 0x4B2808 +0xB0, row-vector storage), which can be a frame off.
    float R[16], Rinv[16];
    bool have_r = false;
    {
        std::map<long long, std::pair<int, size_t>> modes;
        std::vector<float> place(g_casts.size() * 16);
        size_t last_k = (size_t)-1;                     // previous voter (shared constant blocks vote alike)
        long long last_h = 0;
        for (size_t k = 0; k < g_casts.size(); k++) {
            // characters do not vote: each is drawn as many parts sharing its own placement, and with a few units on
            // screen in an arena made of few big draws (Stadium Dreas) a fighter's frame won - cached scenery was
            // then replayed relative to it (tree shadows losing chunks, a building's shadow collapsing to a stripe)
            if (g_casts[k].dynamic)
                continue;
            if (last_k != (size_t)-1 && g_casts[k].consts == g_casts[last_k].consts) {
                memcpy(&place[k * 16], &place[last_k * 16], 64);
                modes[last_h].first++;
                last_k = k;
                continue;
            }
            const float *src = &g_consts[g_casts[k].consts * 4];
            float wvp[16], ov[16], *q = &place[k * 16];
            wvp_of(src, wvp);
            mul4(Pinv, wvp, ov);
            bool vb = true;
            for (int i = 0; i < 16 && vb; i++)
                vb = fabsf(ov[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-3f;
            if (vb) {
                memcpy(q, src, 48);                     // bone 0 rows (Xbox c-96..c-94)
                q[12] = q[13] = q[14] = 0, q[15] = 1;
            } else {
                memcpy(q, ov, 64);
            }
            long long h = 0;
            for (int i = 0; i < 12; i++)
                h = h * 1000003LL + lroundf(q[i] * 1000);
            auto &v = modes[h];
            if (!v.first++)
                v.second = k;
            last_k = k, last_h = h;
        }
        int bc = 0;
        size_t bk = 0;
        for (auto &kv : modes)
            if (kv.second.first > bc)
                bc = kv.second.first, bk = kv.second.second;
        if (bc >= 20) {
            memcpy(R, &place[bk * 16], sizeof R);
            have_r = invert4(R, Rinv);
        }
    }
    if (!have_r)
        if (const float *cam = *(const float *const *)0x4B2808) {
            const float *vb = cam + 0xB0 / 4;
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    R[j * 4 + i] = vb[i * 4 + j];
            float rr = R[0] * R[0] + R[1] * R[1] + R[2] * R[2];
            have_r = rr > 0.9f && rr < 1.1f && fabsf(R[15] - 1) < 1e-3f && invert4(R, Rinv);
        }
    if (have_r) {                                       // for TAA reprojection (d3d_taa.cpp)
        memcpy(g_cam_P, P, sizeof g_cam_P);
        memcpy(g_cam_R, R, sizeof g_cam_R);
        g_cam_frame = g_d3d.frame;
    }
    // the sun and world up: from this frame's brightest lit draw if any, kept in world space for the frames without
    if (g_sun_frame == g_d3d.frame) {
        float M[16];
        mul4(Pinv, g_sun_wvp, M);
        float v[3], bestu = 0;
        for (int k = 0; k < 3; k++)
            v[k] = M[k * 4] * g_sun_obj[0] + M[k * 4 + 1] * g_sun_obj[1] + M[k * 4 + 2] * g_sun_obj[2];
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) {
            for (int k = 0; k < 3; k++)
                g_sun[k] = v[k] / len;
            for (int ax = 0; ax < 3; ax++) {            // input-space axis most aligned with screen-up = world up
                float a3[3] = {M[ax], M[4 + ax], M[8 + ax]};
                float al = sqrtf(a3[0] * a3[0] + a3[1] * a3[1] + a3[2] * a3[2]);
                if (al < 1e-6f || fabsf(a3[1]) / al <= bestu)
                    continue;
                bestu = fabsf(a3[1]) / al;
                float sg = a3[1] < 0 ? -1.0f : 1.0f;
                for (int k = 0; k < 3; k++)
                    g_up[k] = sg * a3[k] / al;
            }
            if (have_r) {                               // remember them in world space
                dir_xform(Rinv, g_sun, g_sun_world);
                dir_xform(Rinv, g_up, g_up_world);
                g_have_world_sun = true;
                static float last[3];                   // [diagnostic] world sun changes (> ~3 degrees)
                static int logs;
                float d = last[0] * g_sun_world[0] + last[1] * g_sun_world[1] + last[2] * g_sun_world[2];
                if (d < 0.9986f && logs < 300) {
                    logs++;
                    port_log("sun: frame %u world %.3f %.3f %.3f (lum %.2f, lit wvp00 %.3f), was %.3f %.3f %.3f\n",
                             g_d3d.frame, g_sun_world[0], g_sun_world[1], g_sun_world[2], g_sun_lum, g_sun_wvp[0],
                             last[0], last[1], last[2]);
                    memcpy(last, g_sun_world, sizeof last);
                }
            }
        }
    } else if (have_r && g_have_world_sun) {
        dir_xform(R, g_sun_world, g_sun);
        dir_xform(R, g_up_world, g_up);
    } else {
        return;                                         // no sun known yet
    }
    {   // [diagnostic] Stadium Dreas canopy: cookie / bark casters per frame, logged when the set changes
        static UINT last_ck = 0xFFFFFFFF, last_bk;
        static int logs;
        UINT ck = 0, bk = 0, cn = 0;
        for (const Cast &c : g_casts) {
            if (strstr(c.name, "cookie")) ck += c.count, cn++;
            if (strstr(c.name, "bark")) bk += c.count;
        }
        if ((ck != last_ck || bk != last_bk) && logs < 200) {
            logs++;
            port_log("canopy: frame %u cookie %u indices (%u draws), bark %u, casters %u\n", g_d3d.frame, ck, cn, bk,
                     (unsigned)g_casts.size());
            last_ck = ck, last_bk = bk;
        }
    }
    // ---- static caster cache: walls / columns / statues culled by the game when off-screen keep casting
    std::vector<Replay> items;
    items.reserve(g_casts.size() + g_cache.size());
    std::unordered_map<unsigned long long, int> frame_pass;   // key -> draws of it so far this frame
    unsigned hits = 0;
    static int keydiag;
    for (size_t k = 0; k < g_casts.size(); k++) {
        const Cast &c = g_casts[k];
        const float *src = &g_consts[c.consts * 4];
        Replay r = {&c.vs, &c.layout, c.bufs, c.strd, c.offs, c.topo, c.first, c.count, src, c.tex0, c.aref, c.ib};
        if (k && c.consts == g_casts[k - 1].consts) {
            memcpy(r.ov, items.back().ov, sizeof r.ov); // shared block: same placement
        } else {
            float wvp[16];
            wvp_of(src, wvp);
            mul4(Pinv, wvp, r.ov);                      // object -> view
        }
        items.push_back(r);
        // Characters / units (the engine's lit shaders) are never cached: one that stood still for a moment and then
        // walked off left its shadow behind at the old spot for the whole keep time (world map: a trail of them).
        if (!g_use_cache || !have_r || c.dynamic)
            continue;
        float M[16];
        mul4(Rinv, r.ov, M);                            // object -> world
        // Meshes whose object -> view is the identity carry the camera in their bone rows (dp4 rows of 3 from
        // c-96): their placement is in the bones, M is just the inverse camera
        bool view_bones = true;
        for (int i = 0; i < 16 && view_bones; i++)
            view_bones = fabsf(r.ov[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-3f;
        float t[3] = {M[3], M[7], M[11]};               // where it stands, in world space
        if (view_bones)
            for (int k = 0; k < 3; k++)                 // first bone's origin: Rinv * (bone row translations, 1)
                t[k] = Rinv[k * 4] * src[3] + Rinv[k * 4 + 1] * src[7] + Rinv[k * 4 + 2] * src[11] + Rinv[k * 4 + 3];
        // + where it stands: instances of one prop (a row of identical columns) share buffers and indices
        unsigned long long h = cast_key(c) ^
                               ((unsigned long long)lroundf(t[0] * 20) * 0x9E3779B97F4A7C15ull) ^
                               ((unsigned long long)lroundf(t[1] * 20) * 0xC2B2AE3D27D4EB4Full) ^
                               ((unsigned long long)lroundf(t[2] * 20) * 0x165667B19E3779F9ull);
        // + which pass: the same mesh at the same spot drawn again this frame with other constants (multi-pass
        // materials - Palace Ibliis white_marble3) gets an entry per pass instead of the passes overwriting each other
        int pass = frame_pass[h]++;
        h ^= (unsigned long long)pass * 0xD6E8FEB86659FD93ull;
        auto it = g_cache.find(h);
        hits += it != g_cache.end();
        if (!strcmp(c.name, "bluetile3b") && keydiag < 12 && g_d3d.frame > 3500) {   // [diagnostic] key ingredients
            keydiag++;
            port_log("keydiag frame %u: base %016llx t %.3f %.3f %.3f pass %d vb %d count %u buf0 %p off0 %u -> %s\n",
                     g_d3d.frame, cast_key(c), t[0], t[1], t[2], pass, (int)view_bones, c.count,
                     (void *)c.bufs[0], c.offs[0], it != g_cache.end() ? "hit" : "new");
        }
        if (it == g_cache.end()) {
            Cached &e = g_cache[h];
            e.vs = c.vs, e.layout = c.layout, e.topo = c.topo, e.tex0 = c.tex0, e.aref = c.aref;
            for (int s = 0; s < 16; s++) {
                e.bufs[s] = c.bufs[s];
                if (e.bufs[s]) e.bufs[s]->AddRef();
                e.strd[s] = c.strd[s], e.offs[s] = c.offs[s];
            }
            if (e.tex0) e.tex0->AddRef();
            e.count = c.count;
            e.ib = c.ib;
            if (e.ib)
                e.ib->AddRef();
            else
                e.idx.assign(&g_idx[c.first], &g_idx[c.first] + c.count);
            e.name = c.name;
            e.changes = 0;
            e.consts.assign(src, src + VSC * 4);
            memcpy(e.M, M, sizeof M);
            memcpy(e.Rinv, Rinv, sizeof Rinv);
            e.stable = 0;
            e.seen = g_d3d.frame;
            continue;
        }
        Cached &e = it->second;
        // Unchanged = same object -> world and same per-object constants (bones, materials, lights); view_bones
        // meshes: bone rows compared after taking the camera out (row-block * view^-1, old and new view each),
        // M (= the inverse camera for them) not at all.
        bool same = true;
        float worst = 0;
        int qmax = view_bones ? 3 : 0;                  // bone 0 rows (Xbox c-96..c-94): its placement
        for (int q = 0; q + 2 < qmax && same; q += 3) {
            const float *o = &e.consts[q * 4], *n = src + q * 4;
            if (!memcmp(o, n, 48))                      // untouched rows (unused / constant): equal as they are
                continue;
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) {
                    float wo = 0, wn = 0;
                    // rows map object -> view: view.r = dot(row, p). world = Rinv * view, so the 3 rows of a bone
                    // combine: world_row_c = sum_r Rinv[c][r] * row_r (+ Rinv[c][3] for the constant part)
                    for (int rr = 0; rr < 3; rr++) {
                        wo += e.Rinv[row * 4 + rr] * o[rr * 4 + col];
                        wn += Rinv[row * 4 + rr] * n[rr * 4 + col];
                    }
                    if (col == 3) wo += e.Rinv[row * 4 + 3], wn += Rinv[row * 4 + 3];
                    float d = fabsf(wo - wn) / (1 + fabsf(wn));
                    worst = d > worst ? d : worst;
                }
            same = worst <= 1e-3f;
        }
        // Other meshes: placement (M) decides. Their "bone" slots are not compared - the constant block is global, so
        // slots a static mesh's shader never reads still hold the last skinned character's bones (they changed every
        // frame while the mesh did not). The light block / materials (c39..) do not move geometry either.
        for (int q = 0; q < 0 && same; q++) {
            float d = fabsf(e.consts[q] - src[q]) / (1 + fabsf(src[q]));
            worst = d > worst ? d : worst;
            same = worst <= 1e-3f;
        }
        bool msame = true;
        for (int i = 0; i < 16 && msame && !view_bones; i++)
            msame = fabsf(e.M[i] - M[i]) <= 1e-3f * (1 + fabsf(M[i]));
        static int diag;                                // [diagnostic] what changes between frames
        if ((!same || !msame) && diag < 40) {
            diag++;
            std::string slots;
            int nd = 0;
            for (int q = 0; q < 76 + 96; q++)
                if (memcmp(&e.consts[q * 4], src + q * 4, 16)) {
                    if (nd++ < 12) slots += " c" + std::to_string(q - 96);
                }
            port_log("shadow cache: '%s' changed: consts %d slots (%s), M %s\n", c.name, nd, slots.c_str(),
                     msame ? "same" : "moved");
        }
        same = same && msame;
        if (same) {
            if (e.stable < 1000) e.stable++;
        } else {
            e.stable = 0;
            e.changes++;
            memcpy(e.M, M, sizeof M);
            memcpy(e.Rinv, Rinv, sizeof Rinv);
            memcpy(e.consts.data(), src, VSC * 16);
        }
        e.seen = g_d3d.frame;
    }
    // static entries not drawn this frame (off-screen): replay at their world placement
    UINT extra = 0;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        Cached &e = it->second;
        // Static scenery stays for the whole battle (cleared when 3D stops - a new arena): with a time limit the
        // Stadium Dreas canopy over the field, off-screen in the close unit camera for a while, lost its shadows.
        // Entries that never settled (moving / one-off draws) go after 30 frames unseen.
        if (e.stable < 3 && g_d3d.frame - e.seen > 30u) {
            cached_release(e);
            it = g_cache.erase(it);
            continue;
        }
        if (g_use_cache && have_r && e.seen != g_d3d.frame && e.stable >= 3) {
            Replay r = {&e.vs, &e.layout, e.bufs, e.strd, e.offs, e.topo, 0, e.count, e.consts.data(),
                        e.tex0, e.aref, e.ib};
            mul4(R, e.M, r.ov);
            r.cached = &e;
            items.push_back(r);
            if (!e.ib)
                extra += e.count;
        }
        ++it;
    }
    if (g_d3d.frame % 900 == 0) {
        port_log("shadows: frame %u, %u drawn + %u cached casters (%u in cache)%s\n", g_d3d.frame,
                 (unsigned)g_casts.size(), (unsigned)(items.size() - g_casts.size()), (unsigned)g_cache.size(),
                 have_r ? "" : ", no world reference this frame");
        // [diagnostic] cache health: stable vs changing, off-screen ones, which textures keep changing
        unsigned st = 0, off = 0, off_st = 0;
        std::map<std::string, int> churn;
        for (auto &kv : g_cache) {
            const Cached &e = kv.second;
            st += e.stable >= 3;
            if (e.seen != g_d3d.frame) {
                off++;
                off_st += e.stable >= 3;
            }
            if (e.stable < 3)
                churn[e.name]++;
        }
        std::string top;
        int shown = 0;
        for (auto &kv : churn)
            if (kv.second > 3 && shown++ < 10)
                top += " " + kv.first + "x" + std::to_string(kv.second);
        port_log("shadow cache: %u stable of %u, off-screen %u (stable %u), %u of %u draws found their entry; "
                 "unstable by texture:%s\n", st, (unsigned)g_cache.size(), off, off_st, hits, (unsigned)g_casts.size(),
                 top.c_str());
    }
    // view-space frustum slice [near, distance] -> light-space box. View point at clip w: z from P row 3.
    float D[3] = {g_sun[0], g_sun[1], g_sun[2]};
    {
        float down = -(D[0] * g_up[0] + D[1] * g_up[1] + D[2] * g_up[2]);   // sin(elevation)
        float smin = sinf(g_min_elev * 3.14159265f / 180.0f);
        if (down < smin) {
            float h[3] = {D[0] + down * g_up[0], D[1] + down * g_up[1], D[2] + down * g_up[2]};   // horizontal part
            float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            float c = sqrtf(1 - smin * smin);
            for (int k = 0; k < 3; k++)
                D[k] = (hl > 1e-6f ? h[k] / hl * c : 0) - g_up[k] * smin;
        }
    }
    // Stable shadow map (no swimming / jumping while the camera moves - Theatre Antiquitis): light axes from WORLD up
    // (g_up, view space) so the map does not roll with the camera; the visible slice [1, distance] enclosed by a sphere
    // so the map's size does not change as the camera turns; its centre snapped to whole map texels measured from the
    // world origin (R), so the texel grid stays put in the world and edges do not crawl.
    float up[3] = {g_up[0], g_up[1], g_up[2]};
    if (fabsf(D[0] * up[0] + D[1] * up[1] + D[2] * up[2]) > 0.95f) up[0] = 1, up[1] = 0, up[2] = 0;
    float U[3] = {up[1] * D[2] - up[2] * D[1], up[2] * D[0] - up[0] * D[2], up[0] * D[1] - up[1] * D[0]};
    float ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    for (float &u : U) u /= ul;
    float W[3] = {D[1] * U[2] - D[2] * U[1], D[2] * U[0] - D[0] * U[2], D[0] * U[1] - D[1] * U[0]};
    float corner[8][3], ctr[3] = {};
    int nc = 0;
    for (float w : {1.0f, g_distance})
        for (float nx : {-1.0f, 1.0f})
            for (float ny : {-1.0f, 1.0f}) {
                float z = (w - P[15]) / P[14];
                corner[nc][0] = (nx * w - P[2] * z - P[3]) / P[0], corner[nc][1] = (ny * w - P[6] * z - P[7]) / P[5];
                corner[nc][2] = z;
                for (int k = 0; k < 3; k++)
                    ctr[k] += corner[nc][k] / 8;
                nc++;
            }
    float rad = 0;
    for (int c = 0; c < 8; c++) {
        float dd = 0;
        for (int k = 0; k < 3; k++)
            dd += (corner[c][k] - ctr[k]) * (corner[c][k] - ctr[k]);
        rad = dd > rad ? dd : rad;
    }
    rad = ceilf(sqrtf(rad) + 2.0f);                     // + margin for the PCF taps; whole units: steady size
    float q[3] = {U[0] * ctr[0] + U[1] * ctr[1] + U[2] * ctr[2], W[0] * ctr[0] + W[1] * ctr[1] + W[2] * ctr[2],
                  D[0] * ctr[0] + D[1] * ctr[1] + D[2] * ctr[2]};
    if (have_r) {                                       // snap to texels relative to the world origin (view: R col 3)
        float o[3] = {R[3], R[7], R[11]};
        float oq[2] = {U[0] * o[0] + U[1] * o[1] + U[2] * o[2], W[0] * o[0] + W[1] * o[1] + W[2] * o[2]};
        float texel = 2 * rad / g_size;
        for (int k = 0; k < 2; k++)
            q[k] = oq[k] + floorf((q[k] - oq[k]) / texel + 0.5f) * texel;
    }
    float lo[3] = {q[0] - rad, q[1] - rad, q[2] - rad - 150}, hi[3] = {q[0] + rad, q[1] + rad, q[2] + rad};
    // (lo z - 150: casters between the sun and the visible area. No fade at the map border: the sphere holds the
    // whole visible slice.)
    float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    float L[16] = {2 * U[0] / dx, 2 * U[1] / dx, 2 * U[2] / dx, -(hi[0] + lo[0]) / dx,
                   2 * W[0] / dy, 2 * W[1] / dy, 2 * W[2] / dy, -(hi[1] + lo[1]) / dy,
                   D[0] / dz,     D[1] / dz,     D[2] / dz,     -lo[2] / dz,
                   0, 0, 0, 1};

    ID3D11DeviceContext *ctx = g_d3d.ctx;
    ID3D11ShaderResourceView *none[4] = {};
    ctx->PSSetShaderResources(0, 4, none);
    ctx->PSSetShaderResources(5, 1, none);              // lit draws sampled the map at t5
    {
        float LP[16];
        mul4(L, Pinv, LP);
        memcpy(g_prevLP, LP, sizeof LP);                // for the characters of the next frame (drawn before this)
        memcpy(g_prevP, P, sizeof g_prevP);
        g_prevD[0] = D[0], g_prevD[1] = D[1], g_prevD[2] = D[2];
        g_prev_valid = true;
        g_prev_frame = g_d3d.frame;
    }
    ID3D11RenderTargetView *no_rt = nullptr;
    ctx->OMSetRenderTargets(1, &no_rt, g_dsv);
    ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_size, (float)g_size, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs_cast);
    ctx->OMSetDepthStencilState(g_ds_cast, 0);
    float bf[4] = {};
    ctx->OMSetBlendState(g_bs_none, bf, 0xFFFFFFFF);
    ctx->PSSetShader(g_ps_cast, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &g_smp);
    // indices: this frame's casters, then the cached ones (placed after them)
    UINT total = (UINT)g_idx.size() + extra;
    UINT bytes = total * 4;
    if (!g_ib || g_ib_cap < bytes) {
        if (g_ib) g_ib->Release();
        g_ib_cap = bytes + bytes / 2 + 65536;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = g_ib_cap;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        g_d3d.dev->CreateBuffer(&bd, nullptr, &g_ib);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, g_idx.data(), g_idx.size() * 4);
    UINT at = (UINT)g_idx.size();
    for (Replay &r : items)
        if (r.cached && !r.ib) {
            memcpy((UINT *)m.pData + at, r.cached->idx.data(), r.count * 4);
            r.first = at;
            at += r.count;
        }
    ctx->Unmap(g_ib, 0);
    // Constants: every caster's block written into one big buffer with a single Map, each draw binding its own
    // window (D3D11.1 constant buffer offsets) - a Map + 3.4 KB per caster was most of the shadow pass's CPU cost
    // on the world map (~3000 casters, 2026-10-10). Binds are only issued when they change.
    static ID3D11DeviceContext1 *ctx1;
    static int offsets_ok = -1;
    if (offsets_ok < 0) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
        offsets_ok = SUCCEEDED(g_d3d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)) &&
                     o.ConstantBufferOffsetting &&
                     SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1));
        port_log("shadows: constant buffer offsets %s\n", offsets_ok ? "on" : "off (one Map per caster)");
    }
    const UINT SLOT = 224;                              // float4s per caster window (multiple of 16, >= VSC)
    static ID3D11Buffer *big;
    static UINT big_slots;
    float *bigp = nullptr;
    if (offsets_ok && !items.empty()) {
        UINT need = (UINT)items.size();
        if (!big || big_slots < need) {
            if (big) big->Release();
            big = nullptr;
            big_slots = need + need / 2 + 64;
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = big_slots * SLOT * 16;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_d3d.dev->CreateBuffer(&bd, nullptr, &big)))
                big = nullptr, offsets_ok = 0;
        }
        if (big && SUCCEEDED(ctx->Map(big, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            bigp = (float *)m.pData;
    }
    auto fill = [&](const Replay &c, float *dst) {
        float LW[16];
        mul4(L, c.ov, LW);                              // object -> light clip (column-vector)
        memcpy(dst, c.consts, VSC * 16);
        for (int i = 0; i < 4; i++)                     // WVP rows (row-vector): row i = column i of LW
            for (int j = 0; j < 4; j++)
                dst[(80 + 96 + i) * 4 + j] = LW[j * 4 + i];
        float s = (float)g_size;                         // screen-space programs: clip -> "pixels" -> our NDC
        dst[(94 + 96) * 4 + 0] = s / 2, dst[(94 + 96) * 4 + 1] = -s / 2;
        dst[(95 + 96) * 4 + 0] = s / 2, dst[(95 + 96) * 4 + 1] = s / 2;
        dst[208 * 4 + 0] = s, dst[208 * 4 + 1] = s;
        dst[208 * 4 + 2] = dst[208 * 4 + 3] = 0;        // no TAA jitter in the shadow map
    };
    static std::vector<UINT> win;                       // constant window per item (shared blocks share one)
    win.resize(items.size());
    UINT windows = 0;
    if (bigp) {
        for (size_t i = 0; i < items.size(); i++) {
            if (i && items[i].consts == items[i - 1].consts && !memcmp(items[i].ov, items[i - 1].ov, 64)) {
                win[i] = win[i - 1];
                continue;
            }
            win[i] = windows++;
            fill(items[i], bigp + win[i] * SLOT * 4);
        }
        ctx->Unmap(big, 0);
    }
    if (g_d3d.frame % 900 == 0)
        port_log("shadows: %u casters, %u constant blocks recorded, %u windows\n", (unsigned)items.size(),
                 (unsigned)(g_consts.size() / (VSC * 4)), windows);
    ID3D11InputLayout *b_layout = nullptr;
    ID3D11VertexShader *b_vs = nullptr;
    ID3D11Buffer *b_ib = nullptr, *const *b_bufs = nullptr;
    const UINT *b_strd = nullptr, *b_offs = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY b_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11ShaderResourceView *b_tex = (ID3D11ShaderResourceView *)1;
    float b_pc[4] = {-1, -1, 0, 0};
    if (!bigp)
        ctx->VSSetConstantBuffers(0, 1, &g_cb_vs);
    ctx->PSSetConstantBuffers(1, 1, &g_cb_ps);
    for (size_t i = 0; i < items.size(); i++) {
        const Replay &c = items[i];
        if (bigp) {
            if (!i || win[i] != win[i - 1]) {
                UINT first = win[i] * SLOT, num = SLOT;
                ctx1->VSSetConstantBuffers1(0, 1, &big, &first, &num);
            }
        } else {
            if (FAILED(ctx->Map(g_cb_vs, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                continue;
            fill(c, (float *)m.pData);
            ctx->Unmap(g_cb_vs, 0);
        }
        float pc[4] = {c.tex0 ? 1.0f : 0.0f, c.aref, 0, 0};
        if (memcmp(pc, b_pc, sizeof pc) && SUCCEEDED(ctx->Map(g_cb_ps, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, pc, sizeof pc);
            ctx->Unmap(g_cb_ps, 0);
            memcpy(b_pc, pc, sizeof pc);
        }
        if (*c.layout != b_layout)
            ctx->IASetInputLayout(b_layout = *c.layout);
        if (c.bufs != b_bufs && (!b_bufs || memcmp(c.bufs, b_bufs, 16 * sizeof(void *)) ||
                                 memcmp(c.strd, b_strd, 64) || memcmp(c.offs, b_offs, 64))) {
            ctx->IASetVertexBuffers(0, 16, c.bufs, c.strd, c.offs);
            b_bufs = c.bufs, b_strd = c.strd, b_offs = c.offs;
        }
        if (c.topo != b_topo)
            ctx->IASetPrimitiveTopology(b_topo = c.topo);
        if (*c.vs != b_vs)
            ctx->VSSetShader(b_vs = *c.vs, nullptr, 0);
        if (c.tex0 != b_tex)
            ctx->PSSetShaderResources(0, 1, &(b_tex = c.tex0));
        ID3D11Buffer *ib = c.ib ? c.ib : g_ib;
        if (ib != b_ib)
            ctx->IASetIndexBuffer(b_ib = ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(c.count, c.first, 0);
    }
    if (bigp)
        ctx->VSSetConstantBuffers(0, 1, &g_cb_vs);      // plain binding again for whoever binds slot 0 next
    ctx->PSSetShaderResources(0, 1, none);
    // composite: scene colour *= 1 - shadow
    float cc[44] = {};
    memcpy(cc, P, 64);
    memcpy(cc + 16, L, 64);
    cc[32] = D[0], cc[33] = D[1], cc[34] = D[2], cc[35] = g_strength;
    cc[36] = (float)g_d3d.scene_width, cc[37] = (float)g_d3d.scene_height, cc[38] = 1.0f / g_size, cc[39] = (float)g_show;
    // receiver bias: 0.08 game units + 2 shadow texels (coarse texels on big views - the world map - striped flat
    // ground otherwise) + 0.25 x slope
    cc[40] = (0.08f + 2.0f * (2.0f * rad / g_size)) / dz, cc[41] = 0.25f / dz;
    cc[42] = g_distance;
    if (SUCCEEDED(ctx->Map(g_cb_comp, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, cc, sizeof cc);
        ctx->Unmap(g_cb_comp, 0);
    }
    ctx->OMSetRenderTargets(1, &g_d3d.rtv, nullptr);
    D3D11_VIEWPORT sv = {0, 0, (float)g_d3d.scene_width, (float)g_d3d.scene_height, 0, 1};
    ctx->RSSetViewports(1, &sv);
    ctx->RSSetState(g_rs_full);
    ctx->OMSetDepthStencilState(g_ds_off, 0);
    ctx->OMSetBlendState(g_show ? g_bs_copy : g_bs_mul, bf, 0xFFFFFFFF);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs_full, nullptr, 0);
    ctx->PSSetShader(g_ps_comp, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb_comp);
    ID3D11ShaderResourceView *srv[3] = {g_d3d.gbuf_srv, g_srv, g_d3d.gnrm_srv};
    ctx->PSSetShaderResources(0, 3, srv);
    ID3D11SamplerState *smp[2] = {g_smp, g_cmp};
    ctx->PSSetSamplers(0, 2, smp);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 3, none);
    ctx->OMSetRenderTargets(1, &no_rt, nullptr);
    d3d_state_invalidate();
}
