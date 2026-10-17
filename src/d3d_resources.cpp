// Xbox D3D8 resources on the shim side: headers keep the Xbox layout the game reads/writes; Data holds a real
// pointer into "physical" memory (>= 0x80000000). D3D11 objects get attached in R1 (textures) / R2 (buffers).
#include "d3d.h"
#include "container.h"
#include <set>

// Xbox D3DFORMAT (bits 8-15 of Format): bits per pixel, linear?
typedef XFormatInfoPublic XFormatInfo;
XFormatInfo d3d_xformat(DWORD f)
{
    switch (f) {
    case 0x00: case 0x01: case 0x0B: case 0x19: return {8, false, false};
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x28: case 0x2C: case 0x2D: case 0x32:
        return {16, false, false};
    case 0x06: case 0x07: case 0x2A: case 0x2B: case 0x33: case 0x3A: return {32, false, false};
    case 0x0C: return {4, false, true};                                 // DXT1
    case 0x0E: case 0x0F: return {8, false, true};                       // DXT2/3, DXT4/5
    case 0x13: case 0x1B: case 0x1F: return {8, true, false};
    case 0x10: case 0x11: case 0x16: case 0x17: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25:
    case 0x30: case 0x31: case 0x35: return {16, true, false};
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x3F: return {32, true, false};
    }
    port_log("D3D: unknown Xbox format %02X\n", f);
    return {32, false, false};
}

static int ilog2(UINT v)
{
    int r = 0;
    while ((1u << r) < v)
        r++;
    return r;
}

static DWORD level_bytes(const XFormatInfo &fi, UINT w, UINT h)
{
    if (fi.dxt)
        return ((w + 3) / 4) * ((h + 3) / 4) * (fi.bpp * 2);           // 8 or 16 bytes per 4x4 block
    return w * h * fi.bpp / 8;
}

static void dims(const XPixelContainer *pc, UINT *w, UINT *h, UINT *levels)
{
    if (pc->Size) {
        *w = (pc->Size & 0xFFF) + 1;
        *h = ((pc->Size >> 12) & 0xFFF) + 1;
    } else {
        *w = 1u << ((pc->Format >> 20) & 0xF);
        *h = 1u << ((pc->Format >> 24) & 0xF);
    }
    *levels = (pc->Format >> 16) & 0xF;
    if (!*levels)
        *levels = 1;
}

static DWORD level_offset(const XPixelContainer *pc, UINT level, UINT *pitch)
{
    UINT w, h, levels;
    dims(pc, &w, &h, &levels);
    XFormatInfo fi = d3d_xformat((pc->Format >> 8) & 0xFF);
    DWORD off = 0;
    for (UINT l = 0; l < level; l++) {
        off += level_bytes(fi, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (pitch)
        *pitch = fi.dxt ? ((w + 3) / 4) * fi.bpp * 2 : w * fi.bpp / 8;
    return off;
}

static std::set<XResource *> g_owned;                 // headers allocated by the shim (freed on last Release)
static XSurface g_backbuffer, g_depth;

static void init_static_surfaces()
{
    if (g_backbuffer.Common)
        return;
    for (XSurface *s : {&g_backbuffer, &g_depth}) {
        s->Common = XCOMMON_TYPE_SURFACE | 1;                            // not in g_owned: never freed
        s->Size = (g_d3d.width - 1) | ((g_d3d.height - 1) << 12) | (((g_d3d.width * 4 + 63) / 64 - 1) << 24);
    }
    g_backbuffer.Format = 0x12 << 8 | 0x20;           // LIN_A8R8G8B8, 2D
    g_depth.Format = 0x2E << 8 | 0x20;                // LIN_D24S8
    g_backbuffer.Data = (DWORD)xphys_alloc_port(g_d3d.width * g_d3d.height * 4);   // window-sized: per PC
    g_depth.Data = (DWORD)xphys_alloc_port(g_d3d.width * g_d3d.height * 4);
}

extern "C" {

XSurface *__stdcall d3d_D3DDevice_GetRenderTarget2()
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_GetRenderTarget2");
    init_static_surfaces();
    return &g_backbuffer;
}

XSurface *__stdcall d3d_D3DDevice_GetDepthStencilSurface2()
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_GetDepthStencilSurface2");
    init_static_surfaces();
    return &g_depth;
}

XPixelContainer *__stdcall d3d_D3DDevice_CreateTexture2(UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                                        DWORD format, DWORD rtype)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_CreateTexture2");
    XFormatInfo fi = d3d_xformat(format & 0xFF);
    if (!levels) {
        levels = 1;
        for (UINT m = w > h ? w : h; m > 1; m >>= 1)
            levels++;
    }
    XPixelContainer *t = (XPixelContainer *)game_alloc(sizeof(XSurface));
    t->Common = XCOMMON_TYPE_TEXTURE | 1;
    t->Format = (format & 0xFF) << 8 | 0x20 | (levels & 0xF) << 16;
    if (fi.linear) {
        t->Size = (w - 1) | ((h - 1) << 12) | (((w * fi.bpp / 8 + 63) / 64 - 1) << 24);
    } else {
        t->Format |= ilog2(w) << 20 | ilog2(h) << 24;
    }
    DWORD total = 0;
    for (UINT l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1)
        total += level_bytes(fi, lw, lh);
    t->Data = (DWORD)xphys_alloc(total);
    g_owned.insert(t);
    return t;
}

