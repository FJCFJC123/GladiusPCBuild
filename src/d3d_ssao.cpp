// Screen-space ambient occlusion ([video] SSAO=1, SSAORadius, SSAOStrength; [debug] ShowAO=1). Runs once a frame when
// the engine enters its 3D particle renderer, so the opaque scene is complete and particles / 2D stay undarkened.
// Half-resolution AO from the G-buffer depth (12 spiral samples), depth-aware blur, multiplied into the scene colour.
#include "d3d.h"
#include "d3d_prof.h"
#include <d3dcompiler.h>
#include <math.h>

static bool g_init, g_failed, g_on;
static float g_radius = 0.8f, g_strength = 1.0f;
static int g_show;

void d3d_ssao_set_show(int mode)                        // debug key 0: 1 AO, 2 AO before the blur, 3 the noise
{
    g_show = mode;
    port_log("ssao: debug view %d (key 0)\n", mode);
}
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_ao, *g_ps_blur, *g_ps_comp;
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_lin, *g_pt;
static ID3D11BlendState *g_bs_mul, *g_bs_copy;
static ID3D11DepthStencilState *g_ds_off;
static ID3D11RasterizerState *g_rs;
static ID3D11Texture2D *g_tex[2];
static ID3D11RenderTargetView *g_rtv[2];
static ID3D11ShaderResourceView *g_srv[2];
static UINT g_w, g_h;
static DWORD g_done_frame = 0xFFFFFFFF;

static const char *k_hlsl = R"(
Texture2D<float> depthTex : register(t0);     // G-buffer view depth (full scene resolution)
Texture2D aoTex : register(t1);               // AO (half resolution)
Texture2D gnorm : register(t2);               // G-buffer normals (engine view space, -z forward), alpha 1 = present
SamplerState lin : register(s0);
SamplerState pt : register(s1);
cbuffer AOCB : register(b0) {
    float4 size;      // full w, h, half w, h
    float4 proj;      // tan half fov x, y, radius, strength
    float4 dir;       // blur direction (half-res texels), show
};
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// The depth texel a uv reads. The AO / blur passes run at half resolution: their pixel centres land EXACTLY on a
// boundary between two full-resolution rows / columns (2j + 1.0), and float rounding put that a hair below or above
// depending on j's binary magnitude - one band of rows (about 25..51 % of the screen, rows 256..511) read the row
// below while neighbours read the right one: false occlusion in a fixed grid, the blur rejecting its neighbours -
// the horizontal bars. A quarter texel past the boundary makes every lookup agree.
int2 texel_of(float2 uv) { return int2(clamp(uv, 0, 0.9999) * size.xy + 0.25); }
float depth_at(float2 uv) { return abs(depthTex.Load(int3(texel_of(uv), 0))); }
// the centre of that texel: positions are built from the same point the depth belongs to
float2 texel_uv(float2 uv) { return (float2(texel_of(uv)) + 0.5) / size.xy; }
float3 view_pos(float2 uv, float d) { uv = texel_uv(uv); return float3((uv * 2 - 1) * float2(proj.x, -proj.y) * d, d); }
float ao(V i) : SV_Target
{
    float d = depth_at(i.uv);
    if (d > 60000 || d <= 0) return 1;
    float3 P = view_pos(i.uv, d);
    float2 px = 1.0 / size.xy;                 // normal: the flatter side of each axis (no halo at edges)
    float3 Pr = view_pos(i.uv + float2(px.x, 0), depth_at(i.uv + float2(px.x, 0)));
    float3 Pl = view_pos(i.uv - float2(px.x, 0), depth_at(i.uv - float2(px.x, 0)));
    float3 Pd = view_pos(i.uv + float2(0, px.y), depth_at(i.uv + float2(0, px.y)));
    float3 Pu = view_pos(i.uv - float2(0, px.y), depth_at(i.uv - float2(0, px.y)));
    float3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    float3 dy = abs(Pd.z - P.z) < abs(P.z - Pu.z) ? Pd - P : P - Pu;
    float3 N = normalize(cross(dy, dx));
    if (N.z > 0) N = -N;                       // face the camera
    float4 gn = gnorm.Load(int3(texel_of(i.uv), 0));
    if (gn.w > 0.5) { float3 e = normalize(gn.xyz * 2 - 1); N = float3(e.x, e.y, -e.z); }   // engine -z -> +z forward
    float r = proj.z;
    float2 rs = r / (d * proj.xy) * 0.5;       // world radius -> uv radius
    // fade where the radius spans only a few rows / columns of the SURFACE itself (radius / its step per pixel - the
    // world map's foreshortened mid-distance ground: samples snapped to the next row or not, flipping row by row into
    // horizontal lines in a band of fixed screen rows; arenas' close ground spans many rows, unaffected)
    float span = r / max(max(length(dx), length(dy)), 1e-5);
    float kfade = saturate((span - 3) / 3);
    if (kfade <= 0) return 1;
    float noise = frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    if (dir.z > 2.5) return noise;              // debug view 3: the sampling noise alone
    float sum = 0;
    const int S = 12;
    [unroll] for (int k = 0; k < S; k++) {
        float t = (k + 0.5) / S;
        float a = (t * 7.0 + noise) * 6.2831853;
        float2 uv = i.uv + float2(cos(a), sin(a)) * rs * t;
        float ds = depth_at(uv);
        if (ds > 60000) continue;
        float3 v = view_pos(uv, ds) - P;
        float vv = dot(v, v);
        float fall = saturate(1 - vv / (r * r));
        // angle bias: only samples clearly above the surface count - flat ground seen at a grazing angle
        // occluded itself through tiny normal / depth errors, in horizontal bands
        float cosv = dot(v, N) * rsqrt(vv + 1e-6);
        sum += max(0, cosv - 0.25) * fall;
    }
    return saturate(1 - proj.w * 1.5 * sum / S * kfade);
}
float blur(V i) : SV_Target
{
    // depth-aware against the surface's own slope: ground seen at a grazing angle changes depth fast from row to
    // row, so comparing with the centre depth rejected every vertical neighbour and each row kept its own noise -
    // horizontal bars (world map, 2x vertical scene). The neighbour is compared with the plane's prediction.
    float d0 = depth_at(i.uv), w = 0, s = 0;
    float g = (depth_at(i.uv + dir.xy) - depth_at(i.uv - dir.xy)) * 0.5;
    [unroll] for (int k = -4; k <= 4; k++) {
        float2 uv = i.uv + dir.xy * k;
        float dk = depth_at(uv);
        float wk = exp(-k * k / 8.0) * saturate(1 - abs(dk - (d0 + g * k)) / (0.03 * d0 + 0.01));
        s += aoTex.SampleLevel(pt, uv, 0).r * wk;
        w += wk;
    }
    return w > 0 ? s / w : 1;
}
float4 comp(V i) : SV_Target
{
    float a = aoTex.SampleLevel(lin, i.uv, 0).r;
    return dir.z > 0 ? float4(a.xxx, 1) : float4(a.xxx, 1);
}
)";

