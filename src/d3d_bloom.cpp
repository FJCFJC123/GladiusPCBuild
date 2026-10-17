// Bloom ([video] Bloom = intensity, 0 = off; BloomThreshold) and god rays ([video] GodRays). Only overbright HDR light
// blooms. Chain: half-resolution prefilter with a soft-knee threshold, 4 more 13-tap downsamples, then 9-tap tent
// upsamples added onto each larger level. The final pass (d3d_core.cpp) adds level 0 before tone mapping.
#include "d3d.h"
#include "d3d_prof.h"
#include <d3dcompiler.h>
#include <math.h>

void port_screenshot(const char *path);   // d3d_core.cpp

enum { LEVELS = 5 };
static bool g_init, g_failed;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_pre, *g_ps_down, *g_ps_up, *g_ps_rays, *g_ps_sky;
enum { SKY_W = 64, SKY_H = 36, SKY_RING = 3 };
static ID3D11Texture2D *g_sky, *g_sky_read[SKY_RING];
static ID3D11RenderTargetView *g_sky_rtv;
static float g_sun_u, g_sun_v;                          // visible sun on screen (from the sky map, 2 frames old)
static float g_sun_fade;                                // rays shown 0..1 (fades, no switching)
static bool g_sun_seen;                                 // the last read sky map had a sun
static ID3D11Buffer *g_cb;
static ID3D11SamplerState *g_lin;
static ID3D11BlendState *g_bs_copy, *g_bs_add;
static ID3D11DepthStencilState *g_ds_off;
static ID3D11RasterizerState *g_rs;
static ID3D11Texture2D *g_tex[LEVELS];
static ID3D11RenderTargetView *g_rtv[LEVELS];
static ID3D11ShaderResourceView *g_srv[LEVELS];
static UINT g_w[LEVELS], g_h[LEVELS], g_src_w, g_src_h;

static const char *k_hlsl = R"(
Texture2D src : register(t0);
SamplerState lin : register(s0);
cbuffer BloomCB : register(b0) { float4 texel; float4 knee; };   // texel: 1/src size; knee: threshold, knee, -, -
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D<float> gdepth : register(t1);
float3 S(float2 uv) { return src.SampleLevel(lin, uv, 0).rgb; }
float3 down13(float2 uv)
{
    float2 t = texel.xy;
    float3 a = S(uv + t * float2(-2, -2)), b = S(uv + t * float2(0, -2)), c = S(uv + t * float2(2, -2));
    float3 d = S(uv + t * float2(-1, -1)), e = S(uv + t * float2(1, -1));
    float3 f = S(uv + t * float2(-2, 0)), g = S(uv), h = S(uv + t * float2(2, 0));
    float3 i = S(uv + t * float2(-1, 1)), j = S(uv + t * float2(1, 1));
    float3 k = S(uv + t * float2(-2, 2)), l = S(uv + t * float2(0, 2)), m = S(uv + t * float2(2, 2));
    return (d + e + i + j) * 0.125 + (a + b + f + g) * 0.03125 + (b + c + g + h) * 0.03125 +
           (f + g + k + l) * 0.03125 + (g + h + l + m) * 0.03125;
}
float4 pre(V i) : SV_Target
{
    float3 c = max(down13(i.uv), 0);
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - knee.x + knee.y, 0, 2 * knee.y);       // soft knee around the threshold
    rq = rq * rq / (4 * knee.y + 1e-5);
    float w = max(rq, br - knee.x) / max(br, 1e-5);
    return float4(min(c * w, 16), 1);                            // clamp fireflies
}
float4 down(V i) : SV_Target { return float4(down13(i.uv), 1); }
// sun finder: sky brightness per cell of a 64 x 36 map (x = mean luminance of the cell's sky samples, y = sky share)
float4 skylum(V i) : SV_Target
{
    uint w, h;
    gdepth.GetDimensions(w, h);
    float lum = 0, n = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++) {
            float2 uv = i.uv + (float2(x, y) - 1.5) / 4 * texel.xy;
            if (abs(gdepth.Load(int3(clamp(uv, 0, 0.9999) * float2(w, h), 0))) >= 60000) {
                lum += dot(src.SampleLevel(lin, uv, 0).rgb, float3(0.2126, 0.7152, 0.0722));
                n += 1;
            }
        }
    return float4(n > 0 ? lum / n : 0, n / 16, 0, 1);
}
// god rays: sky pixels (no G-buffer depth) near the sun, smeared towards it; knee.zw = sun uv, texel.zw = aspect,
// strength / bloom intensity (the final pass multiplies level 0 by the bloom intensity)
float3 sky_at(float2 uv)
{
    uint w, h;
    gdepth.GetDimensions(w, h);
    float d = gdepth.Load(int3(clamp(uv, 0, 0.9999) * float2(w, h), 0));
    if (any(uv < 0) || any(uv > 1) || abs(d) < 60000) return 0;
    float2 q = (uv - knee.zw) * float2(texel.z, 1);
    float near_sun = saturate(1 - length(q) * 1.6);
    return src.SampleLevel(lin, uv, 0).rgb * near_sun * near_sun;
}
float4 rays(V i) : SV_Target
{
    float2 dir = (knee.zw - i.uv) / 48 * 0.9;
    float2 uv = i.uv;
    float3 acc = 0;
    float wgt = 1;
    [loop] for (int k = 0; k < 48; k++) {
        acc += sky_at(uv) * wgt;
        wgt *= 0.955;
        uv += dir;
    }
    float on_screen = saturate(1.5 - max(abs(knee.z - 0.5), abs(knee.w - 0.5)) * 2);   // fades as the sun leaves
    return float4(acc / 48 * texel.w * on_screen, 1);
}
float4 up(V i) : SV_Target
{
    float2 t = texel.xy;
    float3 s = S(i.uv + t * float2(-1, -1)) + S(i.uv + t * float2(1, -1)) + S(i.uv + t * float2(-1, 1)) +
               S(i.uv + t * float2(1, 1));
    s += (S(i.uv + t * float2(0, -1)) + S(i.uv + t * float2(-1, 0)) + S(i.uv + t * float2(1, 0)) +
          S(i.uv + t * float2(0, 1))) * 2;
    s += S(i.uv) * 4;
    return float4(s / 16, 1);
}
)";

