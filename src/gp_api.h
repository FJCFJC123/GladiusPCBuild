// gladiusport.dll exports used by gladiusnet.dll (gladiusnet_api.h). Force-included into every gladiusport.dll
// source (build.bat /FI) so each definition below is exported; gladiusnet.dll builds with GLADIUSNET_BUILD and
// imports them. Plain C types only across the boundary: both DLLs link the CRT statically (separate heaps).
// Changing anything here means a new GLADIUSNET_ABI.
#pragma once
#ifdef __cplusplus
#include <stdint.h>
#include <stddef.h>

#ifdef GLADIUSNET_BUILD                  // functions resolve through the import library; data must be dllimport
#define GP_API
#define GP_DATA __declspec(dllimport)
#else
#define GP_API __declspec(dllexport)
#define GP_DATA __declspec(dllexport)
#endif

struct GnCtx;

// gn_core.cpp: deterministic mode, I/O gate, session trace, logging (exported data: gn.h, d3d_prof.h)
GP_API void det_enter(GnCtx *c);
GP_API void det_leave(GnCtx *c);
GP_API void io_wait();
GP_API void pace();
GP_API void fpu_enforce(uint32_t frame);
GP_API void trace_open(GnCtx *c, const char *name);
GP_API void trace_close(GnCtx *c);
GP_API void dtrace(const char *fmt, ...);
GP_API void logf(const char *fmt, ...);
GP_API void path_in_dir(char *out, const char *name);

// port_main.cpp, mods.cpp, xapi_file.cpp, save_pages.cpp, d3d_core.cpp
GP_API void port_log(const char *fmt, ...);
GP_API const char *port_ini_name();
GP_API uint32_t port_mods_crc();
GP_API void port_screenshot(const char *path);
GP_API const char *port_u_dir();
GP_API void port_set_u_override(const char *dir);
GP_API void port_set_u_resolver(void (*fn)());
GP_API void port_save_rescan();

// memory container, virtual handles (state sync)
GP_API bool container_on();
GP_API bool container_arena(int arena, uintptr_t *base, size_t *size);
GP_API bool container_range(int arena, uintptr_t *base, uintptr_t *end);
GP_API int container_local_blocks_n(uint32_t *pairs, int max);   // (addr, size) pairs, sorted; returns the total
GP_API void container_tags(void (*cb)(uintptr_t addr, uint32_t size, const uint32_t *stack, void *ctx), void *ctx);
GP_API void vh_publish();
GP_API void vh_reconcile();

// renderer, audio
GP_API void d3d_textures_all_dirty();
GP_API void d3d_vb_invalidate_all();
GP_API void d3d_invalidate_ranges(const uint32_t *pairs, int n);  // (addr, size) pairs a state sync rewrote
GP_API void dsound_audio_stats(uint32_t out[3]);
GP_API void dsound_sync_pause(bool on);
GP_API void dsound_sync_freeze(bool on);
GP_API int dsound_sync_touched_n(uint32_t *pairs, int max);      // (addr, size) pairs; returns the total

// overlay (lobby screens)
GP_API void ov_begin();
GP_API void ov_box(float x, float y, float w, float h, uint32_t argb);
GP_API void ov_frame(float x, float y, float w, float h, float thickness, uint32_t argb);
GP_API void ov_circle(float cx, float cy, float r, uint32_t argb);
GP_API void ov_text(const char *s, float x, float y, int align, uint32_t argb, bool sm);
GP_API void ov_end(bool visible);
#endif