static bool init()
{
    if (g_init)
        return !g_failed;
    g_init = true;
    char ini[MAX_PATH], num[32];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    g_d3d.ssao = GetPrivateProfileIntA("video", "SSAO", 0, ini) != 0;   // start value; F6 toggles
    g_on = true;
    GetPrivateProfileStringA("video", "SSAORadius", "0.8", num, sizeof num, ini);
    g_radius = (float)atof(num);
    GetPrivateProfileStringA("video", "SSAOStrength", "1.0", num, sizeof num, ini);
    g_strength = (float)atof(num);
    g_show = GetPrivateProfileIntA("debug", "ShowAO", 0, ini);
    if (!g_on) {
        g_failed = true;
        return false;
    }
    ID3D11Device *dev = g_d3d.dev;
    auto compile = [&](const char *entry, const char *target) -> ID3DBlob * {
        ID3DBlob *code = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(k_hlsl, strlen(k_hlsl), "ssao", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
            port_log("ssao: %s compile failed: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
            return nullptr;
        }
        return code;
    };
    ID3DBlob *b[4] = {compile("vs", "vs_4_0"), compile("ao", "ps_4_0"), compile("blur", "ps_4_0"),
                      compile("comp", "ps_4_0")};
    if (!b[0] || !b[1] || !b[2] || !b[3]) {
        g_failed = true;
        return false;
    }
    dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &g_ps_ao);
    dev->CreatePixelShader(b[2]->GetBufferPointer(), b[2]->GetBufferSize(), nullptr, &g_ps_blur);
    dev->CreatePixelShader(b[3]->GetBufferPointer(), b[3]->GetBufferSize(), nullptr, &g_ps_comp);
    for (ID3DBlob *x : b)
        x->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&sd, &g_lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &g_pt);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = 15;
    dev->CreateBlendState(&bl, &g_bs_copy);
    bl.RenderTarget[0].BlendEnable = TRUE;               // scene colour *= AO (alpha kept)
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = 7;
    dev->CreateBlendState(&bl, &g_bs_mul);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds_off);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_rs);
    port_log("ssao: %s, radius %.2f, strength %.2f%s\n", g_d3d.ssao ? "on" : "off (F6)", g_radius, g_strength, g_show ? ", showing AO only" : "");
    return true;
}

