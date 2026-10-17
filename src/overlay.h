// Port-side 2D overlay (overlay.cpp): game 640x480 coordinates, straight ARGB colours.
#pragma once
#include <stdint.h>

enum { OV_LEFT, OV_CENTER, OV_RIGHT };

void ov_begin();                                     // start a frame (clears to transparent)
void ov_box(float x, float y, float w, float h, uint32_t argb);
void ov_frame(float x, float y, float w, float h, float thickness, uint32_t argb);
void ov_circle(float cx, float cy, float r, uint32_t argb);
void ov_line(float x0, float y0, float x1, float y1, float thickness, uint32_t argb);   // editor wire boxes
void ov_text(const char *s, float x, float y, int align, uint32_t argb, bool sm);   // y = vertical centre
float ov_text_width(const char *s, bool sm);
void ov_end(bool visible);                           // publish (visible = shown from the next Present)
void ov_panel(bool on);                              // a port panel owns the overlay: other callers cannot hide it
void overlay_draw();                                 // d3d_present, before Present
