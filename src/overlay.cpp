// Port-side 2D overlay (editor, lobby screens): drawn on the CPU into a premultiplied BGRA buffer at 2x the game's
// 640x480 coordinates (GDI text) and composited over the back buffer right before Present.
#include "d3d.h"
#include "d3d_prof.h"
#include "overlay.h"
#include <d3dcompiler.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const int S = 2;                              // buffer pixels per game unit
static const int W = 640 * S, H = 480 * S;

static CRITICAL_SECTION g_lock;
static bool g_lock_init;
static uint32_t *g_px;                               // being drawn (main thread)
static uint32_t *g_pub;                              // last finished frame
static bool g_pub_visible, g_pub_dirty;

static HDC g_dc;
static HBITMAP g_mask_bmp;
static uint32_t *g_mask;                             // GDI target: white text on black
static HFONT g_font_md, g_font_sm;

static ID3D11Texture2D *g_tex;
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11BlendState *g_blend;
static ID3D11RasterizerState *g_raster;
static ID3D11DepthStencilState *g_depth;
static ID3D11SamplerState *g_sampler;
static bool g_gpu_failed;

static void init_cpu()
{
    if (g_px)
        return;
    g_px = (uint32_t *)calloc(W * H, 4);
    g_pub = (uint32_t *)calloc(W * H, 4);
    g_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;                       // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_mask_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_mask, nullptr, 0);
    SelectObject(g_dc, g_mask_bmp);
    g_font_md = CreateFontA(-18 * S, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Georgia");
    g_font_sm = CreateFontA(-13 * S, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                            "Segoe UI");
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, RGB(255, 255, 255));
}

void ov_begin()
{
    init_cpu();
    memset(g_px, 0, W * H * 4);
}

// premultiplied "over" of colour (straight ARGB) with coverage 0..255
static inline void blend_px(uint32_t *d, uint32_t argb, unsigned cov)
{
    unsigned a = ((argb >> 24) * cov + 127) / 255;
    if (!a)
        return;
    unsigned r = ((argb >> 16) & 255) * a / 255, g = ((argb >> 8) & 255) * a / 255, b = (argb & 255) * a / 255;
    uint32_t o = *d;
    unsigned ia = 255 - a;
    unsigned oa = o >> 24, orr = (o >> 16) & 255, og = (o >> 8) & 255, ob = o & 255;
    *d = ((a + oa * ia / 255) << 24) | ((r + orr * ia / 255) << 16) | ((g + og * ia / 255) << 8) | (b + ob * ia / 255);
}

void ov_box(float x, float y, float w, float h, uint32_t argb)
{
    int x0 = (int)(x * S), y0 = (int)(y * S), x1 = (int)((x + w) * S), y1 = (int)((y + h) * S);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            blend_px(&g_px[yy * W + xx], argb, 255);
}

void ov_frame(float x, float y, float w, float h, float t, uint32_t argb)
{
    ov_box(x, y, w, t, argb);
    ov_box(x, y + h - t, w, t, argb);
    ov_box(x, y + t, t, h - 2 * t, argb);
    ov_box(x + w - t, y + t, t, h - 2 * t, argb);
}

void ov_line(float x0, float y0, float x1, float y1, float t, uint32_t argb)
{
    float dx = (x1 - x0) * S, dy = (y1 - y0) * S;
    int steps = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) + 1;
    if (steps > 4 * (W + H)) return;                    // far off screen: not worth the walk
    int r = (int)(t * S * 0.5f + 0.5f);
    if (r < 1) r = 1;
    for (int i = 0; i <= steps; i++) {
        int cx = (int)(x0 * S + dx * i / steps), cy = (int)(y0 * S + dy * i / steps);
        if (cx < -r || cy < -r || cx >= W + r || cy >= H + r) continue;
        for (int yy = cy - r + 1; yy < cy + r; yy++)
            for (int xx = cx - r + 1; xx < cx + r; xx++)
                if (xx >= 0 && yy >= 0 && xx < W && yy < H)
                    blend_px(&g_px[yy * W + xx], argb, 128);
    }
}

void ov_circle(float cx, float cy, float r, uint32_t argb)
{
    int x0 = (int)((cx - r) * S) - 1, x1 = (int)((cx + r) * S) + 1;
    int y0 = (int)((cy - r) * S) - 1, y1 = (int)((cy + r) * S) + 1;
    float rr = r * S;
    for (int yy = y0 < 0 ? 0 : y0; yy < y1 && yy < H; yy++)
        for (int xx = x0 < 0 ? 0 : x0; xx < x1 && xx < W; xx++) {
            float dx = xx + 0.5f - cx * S, dy = yy + 0.5f - cy * S;
            float d = rr - sqrtf(dx * dx + dy * dy);           // > 0 inside, 1 px soft edge
            if (d <= -0.5f)
                continue;
            unsigned cov = d >= 0.5f ? 255 : (unsigned)((d + 0.5f) * 255);
            blend_px(&g_px[yy * W + xx], argb, cov);
        }
}

