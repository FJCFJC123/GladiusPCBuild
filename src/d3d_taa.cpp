// Temporal anti-aliasing ([video] AntiAlias=taa; F9 cycles off / FXAA / TAA). 3D draws are jittered by a Halton(2,3)
// sub-pixel offset over 8 frames. The 3D image is resolved before the 2D layer is drawn, so the HUD never smears:
// last frame's image is reprojected - per pixel by the motion vectors of opaque program draws (d3d_program.cpp),
// elsewhere by the camera's motion - clamped to the current 3x3 neighbourhood and blended 1:9.
#include "d3d.h"
#include "d3d_prof.h"
#include <d3dcompiler.h>
#include <math.h>

static bool g_init, g_failed;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_lin, *g_pt;
static ID3D11BlendState *g_bs;
static ID3D11DepthStencilState *g_ds_off;
static ID3D11RasterizerState *g_rs;
static ID3D11Texture2D *g_hist[2];
static ID3D11RenderTargetView *g_hist_rtv[2];
static ID3D11ShaderResourceView *g_hist_srv[2];
static UINT g_w, g_h;
static int g_cur;                                       // history written this frame
static bool g_hist_valid;
static float g_jitter[2];                               // NDC offset of this frame's 3D draws
static bool g_pending, g_had_3d, g_resolved;
static float g_prevVP[16];                              // last frame: world -> clip (P * R)
static bool g_prev_ok;

static const char *k_hlsl = R"(
Texture2D cur : register(t0);
Texture2D hist : register(t1);
Texture2D<float> gdepth : register(t2);
Texture2D<float2> gmotion : register(t3);               // uv offset to last frame (x 60000 = unknown)
SamplerState lin : register(s0);
SamplerState pt : register(s1);
cbuffer TaaCB : register(b0) {
    float4 reproj[4];   // current clip (x, y from uv, z / w from view depth) -> last frame's clip
    float4 P;           // current projection: P00, P11, P[2][2]... packed: x = P00, y = P11, z = P22, w = P23
    float4 info;        // 1/w, 1/h, history valid, -
    float4 jit;         // this frame's jitter (NDC)
};
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 ycocg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float wt(float3 c) { return 1 / (1 + dot(c, float3(0.2126, 0.7152, 0.0722))); }
float4 ps(V i) : SV_Target
{
    float4 c = cur.SampleLevel(pt, i.uv, 0);
    // neighbourhood (YCoCg, tone-mapped) for the history clamp
    float3 mn = 1e9, mx = -1e9, m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            float3 s = cur.SampleLevel(pt, i.uv + float2(x, y) * info.xy, 0).rgb;
            float3 q = ycocg(s * wt(s));
            mn = min(mn, q), mx = max(mx, q), m1 += q, m2 += q * q;
        }
    m1 /= 9, m2 /= 9;
    float3 sd = sqrt(max(m2 - m1 * m1, 0));
    mn = max(mn, m1 - sd), mx = min(mx, m1 + sd);       // variance clip, tightened by the box
    if (info.z < 0.5) return c;
    // reprojection: this pixel's point (G-buffer view depth; |w|, lit draws store it negative) in last frame's clip
    float w = abs(gdepth.Load(int3(i.uv / info.xy, 0)));
    // the pixel centre, not the jittered sample: history is looked up where this output pixel was last frame
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float2 prev_uv = i.uv;
    float2 mv = gmotion.Load(int3(i.uv / info.xy, 0));
    if (mv.x < 30000) {                                 // the surface's own motion (moving / spinning / animated)
        prev_uv = i.uv - mv;
    } else if (w > 0 && w < 60000) {                    // no motion vector: the camera's motion
        float4 clip = float4(ndc * w, P.z * -w + P.w, w);          // z from P row 2 with view z = -w
        float4 pc = float4(dot(reproj[0], clip), dot(reproj[1], clip), dot(reproj[2], clip), dot(reproj[3], clip));
        if (pc.w > 1e-4) prev_uv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    } else {                                            // sky: direction only (w -> infinity)
        float4 clip = float4(ndc, -P.z, 1);
        float4 pc = float4(dot(reproj[0], clip), dot(reproj[1], clip), dot(reproj[2], clip), dot(reproj[3], clip));
        if (pc.w > 1e-6) prev_uv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    }
    if (any(prev_uv < 0) || any(prev_uv > 1)) return c;
    float3 h = hist.SampleLevel(lin, prev_uv, 0).rgb;
    float3 hy = ycocg(h * wt(h)), hq = clamp(hy, mn, mx);
    float3 cq = ycocg(c.rgb * wt(c.rgb));
    // anti-ghosting: history the clamp had to pull far (something moved here) gives way to the current frame faster
    float k = lerp(0.1, 0.5, saturate(length(hy - hq) * 8));
    float3 r = rgb(lerp(hq, cq, k));
    r = r / max(1 - dot(r, float3(0.2126, 0.7152, 0.0722)), 1e-3);   // undo the tone-mapped weight
    return float4(max(r, 0), c.a);
}
)";

