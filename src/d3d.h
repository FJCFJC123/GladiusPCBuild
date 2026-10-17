// Xbox D3D8 -> D3D11 shim: shared declarations. The game calls Xbox D3D8 entry points (tools/d3d_table.py);
// xbe2pe.py redirects each to d3d_<Name> here. No d3d8.dll involved - D3D11 only.
#pragma once
#include "port.h"
#include <d3d11.h>
#include <dxgi.h>
#include <string>

// Xbox D3D static device object (engine writes push data through it directly)
#define XBE_D3D_DEVICE_PUT      0x2C5670   // push-buffer write pointer
#define XBE_D3D_DEVICE_LIMIT    0x2C5674   // engine calls MakeSpace when put >= limit
#define XBE_D3D_G_PDEVICE       0x2C5668   // D3D_g_pDevice

// Xbox resource headers (D3DResource / D3DPixelContainer / D3DSurface)
struct XResource {
    DWORD Common;        // refcount (low 16), type (bits 16-18), flags
    DWORD Data;          // pixel/vertex memory (here: a real pointer)
    DWORD Lock;
};
struct XPixelContainer : XResource {
    DWORD Format;        // dma/cube/border/dims/D3DFMT(8-15)/levels(16-19)/log2 u,v,p
    DWORD Size;          // linear only: width-1 (0-11), height-1 (12-23), pitch/64-1 (24-31)
};
struct XSurface : XPixelContainer {
    XResource *Parent;
};
enum {
    XCOMMON_REFCOUNT_MASK = 0x0000FFFF,
    XCOMMON_TYPE_MASK = 0x00070000,
    XCOMMON_TYPE_VERTEXBUFFER = 0x00000000, XCOMMON_TYPE_INDEXBUFFER = 0x00010000,
    XCOMMON_TYPE_PUSHBUFFER = 0x00020000, XCOMMON_TYPE_PALETTE = 0x00030000,
    XCOMMON_TYPE_TEXTURE = 0x00040000, XCOMMON_TYPE_SURFACE = 0x00050000,
};

// Xbox D3DFORMAT (Format bits 8-15) facts
struct XFormatInfoPublic {
    BYTE bpp;
    bool linear, dxt;
};
XFormatInfoPublic d3d_xformat(DWORD fmt);
void d3d_texture_dirty(XPixelContainer *t);            // contents changed (LockRect)
void d3d_vb_invalidate(DWORD data);              // vertex buffer locked for writing (d3d_program.cpp)
void d3d_textures_all_dirty();                   // game memory was replaced (netplay state sync): re-read every texture
void d3d_vb_invalidate_all();                    // ... and every vertex stream
void d3d_nv_method(DWORD method, const DWORD *data, DWORD count);   // NV2A state from push / simple states

// shaders assembled by the engine at runtime (xg_shader.cpp): id -> source text
struct XgShader {
    bool pixel;
    std::string source;
};
const XgShader *xg_shader(DWORD id);
DWORD xg_tag_vs(const BYTE *microcode);                 // id from a tagged program, 0xFFFFFFFF if none
DWORD xg_tag_vs_hash(const BYTE *microcode);            // source hash from a tagged program, 0 if none
DWORD xg_hash_id(DWORD hash);                           // source hash -> local id (0xFFFFFFFF if unknown)
DWORD xg_tag_ps(const DWORD *psdef);

// renderer internals shared by d3d_render.cpp / d3d_program.cpp
ID3D11ShaderResourceView *d3d_texture_srv(DWORD data, DWORD format, DWORD size);
// tex_replace.cpp: HD view for this texture, nullptr = none (*pending: still loading, ask again later)
ID3D11ShaderResourceView *tex_replace_srv(DWORD data, DWORD format, DWORD size, bool *pending);
DWORD d3d_texel(DWORD data, DWORD format, UINT x, UINT y);   // level-0 texel as BGRA (swizzled / DXT)
std::string tex_key(DWORD data, DWORD format);   // tex_replace.cpp: asset name of a texture ("" unknown)
// d3d_shadow.cpp: sun shadow maps from recorded opaque draws
#include <vector>
bool d3d_shadows_on();
void d3d_shadow_init();
bool d3d_invert4(const float m[16], float out[16]);     // d3d_shadow.cpp, row-major 4x4
bool d3d_shadow_lookup(const float wvp[16], float S[16], ID3D11ShaderResourceView **srv, ID3D11SamplerState **cmp,
                       float *texel, float *strength);  // lit draws: surface -> last frame's shadow map