float ov_text_width(const char *s, bool sm)
{
    init_cpu();
    SelectObject(g_dc, sm ? g_font_sm : g_font_md);
    SIZE sz;
    GetTextExtentPoint32A(g_dc, s, (int)strlen(s), &sz);
    return (float)sz.cx / S;
}

void ov_text(const char *s, float x, float y, int align, uint32_t argb, bool sm)
{
    int n = (int)strlen(s);
    if (!n)
        return;
    SelectObject(g_dc, sm ? g_font_sm : g_font_md);
    SIZE sz;
    GetTextExtentPoint32A(g_dc, s, n, &sz);
    int px = (int)(x * S), py = (int)(y * S) - sz.cy / 2;     // y = vertical centre
    if (align == OV_CENTER)
        px -= sz.cx / 2;
    else if (align == OV_RIGHT)
        px -= sz.cx;
    RECT rc = {px - 2, py - 2, px + sz.cx + 4, py + sz.cy + 2};
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > W) rc.right = W;
    if (rc.bottom > H) rc.bottom = H;
    if (rc.left >= rc.right || rc.top >= rc.bottom)
        return;
    GdiFlush();
    for (int yy = rc.top; yy < rc.bottom; yy++)
        memset(&g_mask[yy * W + rc.left], 0, (rc.right - rc.left) * 4);
    SetTextColor(g_dc, RGB(255, 255, 255));
    TextOutA(g_dc, px, py, s, n);
    GdiFlush();
    // drop shadow first: the text sits on top of the game's own menus
    for (int yy = rc.top; yy < rc.bottom; yy++)
        for (int xx = rc.left; xx < rc.right; xx++) {
            unsigned cov = g_mask[yy * W + xx] & 255;
            if (cov && xx + S < W && yy + S < H)
                blend_px(&g_px[(yy + S) * W + xx + S], 0xC0000000, cov);
        }
    for (int yy = rc.top; yy < rc.bottom; yy++)
        for (int xx = rc.left; xx < rc.right; xx++) {
            unsigned cov = g_mask[yy * W + xx] & 255;
            if (cov)
                blend_px(&g_px[yy * W + xx], argb, cov);
        }
}

static bool g_panel;                                    // a port panel (Options Video help) is showing

void ov_panel(bool on) { g_panel = on; }

void ov_end(bool visible)
{
    if (!visible && g_panel)                            // the netplay menu's per-frame "nothing to show"
        return;
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }
    EnterCriticalSection(&g_lock);
    if (visible && g_px)
        memcpy(g_pub, g_px, W * H * 4);
    g_pub_dirty = visible;
    g_pub_visible = visible;
    LeaveCriticalSection(&g_lock);
}

static const char k_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n";

static bool init_gpu()
{
    if (g_vs)
        return true;
    if (g_gpu_failed)
        return false;
    ID3D11Device *dev = g_d3d.dev;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(k_hlsl, sizeof k_hlsl - 1, "overlay", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(k_hlsl, sizeof k_hlsl - 1, "overlay", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &err))) {
        port_log("overlay: shader compile failed: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        g_gpu_failed = true;
        return false;
    }
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
    vs->Release();
    ps->Release();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateTexture2D(&td, nullptr, &g_tex);
    dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;                 // premultiplied
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = 15;
    dev->CreateBlendState(&bd, &g_blend);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_raster);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_depth);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sd, &g_sampler);
    return g_vs && g_ps && g_srv && g_blend;
}

// d3d_present: composite the latest overlay frame over the back buffer
void overlay_draw()
{
    if (!g_lock_init || !g_pub_visible || !g_d3d.present_rtv || !init_gpu())
        return;
    prof_gpu_pass(GP_OVERLAY);
    ID3D11DeviceContext *ctx = g_d3d.ctx;
    EnterCriticalSection(&g_lock);
    if (g_pub_dirty) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < H; y++)
                memcpy((BYTE *)m.pData + y * m.RowPitch, g_pub + y * W, W * 4);
            ctx->Unmap(g_tex, 0);
        }
        g_pub_dirty = false;
    }
    LeaveCriticalSection(&g_lock);

    ID3D11RenderTargetView *old_rtv = nullptr;
    ID3D11DepthStencilView *old_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    D3D11_VIEWPORT old_vp;
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &old_vp);

    ID3D11Texture2D *bb;
    g_d3d.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    bb->Release();
    D3D11_VIEWPORT vp = {0, 0, (float)td.Width, (float)td.Height, 0, 1};
    ctx->OMSetRenderTargets(1, &g_d3d.present_rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_srv);
    ctx->PSSetSamplers(0, 1, &g_sampler);
    float f[4] = {};
    ctx->OMSetBlendState(g_blend, f, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depth, 0);
    ctx->RSSetState(g_raster);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (nvp)
        ctx->RSSetViewports(1, &old_vp);
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
}