// Release the history textures.
static void release_hist()
{
    for (int k = 0; k < 2; k++) {
        if (g_hist_srv[k]) { g_hist_srv[k]->Release(); g_hist_srv[k] = nullptr; }
        if (g_hist_rtv[k]) { g_hist_rtv[k]->Release(); g_hist_rtv[k] = nullptr; }
        if (g_hist[k]) { g_hist[k]->Release(); g_hist[k] = nullptr; }
    }
    g_w = g_h = 0;
    g_hist_valid = false;
}

// (Re)create the two history textures at the scene size.
static bool targets()
{
    D3D11_TEXTURE2D_DESC sd;
    g_d3d.scene_tex->GetDesc(&sd);
    if (g_hist[0] && g_w == sd.Width && g_h == sd.Height)
        return true;
    release_hist();
    sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int k = 0; k < 2; k++) {
        if (FAILED(g_d3d.dev->CreateTexture2D(&sd, nullptr, &g_hist[k])))
            return false;
        g_d3d.dev->CreateRenderTargetView(g_hist[k], nullptr, &g_hist_rtv[k]);
        g_d3d.dev->CreateShaderResourceView(g_hist[k], nullptr, &g_hist_srv[k]);
    }
    g_w = sd.Width, g_h = sd.Height;
    return true;
}

// Compile the shaders and create the fixed states (once).
static bool init()
{
    if (g_init)
        return !g_failed;
    g_init = true;
    ID3D11Device *dev = g_d3d.dev;
    ID3DBlob *v = nullptr, *p = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(k_hlsl, strlen(k_hlsl), "taa", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &v, &err)) ||
        FAILED(D3DCompile(k_hlsl, strlen(k_hlsl), "taa", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &p, &err))) {
        port_log("taa: shader failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        g_failed = true;
        return false;
    }
    dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g_ps);
    v->Release(), p->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 7 * 16;
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
    dev->CreateBlendState(&bl, &g_bs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds_off);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_rs);
    port_log("taa: ready\n");
    return true;
}

// Halton sequence value i in base b.
static float halton(int i, int b)
{
    float f = 1, r = 0;
    for (; i > 0; i /= b)
        f /= b, r += f * (i % b);
    return r;
}

// Present: pick the next frame's jitter (only when this frame had 3D, so menus stay still).
void d3d_taa_frame_begin()
{
    bool on = g_d3d.anti_alias == 2 && g_had_3d && !g_failed;
    if (!on && g_hist_valid && g_d3d.anti_alias != 2)
        g_hist_valid = false;
    g_had_3d = false;
    g_pending = false;
    g_resolved = false;
    if (!on || !g_d3d.scene_width) {
        g_jitter[0] = g_jitter[1] = 0;
        return;
    }
    int k = (int)(g_d3d.frame % 8) + 1;
    g_jitter[0] = (halton(k, 2) - 0.5f) * 2.0f / g_d3d.scene_width;
    g_jitter[1] = (halton(k, 3) - 0.5f) * 2.0f / g_d3d.scene_height;
}