bool d3d_sun_in_object(const float wvp[16], float out[3]);   // relight draws: sun travel direction, object space
void d3d_taa_frame_begin();                              // d3d_taa.cpp: Present (next frame's jitter)
void d3d_taa_jitter(float *xy);                          // this draw's jitter (NDC), 0 after the resolve
void d3d_taa_mark_3d();                                  // fx_render_3d returned
void d3d_taa_before_draw(bool depth_test);               // resolve at the first 2D draw
void d3d_taa_before_final();                             // or at the final pass
bool d3d_camera_this_frame(float P[16], float R[16]);    // d3d_shadow.cpp: projection, world -> view
ID3D11ShaderResourceView *d3d_bloom_run(ID3D11ShaderResourceView *scene, UINT w, UINT h);   // d3d_bloom.cpp
bool d3d_object_to_view(const float wvp[16], float M[16]);   // P^-1 * WVP with last frame's projection
void d3d_shadow_install_cull();                         // wider engine culling while shadows are on
void d3d_shadow_record(ID3D11VertexShader *vs, ID3D11InputLayout *layout, ID3D11Buffer *const bufs[16],
                       const UINT strd[16], const UINT offs[16], D3D11_PRIMITIVE_TOPOLOGY topo,
                       const std::vector<UINT> &indices, const float *vsc, ID3D11ShaderResourceView *tex0, float aref,
                       const char *name, bool dynamic, ID3D11Buffer *ib, DWORD consts_gen);   // ib: indices already on the GPU; dynamic: never cached (characters)
void d3d_shadow_light(const float *vsc, int first, int nlights, int wv);
void d3d_shadow_apply();
void d3d_shadow_frame_end();
void d3d_install_fx_hooks();                           // particle renderer bracket (soft particles)
void d3d_begin_draw(bool pretransformed, bool soft = false);   // targets + blend/depth/raster from NV2A state;
                                                       // soft: G-buffer as texture t4 instead of a target
bool d3d_ready();
DWORD d3d_nv(DWORD method);                            // current NV2A state value
void d3d_stage_set(int stage, DWORD data, DWORD format, DWORD size);
void d3d_state_invalidate();                           // something else (overlay) changed the bound D3D11 state
void d3d_scene_targets_changed();                      // scene target size changed: rebuild dependent render state

// D3D entry points are called from several threads (engine, CRI Sofdec server); one recursive lock serializes them.
// Every d3d_* entry starts with `D3DLock lock;`.
void d3d_lock();
void d3d_unlock();
struct D3DLock {
    D3DLock() { d3d_lock(); }
    ~D3DLock() { d3d_unlock(); }
};