static bool targets(UINT w, UINT h)                   // two half-resolution R8 AO buffers (ping-pong)
{
    if (g_tex[0] && g_w == w && g_h == h)
        return true;
    for (int k = 0; k < 2; k++) {
        if (g_srv[k]) g_srv[k]->Release(), g_srv[k] = nullptr;
        if (g_rtv[k]) g_rtv[k]->Release(), g_rtv[k] = nullptr;
        if (g_tex[k]) g_tex[k]->Release(), g_tex[k] = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int k = 0; k < 2; k++) {
        if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_tex[k])))
            return false;
        g_d3d.dev->CreateRenderTargetView(g_tex[k], nullptr, &g_rtv[k]);
        g_d3d.dev->CreateShaderResourceView(g_tex[k], nullptr, &g_srv[k]);
    }
    g_w = w, g_h = h;
    return true;
}

// the active 3D camera's tan(half fov) per axis; false when it does not look like a perspective camera
static bool camera_tan(float *tx, float *ty)
{
    const BYTE *cam = *(const BYTE *const *)0x4B2808;   // cam_activecam
    if (!cam)
        return false;
    float n = *(const float *)(cam + 0x274), hw = *(const float *)(cam + 0x27C), hh = *(const float *)(cam + 0x280);
    if (!(n > 0) || !(hw > 0) || !(hh > 0))
        return false;
    *tx = hw / n, *ty = hh / n;
    // the projection the frame was really drawn with (from the shadow pass) wins over the camera object
    float P[16], R[16];
    bool have_p = d3d_camera_this_frame(P, R) && P[0] > 1e-4f && P[5] > 1e-4f;
    static DWORD logged;
    if (g_d3d.frame - logged >= 600) {
        logged = g_d3d.frame;
        port_log("ssao: camera near %.3f far %.1f tan %.3f %.3f | projection tan %s %.3f %.3f | radius %.2f\n", n,
                 *(const float *)(cam + 0x278), *tx, *ty, have_p ? "" : "(none)", have_p ? 1 / P[0] : 0,
                 have_p ? 1 / P[5] : 0, g_radius);
    }
    if (have_p)
        *tx = 1 / P[0], *ty = 1 / P[5];
    return *tx > 0.05f && *tx < 5 && *ty > 0.05f && *ty < 5;
}

void d3d_ssao_init()                                    // at device creation: no shader compile mid-game
{
    init();
}

void d3d_ssao_apply()
{
    if (g_done_frame == g_d3d.frame)                    // once a frame (the particle renderer runs more than once)
        return;
    g_done_frame = g_d3d.frame;
    if (!g_d3d.ssao || !g_d3d.gbuf_srv || !g_d3d.rtv || !init())
        return;
    float tx, ty;
    if (!camera_tan(&tx, &ty))
        return;
    UINT fw = g_d3d.scene_width, fh = g_d3d.scene_height, hw = (fw + 1) / 2, hh = (fh + 1) / 2;
    if (!targets(hw, hh))
        return;
    prof_gpu_pass(GP_SSAO);
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    ID3D11ShaderResourceView *none[4] = {};
    ctx->PSSetShaderResources(0, 4, none);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D11SamplerState *smp[2] = {g_lin, g_pt};
    ctx->PSSetSamplers(0, 2, smp);
    ctx->OMSetDepthStencilState(g_ds_off, 0);
    ctx->RSSetState(g_rs);
    float bf[4] = {};
    auto pass = [&](ID3D11PixelShader *ps, ID3D11RenderTargetView *rt, UINT w, UINT h, ID3D11ShaderResourceView *ao,
                    float dx, float dy, ID3D11BlendState *bs) {
        float cb[12] = {(float)fw, (float)fh, (float)hw, (float)hh, tx, ty, g_radius, g_strength, dx, dy,
                        (float)g_show, 0};
        ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
        ID3D11ShaderResourceView *srv[3] = {g_d3d.gbuf_srv, ao, g_d3d.gnrm_srv};
        ctx->PSSetShaderResources(0, 3, srv);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 3, none);
        ID3D11RenderTargetView *no_rt = nullptr;
        ctx->OMSetRenderTargets(1, &no_rt, nullptr);
    };
    pass(g_ps_ao, g_rtv[0], hw, hh, nullptr, 0, 0, g_bs_copy);
    if (g_show < 2) {                                   // debug views 2 / 3: the AO pass output unblurred
        pass(g_ps_blur, g_rtv[1], hw, hh, g_srv[0], 1.0f / hw, 0, g_bs_copy);
        pass(g_ps_blur, g_rtv[0], hw, hh, g_srv[1], 0, 1.0f / hh, g_bs_copy);
    }
    pass(g_ps_comp, g_d3d.rtv, fw, fh, g_srv[0], 0, 0, g_show ? g_bs_copy : g_bs_mul);
    d3d_state_invalidate();                             // the next game draw rebinds targets, blend, depth, raster
}