// This frame's jitter for the vertex shaders (0 once resolved: the 2D layer is not jittered).
void d3d_taa_jitter(float *xy)
{
    xy[0] = g_resolved ? 0 : g_jitter[0];
    xy[1] = g_resolved ? 0 : g_jitter[1];
}

void d3d_taa_mark_3d()                                  // fx_render_3d returned: the 3D image may be complete
{
    g_had_3d = true;
    if (g_d3d.anti_alias == 2)
        g_pending = true;
}

// Resolve: blend the reprojected history into the scene and keep the result as the next history.
static void resolve()
{
    g_pending = false;
    g_resolved = true;
    if (!g_d3d.scene_tex || !g_d3d.gbuf_srv || !init() || !targets())
        return;
    prof_gpu_pass(GP_TAA);
    float P[16], R[16];
    bool cam = d3d_camera_this_frame(P, R);
    float VP[16] = {}, reproj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // no camera: still camera
    bool hist_ok = g_hist_valid;
    if (cam) {
        for (int r = 0; r < 4; r++)                     // VP = P * R (world -> clip)
            for (int c = 0; c < 4; c++)
                VP[r * 4 + c] = P[r * 4] * R[c] + P[r * 4 + 1] * R[4 + c] + P[r * 4 + 2] * R[8 + c] + P[r * 4 + 3] * R[12 + c];
        float VPi[16];
        if (hist_ok && g_prev_ok && d3d_invert4(VP, VPi))   // current clip -> world -> last frame's clip
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    reproj[r * 4 + c] = g_prevVP[r * 4] * VPi[c] + g_prevVP[r * 4 + 1] * VPi[4 + c] +
                                        g_prevVP[r * 4 + 2] * VPi[8 + c] + g_prevVP[r * 4 + 3] * VPi[12 + c];
    }
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    float cb[28] = {};
    memcpy(cb, reproj, 64);
    cb[16] = cam ? P[0] : 1, cb[17] = cam ? P[5] : 1, cb[18] = cam ? P[10] : 0, cb[19] = cam ? P[11] : 0;
    cb[20] = 1.0f / g_w, cb[21] = 1.0f / g_h, cb[22] = hist_ok ? 1.0f : 0.0f;
    cb[24] = g_jitter[0], cb[25] = g_jitter[1];
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    int out = g_cur ^ 1;
    ID3D11ShaderResourceView *none[4] = {};
    ctx->PSSetShaderResources(0, 4, none);
    ctx->OMSetRenderTargets(1, &g_hist_rtv[out], nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    float bf[4] = {};
    ctx->OMSetBlendState(g_bs, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_ds_off, 0);
    ctx->RSSetState(g_rs);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D11SamplerState *smp[2] = {g_lin, g_pt};
    ctx->PSSetSamplers(0, 2, smp);
    ID3D11ShaderResourceView *in[4] = {g_d3d.scene_srv, g_hist_srv[g_cur], g_d3d.gbuf_srv, g_d3d.gmv_srv};
    ctx->PSSetShaderResources(0, 4, in);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 4, none);
    ID3D11RenderTargetView *no_rt = nullptr;
    ctx->OMSetRenderTargets(1, &no_rt, nullptr);
    ctx->CopyResource(g_d3d.scene_tex, g_hist[out]);    // the 2D layer is drawn over the resolved image
    g_cur = out;
    g_hist_valid = true;
    if (cam)
        memcpy(g_prevVP, VP, sizeof VP);
    g_prev_ok = cam;
    d3d_state_invalidate();
}

void d3d_taa_before_draw(bool depth_test)              // first 2D draw after the 3D effects resolves
{
    if (g_pending && !depth_test)
        resolve();
}

void d3d_taa_before_final()                             // final pass: a 3D frame without a 2D layer
{
    if (g_pending)
        resolve();
}