struct D3DState {
    HWND hwnd;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *rtv;        // scene render target: game draws here
    ID3D11Texture2D *scene_tex;
    ID3D11ShaderResourceView *scene_srv;
    ID3D11RenderTargetView *present_rtv; // swap-chain render target: final pass / overlay
    UINT width, height;            // game back buffer size (Xbox present parameters)
    UINT game_ss_x = 1, game_ss_y = 1; // Xbox-requested screen-space factors
    UINT render_scale = 1;         // user render scale on top of game_ss_*
    bool auto_render_scale;        // choose integer render_scale from the window client size
    UINT ss_x = 1, ss_y = 1;       // supersample factors: shader screen space = back buffer size * these
    UINT scene_width, scene_height; // internal scene target size
    DWORD frame;
    void (__cdecl *vblank_callback)(DWORD);
    UINT bb_width, bb_height;      // swap-chain back buffer (window client size at creation)
    bool stats_frame;              // this frame's call statistics are logged (d3d_frame_stats)
    DWORD draws;                   // draw calls submitted this frame
    int final_filter;              // 0 point, 1 linear
    float sharpen;                 // final-pass unsharp amount, 0 = off
    int texture_filter;            // 0 point, 1 linear, 2 anisotropic
    UINT anisotropy;               // texture sampler max anisotropy
    int anti_alias;                // anti-aliasing: 0 off, 1 FXAA, 2 TAA (d3d_taa.cpp)
    bool pixel_lighting;           // engine vertex lighting evaluated per pixel ([video] Lighting, F8)
    float specular, spec_power, rim; // per-pixel extras ([video] Specular / SpecPower / Rim; 0 = off)
    float sun_relight;             // [video] SunRelight: sun term on baked (arena) surfaces, 0 = off
    float bump;                    // [video] Bump: relief from texture luminance (surface units), 0 = off
    float fx_lights;               // [video] FxLights: intensity of lights from additive effects, 0 = off
    bool hdr;                      // [video] HDR: float scene target, overbright rolled off in the final pass
    float materials;               // [video] Materials: water / lava / glow / foliage / cloth effects, 0 = off
    float bloom, bloom_threshold;  // [video] Bloom (intensity, 0 = off), BloomThreshold (scene level that starts to bloom)
    float saturation, contrast, warmth, vignette;   // [video] grading, neutral 1 / 1 / 0 / 0
    float god_rays;                // [video] GodRays: sun shafts through gaps (needs Bloom > 0), 0 = off
    float hdr_white;               // [video] HdrWhite: scene level that reaches pure white
    // G-buffer: linear view depth (SV_Position.w) of opaque program draws, render target 1 next to the scene colour
    ID3D11Texture2D *gbuf_tex;
    ID3D11RenderTargetView *gbuf_rtv;
    ID3D11ShaderResourceView *gbuf_srv;
    // G-buffer normals (render target 2): view-space normal of lit / relit draws, alpha 1 = present
    ID3D11Texture2D *gnrm_tex;
    ID3D11RenderTargetView *gnrm_rtv;
    ID3D11ShaderResourceView *gnrm_srv;
    bool soft_particles;           // [video] SoftParticles: blended FX fade where they meet geometry
    float soft_range;              // [video] SoftRange: view-depth distance of the fade
    float show_depth;              // [debug] ShowDepth=scale: final pass shows the G-buffer depth (0 = off)
    bool in_fx;                    // inside the engine's 3D particle renderer (fx_Render): soft-particle candidates
    // live toggles (window hotkeys F4-F9, state in the title bar): ini sets the start value
    bool ssao;                     // [video] SSAO (F6)
    bool shadows;                  // [video] Shadows (F5)
    bool hd_textures;              // [textures] Pack loaded and in use (F4)
};
extern D3DState g_d3d;

void d3d_window_start();
bool d3d_device_create(UINT width, UINT height);
void d3d_present();

// push-buffer capture: the engine writes NV2A methods into our buffer; decoded on every shim call
void d3d_push_init();
void d3d_drain();
DWORD *d3d_push_space(DWORD dwords);

// logging stub support (d3d_stubs.cpp) and per-frame call statistics
void d3d_stub_call(DWORD addr, const char *name, const char *fmt, ...);
void d3d_count(const char *name);
void d3d_frame_stats();
bool d3d_tracing();                         // [debug] TraceFrame=N
void d3d_trace(const char *fmt, ...);
void d3d_trace_init();

// "physical" (GPU-visible on Xbox) memory: allocated >= 0x80000000 so the game's Data|0x80000000 is a no-op
void *xphys_alloc(SIZE_T size);
void *xphys_alloc_port(SIZE_T size);   // port-sized memory (window back buffer): never in the container
void xphys_free(void *p);