// Release the bloom level textures.
static void release_targets()
{
    for (int k = 0; k < LEVELS; k++) {
        if (g_srv[k]) { g_srv[k]->Release(); g_srv[k] = nullptr; }
        if (g_rtv[k]) { g_rtv[k]->Release(); g_rtv[k] = nullptr; }
        if (g_tex[k]) { g_tex[k]->Release(); g_tex[k] = nullptr; }
    }
    g_src_w = g_src_h = 0;
}

// (Re)create the bloom levels for a w x h scene.
static bool targets(UINT w, UINT h)
{
    if (g_tex[0] && g_src_w == w && g_src_h == h)
        return true;
    release_targets();
    for (int k = 0; k < LEVELS; k++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = g_w[k] = (w >> (k + 1)) ? (w >> (k + 1)) : 1;
        td.Height = g_h[k] = (h >> (k + 1)) ? (h >> (k + 1)) : 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3d.dev->CreateTexture2D(&td, nullptr, &g_tex[k])))
            return false;
        g_d3d.dev->CreateRenderTargetView(g_tex[k], nullptr, &g_rtv[k]);
        g_d3d.dev->CreateShaderResourceView(g_tex[k], nullptr, &g_srv[k]);
    }
    g_src_w = w, g_src_h = h;
    return true;
}

// Compile the shaders and create the fixed states (once).
static bool init()
{
    if (g_init)
        return !g_failed;
    g_init = true;
    ID3D11Device *dev = g_d3d.dev;
    auto compile = [&](const char *entry, const char *target) -> ID3DBlob * {
        ID3DBlob *code = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(k_hlsl, strlen(k_hlsl), "bloom", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
            port_log("bloom: shader %s failed: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
            return nullptr;
        }
        return code;
    };
    ID3DBlob *b[6] = {compile("vs", "vs_4_0"), compile("pre", "ps_4_0"), compile("down", "ps_4_0"),
                      compile("up", "ps_4_0"), compile("rays", "ps_4_0"), compile("skylum", "ps_4_0")};
    if (!b[0] || !b[1] || !b[2] || !b[3] || !b[4] || !b[5]) {
        g_failed = true;
        return false;
    }
    dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &g_ps_pre);
    dev->CreatePixelShader(b[2]->GetBufferPointer(), b[2]->GetBufferSize(), nullptr, &g_ps_down);
    dev->CreatePixelShader(b[3]->GetBufferPointer(), b[3]->GetBufferSize(), nullptr, &g_ps_up);
    dev->CreatePixelShader(b[4]->GetBufferPointer(), b[4]->GetBufferSize(), nullptr, &g_ps_rays);
    dev->CreatePixelShader(b[5]->GetBufferPointer(), b[5]->GetBufferSize(), nullptr, &g_ps_sky);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = SKY_W, td.Height = SKY_H, td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_sky)))
        dev->CreateRenderTargetView(g_sky, nullptr, &g_sky_rtv);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int k = 0; k < SKY_RING; k++)
        dev->CreateTexture2D(&td, nullptr, &g_sky_read[k]);
    for (ID3DBlob *x : b)
        x->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_lin);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = 15;
    dev->CreateBlendState(&bl, &g_bs_copy);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bl, &g_bs_add);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds_off);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_rs);
    port_log("bloom: intensity %.2f, threshold %.2f\n", g_d3d.bloom, g_d3d.bloom_threshold);
    return true;
}