// D3DDevice_CreateSurface2(Width, Height, Usage, Format): a standalone image surface (save-game code)
XSurface *__stdcall d3d_D3D_CreateSaveSurface(UINT w, UINT h, DWORD usage, DWORD format)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_CreateSurface2");
    XFormatInfo fi = d3d_xformat(format & 0xFF);
    XSurface *s = (XSurface *)game_alloc(sizeof(XSurface));
    s->Common = XCOMMON_TYPE_SURFACE | 1;
    s->Format = (format & 0xFF) << 8 | 0x20 | 1 << 16;
    if (fi.linear)
        s->Size = (w - 1) | ((h - 1) << 12) | (((w * fi.bpp / 8 + 63) / 64 - 1) << 24);
    else
        s->Format |= ilog2(w) << 20 | ilog2(h) << 24;
    s->Data = (DWORD)xphys_alloc(level_bytes(fi, w, h));
    g_owned.insert(s);
    return s;
}

// Get2DSurfaceDesc(container, level, D3DSURFACE_DESC *): Format, Type, Usage, Size, MultiSampleType, Width, Height
void __stdcall d3d_D3D_Get2DSurfaceDesc(XPixelContainer *c, UINT level, DWORD *desc)
{
    D3DLock lock;
    UINT w, h, levels;
    dims(c, &w, &h, &levels);
    w = w >> level ? w >> level : 1;
    h = h >> level ? h >> level : 1;
    DWORD fmt = (c->Format >> 8) & 0xFF;
    desc[0] = fmt;
    desc[1] = (c->Common & XCOMMON_TYPE_MASK) == XCOMMON_TYPE_SURFACE ? 1 : 3;   // D3DRTYPE_SURFACE / TEXTURE
    desc[2] = 0;
    desc[3] = level_bytes(d3d_xformat(fmt), w, h);
    desc[4] = 0x0011;                                   // D3DMULTISAMPLE_NONE
    desc[5] = w;
    desc[6] = h;
}

XSurface *__stdcall d3d_D3DTexture_GetSurfaceLevel2(XPixelContainer *tex, UINT level)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DTexture_GetSurfaceLevel2");
    XSurface *s = (XSurface *)game_alloc(sizeof(XSurface));
    UINT w, h, levels;
    dims(tex, &w, &h, &levels);
    UINT pitch;
    DWORD off = level_offset(tex, level, &pitch);
    w >>= level;
    h >>= level;
    if (!w) w = 1;
    if (!h) h = 1;
    s->Common = XCOMMON_TYPE_SURFACE | 1;
    s->Data = tex->Data + off;
    s->Format = tex->Format & ~0x000F0000 | 0x00010000;
    s->Size = (w - 1) | ((h - 1) << 12) | (((pitch + 63) / 64 - 1) << 24);
    s->Parent = tex;
    g_owned.insert(s);
    tex->Common++;
    return s;
}

struct XLockedRect {
    INT Pitch;
    void *pBits;
};

HRESULT __stdcall d3d_D3DTexture_LockRect(XPixelContainer *tex, UINT level, XLockedRect *out, const RECT *rect,
                                          DWORD flags)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DTexture_LockRect");
    UINT pitch;
    DWORD off = level_offset(tex, level, &pitch);
    if (tex->Size)
        pitch = ((tex->Size >> 24) + 1) * 64;            // linear: the header's pitch
    d3d_texture_dirty(tex);
    out->Pitch = pitch;
    out->pBits = (BYTE *)(tex->Data + off);
    return S_OK;
}

void __stdcall d3d_D3DResource_Register(XResource *res, void *base)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DResource_Register");
    res->Data += (DWORD)base;                         // Data holds the offset set by XGSet*Header
}

DWORD __stdcall d3d_D3DResource_AddRef(XResource *res)
{
    D3DLock lock;
    d3d_count("D3DResource_AddRef");
    return ++res->Common & XCOMMON_REFCOUNT_MASK;
}

DWORD __stdcall d3d_D3DResource_Release(XResource *res)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DResource_Release");
    DWORD refs = (res->Common & XCOMMON_REFCOUNT_MASK);
    if (refs)
        res->Common--, refs--;
    if (!refs && g_owned.erase(res)) {
        DWORD type = res->Common & XCOMMON_TYPE_MASK;
        if (type == XCOMMON_TYPE_SURFACE && ((XSurface *)res)->Parent)
            d3d_D3DResource_Release(((XSurface *)res)->Parent);
        else
            xphys_free((void *)res->Data);
        game_free(res);
    }
    return refs;
}

void *__stdcall d3d_D3DVertexBuffer_Lock2(XResource *vb, DWORD flags)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DVertexBuffer_Lock2");
    d3d_vb_invalidate(vb->Data);                        // contents about to change: re-verify on next draw
    return (void *)vb->Data;
}

// Xbox push-buffer resource (CRI side): header 0x18 bytes, data inline when `inline_data`
XResource *__stdcall d3d_D3DDevice_CreatePushBuffer2(DWORD size, DWORD inline_data)
{
    D3DLock lock;
    d3d_drain();
    d3d_count("D3DDevice_CreatePushBuffer2");
    BYTE *h = (BYTE *)game_alloc(0x18 + (inline_data ? size : 0));
    XResource *r = (XResource *)h;
    r->Common = 0x01020001 | (inline_data ? 0x80000000 : 0);
    r->Data = inline_data ? (DWORD)(h + 0x18) : (DWORD)xphys_alloc(size);
    *(DWORD *)(h + 0x10) = size;
    port_log("CreatePushBuffer2(size %u, inline %u) -> %p\n", size, inline_data, r);
    return r;
}

}
