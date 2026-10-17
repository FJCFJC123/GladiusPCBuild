// Engine shader assembly (xvs.1.1 / xps.1.1 source text) -> HLSL. See shader_translate.cpp.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// The engine's generated per-vertex lighting (directional light list in the constants), recognised so it can be
// evaluated per pixel instead: oD0 = emissive + ambient * matAmbient + sum max(N.-L, 0) * matDiffuse * lightColour
// (x COLOR0), optional sphere-map UVs oT1.xy = dp4(normal, worldview rows 0/1). Constant numbers are Xbox slots.
struct VsLighting {
    bool ok;
    int nlights;
    bool color0;                    // result multiplied by the vertex colour
    bool env;                       // oT1.xy from the lighting normal
    int first, amb, diff, emis, wv; // light list head, material colours, worldview matrix
    std::string normal, color;      // registers: lighting normal (r8), vertex colour input (v2)
    std::string pos;                // register the world-view-projection is applied to (r4); "" = unknown
    // ambient-only (baked) shaders - the arenas: oD0 = (emissive + ambient * matAmbient) x COLOR0, no lights. With a
    // streamed normal they get a sun term per pixel (ok stays false: these are not "lit" draws)
    bool relight;
    std::string vpos, vnormal;      // input registers: position (v0), normal (v1)
};
bool analyze_lighting(const std::string &src, VsLighting *out);

// formats[16]: vertex attribute format per input register v0-v15 (NV2A: type | size << 4; 0x02 = disabled).
// lit: a recognised lighting shader - also outputs the lighting normal and vertex colour (TEXCOORD4 / 5).
std::string translate_vs(const std::string &src, const DWORD formats[16], std::vector<std::string> *warnings,
                         const VsLighting *lit = nullptr);
// lit: evaluate that vertex shader's lighting per pixel (reads the vertex constants at b0) in place of v0 / t1
std::string translate_ps(const std::string &src, std::vector<std::string> *warnings, const VsLighting *lit = nullptr);