// Find the visible sun in the picture: sky brightness per cell of a 64 x 36 map, read back two frames later (no
// stall). A sun is one spot clearly brighter than the rest of the sky; its position is the weighted centre of the
// brightest cells.
static void find_sun(ID3D11ShaderResourceView *scene)
{
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    float cb[8] = {1.0f / SKY_W, 1.0f / SKY_H, 0, 0, 0, 0, 0, 0};
    ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    ctx->OMSetRenderTargets(1, &g_sky_rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)SKY_W, (float)SKY_H, 0, 1};
    ctx->RSSetViewports(1, &vp);
    float bf[4] = {};
    ctx->OMSetBlendState(g_bs_copy, bf, 0xFFFFFFFF);
    ctx->PSSetShader(g_ps_sky, nullptr, 0);
    ID3D11ShaderResourceView *in[2] = {scene, g_d3d.gbuf_srv}, *none[2] = {};
    ctx->PSSetShaderResources(0, 2, in);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView *no_rt = nullptr;
    ctx->OMSetRenderTargets(1, &no_rt, nullptr);
    DWORD f = g_d3d.frame;
    ctx->CopyResource(g_sky_read[f % SKY_RING], g_sky);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *old = g_sky_read[(f + 1) % SKY_RING];   // written two frames ago
    if (f < 2 || FAILED(ctx->Map(old, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)))
        return;
    auto half = [](unsigned short h) {                  // float16 -> float
        unsigned e = (h >> 10) & 31, mt = h & 1023;
        float v = e == 0 ? mt / 1024.0f / 16384.0f : e == 31 ? 65504.0f : ldexpf(1 + mt / 1024.0f, (int)e - 15);
        return h & 0x8000 ? -v : v;
    };
    static float lum[SKY_W * SKY_H], share[SKY_W * SKY_H];
    for (int y = 0; y < SKY_H; y++) {
        const unsigned short *row = (const unsigned short *)((const BYTE *)m.pData + y * m.RowPitch);
        for (int x = 0; x < SKY_W; x++)
            lum[y * SKY_W + x] = half(row[x * 2]), share[y * SKY_W + x] = half(row[x * 2 + 1]);
    }
    ctx->Unmap(old, 0);
    float sum = 0, cnt = 0, peak = 0, geo = 0;
    for (int k = 0; k < SKY_W * SKY_H; k++) {
        geo += share[k] < 0.5f;                         // cells with 3D geometry
        if (share[k] > 0.5f && lum[k] > 0.05f) {      // near-black "sky" = letterbox bars / empty 2D: not sky
            sum += lum[k], cnt += 1;
            peak = lum[k] > peak ? lum[k] : peak;
        }
    }
    // hysteresis: a shown sun stays at 1.2x the sky, a new one needs 1.35x (no flicker)
    float need = g_sun_fade > 0 ? 1.2f : 1.35f;
    if (geo < 0.15f * SKY_W * SKY_H || cnt < 8 || peak < 0.7f || peak < need * sum / cnt) {
        g_sun_seen = false;                             // no 3D (2D screens are all "sky") / no sky / no bright spot
        return;
    }
    float u = 0, v = 0, wsum = 0;
    for (int y = 0; y < SKY_H; y++)
        for (int x = 0; x < SKY_W; x++) {
            float l = lum[y * SKY_W + x];
            if (share[y * SKY_W + x] > 0.5f && l > 0.9f * peak) {
                u += (x + 0.5f) / SKY_W * l, v += (y + 0.5f) / SKY_H * l, wsum += l;
            }
        }
    if (wsum <= 0) {
        g_sun_seen = false;
        return;
    }
    float nu = u / wsum, nv = v / wsum;
    if (g_sun_fade <= 0 || fabsf(nu - g_sun_u) + fabsf(nv - g_sun_v) > 0.3f)
        g_sun_u = nu, g_sun_v = nv;                     // new sun: take it as it is
    else                                                // smooth out the 64 x 36 grid steps
        g_sun_u += (nu - g_sun_u) * 0.3f, g_sun_v += (nv - g_sun_v) * 0.3f;
    g_sun_seen = true;
}

// Scene -> bloom texture (half resolution) for the final pass; nullptr when off.
ID3D11ShaderResourceView *d3d_bloom_run(ID3D11ShaderResourceView *scene, UINT w, UINT h)
{
    if (g_d3d.bloom <= 0 || !scene || !init() || !targets(w, h))
        return nullptr;
    prof_gpu_pass(GP_BLOOM);
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    ID3D11ShaderResourceView *none = nullptr;
    ID3D11RenderTargetView *no_rt = nullptr;
    float bf[4] = {};
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetSamplers(0, 1, &g_lin);
    ctx->OMSetDepthStencilState(g_ds_off, 0);
    ctx->RSSetState(g_rs);
    auto pass = [&](ID3D11PixelShader *ps, ID3D11ShaderResourceView *in, UINT in_w, UINT in_h, int out,
                    ID3D11BlendState *bs) {
        float knee = g_d3d.bloom_threshold * 0.1f;      // narrow: plain white (walls, HUD) stays out
        float cb[8] = {1.0f / in_w, 1.0f / in_h, 0, 0, g_d3d.bloom_threshold, knee > 0.01f ? knee : 0.01f, 0, 0};
        ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(1, &g_rtv[out], nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)g_w[out], (float)g_h[out], 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &in);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(1, &no_rt, nullptr);
    };
    pass(g_ps_pre, scene, w, h, 0, g_bs_copy);
    for (int k = 1; k < LEVELS; k++)
        pass(g_ps_down, g_srv[k - 1], g_w[k - 1], g_h[k - 1], k, g_bs_copy);
    for (int k = LEVELS - 1; k > 0; k--)                // each smaller level blurred up onto the next larger one
        pass(g_ps_up, g_srv[k], g_w[k], g_h[k], k - 1, g_bs_add);
    if (g_d3d.god_rays > 0 && g_d3d.gbuf_srv && g_sky_rtv) {
        find_sun(scene);                                // fades in over ~12 frames, out over ~20
        g_sun_fade += g_sun_seen ? 0.08f : -0.05f;
        g_sun_fade = g_sun_fade < 0 ? 0 : g_sun_fade > 1 ? 1 : g_sun_fade;
    }
    // god rays: added onto level 0 while a sun is (or was just) visible, scaled by the fade
    if (g_d3d.god_rays > 0 && g_d3d.gbuf_srv && g_sun_fade > 0.01f) {
        float fade = g_sun_fade * g_sun_fade * (3 - 2 * g_sun_fade);   // smoothstep
        float cb[8] = {1.0f / w, 1.0f / h, (float)w / h, g_d3d.god_rays / g_d3d.bloom * fade, 0, 0, g_sun_u, g_sun_v};
        ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        ctx->OMSetRenderTargets(1, &g_rtv[0], nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)g_w[0], (float)g_h[0], 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetBlendState(g_bs_add, bf, 0xFFFFFFFF);
        ctx->PSSetShader(g_ps_rays, nullptr, 0);
        ID3D11ShaderResourceView *in[2] = {scene, g_d3d.gbuf_srv};
        ctx->PSSetShaderResources(0, 2, in);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none2[2] = {};
        ctx->PSSetShaderResources(0, 2, none2);
        ctx->OMSetRenderTargets(1, &no_rt, nullptr);
        static int shots = -1;                          // [debug] GodRayShots=N: screenshot while rays are drawn
        static DWORD at;
        if (shots < 0) {
            char ini[MAX_PATH];
            GetModuleFileNameA(nullptr, ini, MAX_PATH);
            strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
            shots = GetPrivateProfileIntA("debug", "GodRayShots", 0, ini);
        }
        if (g_d3d.frame - at > 300) {
            at = g_d3d.frame;
            port_log("god rays: sun on screen at %.2f %.2f (frame %u)\n", g_sun_u, g_sun_v, g_d3d.frame);
            if (shots > 0) {
                char path[MAX_PATH];
                GetModuleFileNameA(nullptr, path, MAX_PATH);
                _snprintf(strrchr(path, 0x5C) + 1, 64, "shot_rays_%u.bmp", g_d3d.frame);
                port_screenshot(path);
                shots--;
            }
        }
    }
    d3d_state_invalidate();
    return g_srv[0];
}
