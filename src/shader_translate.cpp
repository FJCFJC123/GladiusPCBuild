// Engine shader source (Xbox xvs.1.1 / xps.1.1 assembly text, generated at runtime) -> HLSL (shader model 4).
//
// Vertex: registers v0-v15 (inputs, decoded per vertex attribute format), r0-r11 temps, r12 == oPos, c[0..191]
// (Xbox c-96..95 = slot +96), a0.x. "#pragma screenspace" programs end with oPos.xyz = clip.xyz*c94/w + c95
// (viewport done in the shader); pixels are mapped back to clip space over the whole render target.
// Pixel: Xbox register-combiner assembly (tex, mov/add/sub/mul/mad/lrp/dp3/dp4/cnd, xmma/xmmc/xdm/xdd, xfc).
#include "shader_translate.h"
#include <map>
#include <vector>
#include <sstream>
#include <cctype>

// ------------------------------------------------------------------------------------------------ text helpers
static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::vector<std::string> split_operands(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char ch : s) {
        if (ch == '[') depth++;
        if (ch == ']') depth--;
        if (ch == ',' && !depth) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!trim(cur).empty())
        out.push_back(trim(cur));
    return out;
}

static bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

// whole-word macro substitution, repeated until stable (macros reference macros)
static std::string expand_macros(const std::string &line, const std::map<std::string, std::string> &defs)
{
    std::string s = line;
    for (int pass = 0; pass < 8; pass++) {
        std::string out;
        bool changed = false;
        for (size_t i = 0; i < s.size();) {
            if (is_ident(s[i]) && (i == 0 || !is_ident(s[i - 1]))) {
                size_t j = i;
                while (j < s.size() && is_ident(s[j]))
                    j++;
                std::string word = s.substr(i, j - i);
                auto it = defs.find(word);
                if (it != defs.end()) {
                    out += it->second;
                    changed = true;
                } else {
                    out += word;
                }
                i = j;
            } else {
                out += s[i++];
            }
        }
        s = out;
        if (!changed)
            break;
    }
    return s;
}

// lines without comments/defines, macros expanded; collects #define and #pragma
static std::vector<std::string> preprocess(const std::string &src, bool *screenspace,
                                           std::map<std::string, std::string> *defs_out = nullptr)
{
    std::map<std::string, std::string> defs;
    std::vector<std::string> lines;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        c = line.find(';');
        if (c != std::string::npos) line.erase(c);
        line = trim(line);
        if (line.empty())
            continue;
        if (line[0] == '#') {
            std::istringstream ls(line);
            std::string kw, name, rest;
            ls >> kw >> name;
            std::getline(ls, rest);
            if (kw == "#define")
                defs[name] = trim(rest);
            else if (kw == "#pragma" && name == "screenspace" && screenspace)
                *screenspace = true;
            continue;
        }
        lines.push_back(expand_macros(line, defs));
    }
    if (defs_out)
        *defs_out = defs;
    return lines;
}

static std::string swizzle4(const std::string &sw)
{
    if (sw.empty())
        return "";
    std::string s;
    for (char ch : sw) {
        switch (ch) {
        case 'r': s += 'x'; break;
        case 'g': s += 'y'; break;
        case 'b': s += 'z'; break;
        case 'a': s += 'w'; break;
        default: s += ch;
        }
    }
    while (s.size() < 4)
        s += s.back();
    return "." + s;
}

static std::string mask_of(const std::string &m)
{
    std::string s;
    for (char ch : m) {
        switch (ch) {
        case 'r': s += 'x'; break;
        case 'g': s += 'y'; break;
        case 'b': s += 'z'; break;
        case 'a': s += 'w'; break;
        default: s += ch;
        }
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ vertex shader
// constant index expression inside c[...]: "80+0", "a0.x+-96", "a0.x", "VS..." already expanded to numbers
static std::string const_index(const std::string &expr)
{
    std::string e;
    for (char ch : expr)
        if (ch != ' ' && ch != '\t')
            e += ch;
    bool rel = false;
    int sum = 0;
    size_t i = 0;
    while (i < e.size()) {
        int sign = 1;
        while (i < e.size() && (e[i] == '+' || e[i] == '-')) {
            if (e[i] == '-') sign = -sign;
            i++;
        }
        if (e.compare(i, 4, "a0.x") == 0) {
            rel = true;
            i += 4;
            continue;
        }
        size_t j = i;
        while (j < e.size() && isdigit((unsigned char)e[j]))
            j++;
        if (j == i) {
            i++;
            continue;
        }
        sum += sign * atoi(e.substr(i, j - i).c_str());
        i = j;
    }
    std::ostringstream o;
    if (rel)
        o << "c[clamp(a0 + " << (sum + 96) << ", 0, 191)]";
    else
        o << "c[" << (sum + 96) << "]";
    return o.str();
}

static std::string vs_reg(const std::string &r)
{
    if (r == "oPos" || r == "r12") return "oPos";
    if (r == "oD0" || r == "oD1" || r == "oB0" || r == "oB1" || r == "oFog" || r == "oPts") return r;
    if (r.size() >= 3 && r[0] == 'o' && r[1] == 'T') return r;
    if (r == "a0" || r == "a0.x") return "a0f";
    if (r[0] == 'c') {
        if (r.size() > 1 && r[1] == '[') return const_index(r.substr(2, r.size() - 3));
        return const_index(r.substr(1));
    }
    return r;                                          // vN, rN
}

// "-c[a0.x+1].xyz" -> HLSL float4 expression
static std::string vs_src(const std::string &s0)
{
    std::string s = trim(s0);
    bool neg = false;
    if (!s.empty() && s[0] == '-') {
        neg = true;
        s = trim(s.substr(1));
    }
    std::string reg = s, sw;
    size_t close = s.find(']');
    size_t dot = s.find('.', close == std::string::npos ? 0 : close);
    if (s.compare(0, 2, "a0") == 0)
        dot = std::string::npos;
    if (dot != std::string::npos) {
        reg = s.substr(0, dot);
        sw = s.substr(dot + 1);
    }
    std::string e = vs_reg(reg);
    if (e == "a0f")
        e = "float4(a0, a0, a0, a0)";
    e += swizzle4(sw);
    return neg ? "(-" + e + ")" : e;
}

struct VsCtx {
    std::ostringstream body;
    std::vector<std::string> warnings;
};

static void vs_assign(VsCtx &cx, const std::string &dst, const std::string &value)
{
    std::string reg = dst, mask;
    size_t dot = dst.find('.');
    if (dot != std::string::npos) {
        reg = dst.substr(0, dot);
        mask = mask_of(dst.substr(dot + 1));
    }
    if (reg == "a0") {                                 // mov a0.x, src  (round to nearest)
        cx.body << "    a0 = (int)floor((" << value << ").x + 0.5);\n";
        return;
    }
    std::string r = vs_reg(reg);
    if (r == "oFog" || r == "oPts") {
        cx.body << "    " << r << " = (" << value << ").x;\n";
        return;
    }
    if (mask.empty() || mask == "xyzw")
        cx.body << "    " << r << " = " << value << ";\n";
    else
        cx.body << "    " << r << "." << mask << " = (" << value << ")." << mask << ";\n";
}

static bool vs_instruction(VsCtx &cx, std::string line)
{
    line = trim(line);
    if (line[0] == '+')
        line = trim(line.substr(1));
    size_t sp = line.find_first_of(" \t");
    std::string op = sp == std::string::npos ? line : line.substr(0, sp);
    for (auto &ch : op) ch = (char)tolower(ch);
    if (op == "xvs.1.1" || op == "vs.1.1" || op == "xvss.1.1" || op == "xvsw.1.1" || op == "nop")
        return true;
    std::vector<std::string> a = split_operands(sp == std::string::npos ? "" : line.substr(sp + 1));
    auto S = [&](size_t i) { return i < a.size() ? vs_src(a[i]) : std::string("float4(0,0,0,0)"); };
    auto X = [&](size_t i) { return "(" + S(i) + ").x"; };
    std::string v;
    if (op == "mov") v = S(1);
    else if (op == "add") v = S(1) + " + " + S(2);
    else if (op == "sub") v = S(1) + " - " + S(2);
    else if (op == "mul") v = S(1) + " * " + S(2);
    else if (op == "mad") v = S(1) + " * " + S(2) + " + " + S(3);
    else if (op == "dp3") v = "dot((" + S(1) + ").xyz, (" + S(2) + ").xyz).xxxx";
    else if (op == "dp4") v = "dot(" + S(1) + ", " + S(2) + ").xxxx";
    else if (op == "dph") v = "(dot((" + S(1) + ").xyz, (" + S(2) + ").xyz) + (" + S(2) + ").w).xxxx";
    else if (op == "min") v = "min(" + S(1) + ", " + S(2) + ")";
    else if (op == "max") v = "max(" + S(1) + ", " + S(2) + ")";
    else if (op == "slt") v = "float4(" + S(1) + " < " + S(2) + ")";
    else if (op == "sge") v = "float4(" + S(1) + " >= " + S(2) + ")";
    else if (op == "rcp") v = "rcp_x(" + X(1) + ").xxxx";
    else if (op == "rcc") v = "rcc_x(" + X(1) + ").xxxx";
    else if (op == "rsq") v = "rsq_x(" + X(1) + ").xxxx";
    else if (op == "frc") v = "frac(" + S(1) + ")";
    else if (op == "expp" || op == "exp") v = "expp4(" + X(1) + ")";
    else if (op == "logp" || op == "log") v = "logp4(" + X(1) + ")";
    else if (op == "lit") v = "lit4(" + S(1) + ")";
    else if (op == "dst") v = "float4(1, (" + S(1) + ").y * (" + S(2) + ").y, (" + S(1) + ").z, (" + S(2) + ").w)";
    else if (op == "arl") { vs_assign(cx, "a0", S(1)); return true; }
    else if (op == "m4x4" || op == "m4x3" || op == "m3x4" || op == "m3x3" || op == "m3x2") {
        int rows = op[3] - '0', cols = op[1] - '0';
        std::string base = trim(a.size() > 2 ? a[2] : "c0");
        // base is cN or c[expr]: rows read consecutive constants
        std::string idx = base[1] == '[' ? base.substr(2, base.size() - 3) : base.substr(1);
        std::ostringstream e;
        e << "float4(";
        for (int r = 0; r < 4; r++) {
            if (r) e << ", ";
            if (r < rows) {
                std::string row = vs_reg("c[" + idx + "+" + std::to_string(r) + "]");
                e << (cols == 4 ? "dot(" + S(1) + ", " + row + ")"
                                : "dot((" + S(1) + ").xyz, (" + row + ").xyz)");
            } else {
                e << (r == 3 ? "1" : "0");
            }
        }
        e << ")";
        v = e.str();
    } else {
        cx.warnings.push_back("unknown vertex op '" + op + "'");
        return false;
    }
    vs_assign(cx, a.empty() ? "r0" : a[0], v);
    return true;
}

// HLSL input decl + decode for one attribute format (NV2A: type = fmt & 15, size = fmt >> 4)
static void vs_input(int slot, DWORD fmt, std::ostringstream &decl, std::ostringstream &decode)
{
    int type = fmt & 15, size = (fmt >> 4) & 15;
    char name[8];
    sprintf(name, "v%d", slot);
    if (type == 6) {                                   // CMP: packed 11:11:10 signed normal in one dword
        decl << "    uint i" << slot << " : V" << slot << ";\n";
        decode << "    " << name << " = float4(unpack_cmp(i." << "i" << slot << "), 1);\n";
        return;
    }
    if (type == 5) {                                   // S32K: unnormalized shorts
        decl << "    int" << (size > 2 ? 4 : 2) << " i" << slot << " : V" << slot << ";\n";
        decode << "    " << name << " = float4(" << (size > 2 ? "(float4)i.i" + std::to_string(slot)
                                                    : "(float2)i.i" + std::to_string(slot) + ", 0, 1") << ");\n";
        if (size == 1) decode << "    " << name << ".y = 0;\n";
        if (size == 3) decode << "    " << name << ".w = 1;\n";
        return;
    }
    decl << "    float4 i" << slot << " : V" << slot << ";\n";   // F / UB / S1 come in as float4 via the layout
    decode << "    " << name << " = i.i" << slot << ";\n";
    if (type == 2 || type == 1) {                      // missing components: 0, w = 1
        if (size < 4) decode << "    " << name << ".w = 1;\n";
        if (size < 3) decode << "    " << name << ".z = 0;\n";
        if (size < 2) decode << "    " << name << ".y = 0;\n";
    }
}

static const char *k_vs_prelude = R"(
cbuffer VSConst : register(b0) { float4 c[192]; float4 attr_const[16]; float4 rt_size; float4 vmat; };
float rcp_x(float x) { return x == 0 ? 1.#INF : 1.0 / x; }
float rcc_x(float x) { float r = rcp_x(x); float a = clamp(abs(r), 5.42101e-20, 1.84467e+19); return r < 0 ? -a : a; }
float rsq_x(float x) { x = abs(x); return x == 0 ? 1.#INF : rsqrt(x); }
float4 expp4(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1); }
float4 logp4(float x) { x = abs(x); if (x == 0) return float4(-1.#INF, 1, -1.#INF, 1);
                        float e = floor(log2(x)); return float4(e, x / exp2(e), log2(x), 1); }
float4 lit4(float4 s) { float d = max(s.x, 0); float p = clamp(s.w, -128, 128);
                        return float4(1, d, s.x > 0 ? pow(max(s.y, 0.0000001), p) : 0, 1); }
float3 unpack_cmp(uint u) {
    int x = (int)(u << 21) >> 21, y = (int)((u >> 11) << 21) >> 21, z = (int)(u >> 22) << 22 >> 22;
    return float3(x / 1023.0, y / 1023.0, z / 511.0);
}
)";
static const char *k_io_fields = "float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; "
                                 "float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG;";
static const char *k_io_lit = " float4 ln : TEXCOORD4; float4 lc : TEXCOORD5; float4 lp : TEXCOORD6;";

// ------------------------------------------------------------------------------------------------ lighting
// Recognise the engine's generated lighting block (by its own #define names, so the register / slot numbers come
// from the shader itself):
//   mov r11, c[EMISSIVE]                       mov a0.x, c[LIGHT_FIRST].w      mov r2, c[a0.x+0]
//   mad r11, r2, c[AMBIENT], r11               then per light:
//   mov a0.x, c[a0.x].w   dp3 N_DOT_L, NORMAL, -c[a0.x+1]   max N_DOT_L, N_DOT_L, ZERO
//   mul r2, N_DOT_L, c[DIFFUSE]                mad r11, r2, c[a0.x+0], r11     (the last one may write oD0)
//   mul oD0, r11, COLOR0                       (optional)
//   dp4 oT1.x / .y, NORMAL, c[WORLDVIEW+0/1]   (optional sphere-map UVs)
// Anything else writing oD0 afterwards, or a define missing, and the shader stays per-vertex.
static std::string squash(const std::string &s)
{
    std::string o;
    for (char ch : s)
        if (ch != ' ' && ch != '\t')
            o += ch;
    return o;
}

bool analyze_lighting(const std::string &src, VsLighting *out)
{
    *out = VsLighting();
    std::map<std::string, std::string> defs;
    std::vector<std::string> lines = preprocess(src, nullptr, &defs);
    auto def = [&](const char *name) -> std::string {
        auto it = defs.find(name);
        if (it == defs.end())
            return std::string();
        std::string v = it->second;                     // defines may reference defines
        for (int k = 0; k < 4 && defs.count(v); k++)
            v = defs[v];
        return squash(v);
    };
    std::string first = def("VS_LIGHT_FIRST_OFFSET"), amb = def("VS_MATERIAL_COLOR_AMBIENT"),
                diff = def("VS_MATERIAL_COLOR_DIFFUSE"), emis = def("VS_MATERIAL_COLOR_EMISSIVE"),
                wv = def("VS_WORLDVIEW_MATRIX"), ndl = def("VERTEX_REGISTER_N_DOT_L"),
                normal = def("VERTEX_NORMAL_FOR_LIGHTING"), color = def("VERTEX_STREAM_COLOR0");
    if (first.empty() || amb.empty() || diff.empty() || emis.empty() || ndl.empty() || normal.empty() || color.empty())
        return false;
    bool has_emis = false, has_head = false, has_amb = false;
    int nl = 0;
    std::string last_d0;                                // last instruction writing oD0
    bool env_x = false, env_y = false;
    std::string wvp = def("VS_WORLDVIEWPROJECTION_MATRIX"), pos;
    for (const std::string &l0 : lines) {
        std::string l = squash(l0);
        // the position the WVP is applied to: "mul oPos, r4.x, c[80+0]" (or "dp4 oPos.x, r4, c[80+0]")
        if (pos.empty() && !wvp.empty() && l.find(",c[" + wvp + "+0]") != std::string::npos) {
            if (!l.compare(0, 8, "muloPos,")) pos = l.substr(8, l.find('.', 8) - 8);
            else if (!l.compare(0, 11, "dp4oPos.x,")) pos = l.substr(10, l.find(',', 10) - 10);
        }
        if (l == "movr11,c[" + emis + "]") has_emis = true;
        else if (l == "mova0.x,c[" + first + "].w") has_head = true;
        else if (l == "madr11,r2,c[" + amb + "],r11") has_amb = true;
        else if (l == "dp3" + ndl + "," + normal + ",-c[a0.x+1]") nl++;
        if (!wv.empty() && l == "dp4oT1.x," + normal + ",c[" + wv + "+0]") env_x = true;
        if (!wv.empty() && l == "dp4oT1.y," + normal + ",c[" + wv + "+1]") env_y = true;
        size_t sp = l.find_first_of(",");
        std::string op_dst = l.substr(0, sp);
        if (op_dst.size() > 3 && (op_dst.compare(3, 3, "oD0") == 0))
            last_d0 = l;
    }
    if (!has_emis || !has_head || !has_amb || nl > 8)
        return false;
    if (nl == 0) {                                      // baked / ambient-only: relight candidate
        std::string vpos = def("VERTEX_STREAM_POSITION"), vnormal = def("VERTEX_STREAM_NORMAL");
        if (vpos.empty() || vnormal.empty() || wvp.empty() || pos != vpos)
            return false;                               // position must go straight into the WVP (object space)
        if (last_d0 == "muloD0,r11," + color) out->color0 = true;
        else if (last_d0 != "movoD0,r11") return false;
        out->relight = true;
        out->first = atoi(first.c_str());
        out->amb = atoi(amb.c_str());
        out->emis = atoi(emis.c_str());
        out->color = color;
        out->vpos = vpos;
        out->vnormal = vnormal;
        return true;
    }
    if (last_d0 == "muloD0,r11," + color)
        out->color0 = true;
    else if (last_d0 != "madoD0,r2,c[a0.x+0],r11")
        return false;
    out->ok = true;
    out->nlights = nl;
    out->env = env_x && env_y;
    out->first = atoi(first.c_str());
    out->amb = atoi(amb.c_str());
    out->diff = atoi(diff.c_str());
    out->emis = atoi(emis.c_str());
    out->wv = atoi(wv.c_str());
    out->normal = normal;
    out->color = color;
    out->pos = pos.size() >= 2 && pos[0] == 'r' ? pos : std::string();
    return true;
}

std::string translate_vs(const std::string &src, const DWORD formats[16], std::vector<std::string> *warnings,
                         const VsLighting *lit)
{
    bool screenspace = false;
    std::vector<std::string> lines = preprocess(src, &screenspace);
    VsCtx cx;
    for (auto &l : lines)
        vs_instruction(cx, l);
    std::ostringstream decl, decode, h;
    for (int s = 0; s < 16; s++) {
        if ((formats[s] & 0xFF) != 0x02 && formats[s])
            vs_input(s, formats[s], decl, decode);
        else
            decode << "    v" << s << " = attr_const[" << s << "];\n";
    }
    h << k_vs_prelude;
    // relight: only with a streamed normal (formats[n] for vN); the draw picks the relight pixel shader the same way
    int vn = lit && lit->relight ? atoi(lit->vnormal.c_str() + 1) : -1;
    bool relight = vn >= 0 && vn < 16 && formats[vn] && (formats[vn] & 0xFF) != 0x02;
    h << "struct VOut { " << k_io_fields << (lit && lit->ok || relight ? k_io_lit : "") << " };\n";
    h << "struct VIn {\n" << decl.str() << "    uint vid : SV_VertexID;\n};\n";
    h << "VOut main(VIn i)\n{\n";
    h << "    float4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15;\n" << decode.str();
    // material sway (foliage cards, banners): vmat = (class, seconds, amplitude); position input v0, object units.
    // Phase from the position so a card / banner bends in a wave instead of sliding as a whole.
    if (!screenspace)
        h << "    if (vmat.x >= 4) { float ph = dot(v0.xyz, float3(0.37, 0.21, 0.29)); float t = vmat.y;\n"
             "        v0.xyz += float3(sin(t * 1.3 + ph), 0.3 * sin(t * 1.7 + ph * 1.3), cos(t * 1.1 + ph * 0.7)) * vmat.z; }\n";
    h << "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n";
    h << "    float4 oPos = float4(0, 0, 0, 1), oD0 = 0, oD1 = 0, oB0 = 0, oB1 = 0, oT0 = 0, oT1 = 0, oT2 = 0, oT3 = 0;\n";
    h << "    float oFog = 1, oPts = 1;\n    int a0 = 0;\n";
    h << cx.body.str();
    h << "    VOut o;\n";
    if (screenspace) {
        // oPos.xyz = render-target pixels / depth units (the shader applied its viewport, c94 scale / c95 offset,
        // which may be a sub-rectangle), oPos.w = clip w: map pixels back to NDC over the whole target
        h << "    float w = oPos.w == 0 ? 1 : oPos.w;\n";
        h << "    float zs = c[190].z == 0 ? 1 : c[190].z;\n";
        h << "    float3 ndc = float3(oPos.x / rt_size.x * 2 - 1, 1 - oPos.y / rt_size.y * 2, (oPos.z - c[191].z) / zs);\n";
        h << "    o.pos = float4(ndc * w, w);\n";
    } else {
        h << "    o.pos = oPos;\n";
    }
    h << "    o.pos.xy += rt_size.zw * o.pos.w;   // TAA sub-pixel jitter (0 when off / after the 3D resolve)\n";
    h << "    o.d0 = saturate(oD0); o.d1 = saturate(oD1); o.t0 = oT0; o.t1 = oT1; o.t2 = oT2; o.t3 = oT3; o.fog = oFog;\n";
    if (lit && lit->ok)                                // lighting normal (length kept: interpolation shortens it)
        h << "    o.ln = float4(" << lit->normal << ".xyz, length(" << lit->normal << ".xyz)); o.lc = "
          << (lit->color0 ? lit->color : std::string("float4(1, 1, 1, 1)")) << ";\n";
    if (relight)                                       // object-space normal, vertex colour, position
        h << "    o.ln = float4(" << lit->vnormal << ".xyz, 1); o.lc = "
          << (lit->color0 ? lit->color : std::string("float4(1, 1, 1, 1)")) << "; o.lp = float4(" << lit->vpos
          << ".xyz, 1);\n";
    if (lit && lit->ok)                                // surface position, same space (specular / rim view vector)
        h << "    o.lp = " << (lit->pos.empty() ? std::string("float4(0, 0, 0, 0)") : "float4(" + lit->pos + ".xyz, 1)")
          << ";\n";
    h << "    return o;\n}\n";
    if (warnings)
        *warnings = cx.warnings;
    return h.str();
}

// ------------------------------------------------------------------------------------------------ pixel shader
struct PsCtx {
    std::ostringstream body;
    std::vector<std::string> warnings;
    bool tex[4] = {};
    bool final_done = false;
};

static std::string ps_src(PsCtx &cx, const std::string &s0)
{
    std::string s = trim(s0);
    bool invert = false, neg = false;
    if (s.compare(0, 2, "1-") == 0) {
        invert = true;
        s = trim(s.substr(2));
    }
    if (!s.empty() && s[0] == '-') {
        neg = true;
        s = trim(s.substr(1));
    }
    std::string reg = s, sw, mod;
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        reg = s.substr(0, dot);
        sw = s.substr(dot + 1);
    }
    size_t us = reg.find('_');
    if (us != std::string::npos) {
        mod = reg.substr(us + 1);
        reg = reg.substr(0, us);
    }
    std::string e;
    if (reg == "zero") e = "float4(0, 0, 0, 0)";
    else if (reg == "one") e = "float4(1, 1, 1, 1)";
    else if (reg == "fog") e = "fogv";
    else if (reg == "prod") e = "prodv";
    else if (reg == "sum") e = "sumv";
    else if (reg[0] == 'c') e = "pc[" + reg.substr(1) + "]";
    else e = reg;                                      // t0-t3, v0, v1, r0, r1
    if (!sw.empty()) {
        std::string m = mask_of(sw);
        if (m == "xyz") e = "float4((" + e + ").xyz, (" + e + ").w)";   // .rgb as a source = colour part
        else e = "(" + e + ")" + swizzle4(sw);
    }
    if (mod == "bx2") e = "(2 * (" + e + ") - 1)";
    else if (mod == "bias") e = "((" + e + ") - 0.5)";
    if (neg) e = "(-" + e + ")";
    if (invert) e = "(1 - " + e + ")";
    return e;
}

static void ps_assign(PsCtx &cx, const std::string &dst, const std::string &value, bool clamp_out = true)
{
    std::string reg = dst, mask;
    size_t dot = dst.find('.');
    if (dot != std::string::npos) {
        reg = dst.substr(0, dot);
        mask = mask_of(dst.substr(dot + 1));
    }
    size_t us = reg.find('_');
    std::string scale;
    if (us != std::string::npos) {                     // _x2 _x4 _d2 _sat on destination
        std::string m = reg.substr(us + 1);
        reg = reg.substr(0, us);
        if (m == "x2") scale = " * 2";
        else if (m == "x4") scale = " * 4";
        else if (m == "d2") scale = " * 0.5";
    }
    if (reg == "discard")
        return;
    std::string v = "(" + value + ")" + scale;
    if (clamp_out)
        v = "clamp(" + v + ", -1, 1)";
    if (mask.empty() || mask == "xyzw")
        cx.body << "    " << reg << " = " << v << ";\n";
    else
        cx.body << "    " << reg << "." << mask << " = (" << v << ")." << mask << ";\n";
}

static bool ps_instruction(PsCtx &cx, std::string line)
{
    line = trim(line);
    if (line[0] == '+')
        line = trim(line.substr(1));
    size_t sp = line.find_first_of(" \t");
    std::string op = sp == std::string::npos ? line : line.substr(0, sp);
    for (auto &ch : op) ch = (char)tolower(ch);
    if (op == "xps.1.1" || op == "ps.1.1" || op == "ps.1.0" || op == "nop")
        return true;
    std::vector<std::string> a = split_operands(sp == std::string::npos ? "" : line.substr(sp + 1));
    auto S = [&](size_t i) { return i < a.size() ? ps_src(cx, a[i]) : std::string("float4(0,0,0,0)"); };
    if (op == "def") {
        if (a.size() >= 5)
            cx.body << "    pc[" << a[0].substr(1) << "] = float4(" << atof(a[1].c_str()) << ", " << atof(a[2].c_str())
                    << ", " << atof(a[3].c_str()) << ", " << atof(a[4].c_str()) << ");\n";
        return true;
    }
    if (op == "tex") {
        int n = a.empty() ? 0 : a[0][1] - '0';
        cx.tex[n] = true;
        cx.body << "    t" << n << " = sample_stage(" << n << ", i.t" << n << ");\n";
        return true;
    }
    if (op.compare(0, 3, "tex") == 0) {                // texm3x2pad / texbem / ...: approximate with a plain fetch
        int n = a.empty() ? 0 : a[0][1] - '0';
        cx.tex[n] = true;
        cx.body << "    t" << n << " = sample_stage(" << n << ", i.t" << n << ");\n";
        cx.warnings.push_back("approximated texture op '" + op + "'");
        return true;
    }
    if (op == "mov") ps_assign(cx, a[0], S(1));
    else if (op == "add") ps_assign(cx, a[0], S(1) + " + " + S(2));
    else if (op == "sub") ps_assign(cx, a[0], S(1) + " - " + S(2));
    else if (op == "mul") ps_assign(cx, a[0], S(1) + " * " + S(2));
    else if (op == "mad") ps_assign(cx, a[0], S(1) + " * " + S(2) + " + " + S(3));
    else if (op == "lrp") ps_assign(cx, a[0], "lerp(" + S(3) + ", " + S(2) + ", " + S(1) + ")");
    else if (op == "dp3") ps_assign(cx, a[0], "dot((" + S(1) + ").xyz, (" + S(2) + ").xyz).xxxx");
    else if (op == "dp4") ps_assign(cx, a[0], "dot(" + S(1) + ", " + S(2) + ").xxxx");
    else if (op == "cnd") ps_assign(cx, a[0], "((" + S(1) + ").w > 0.5 ? " + S(2) + " : " + S(3) + ")");
    else if (op == "xmma" || op == "xmmc") {           // d0 = s0*s1, d1 = s2*s3, d2 = d0+d1 (xmmc: r0.a mux)
        std::string p0 = S(3) + " * " + S(4), p1 = S(5) + " * " + S(6);
        cx.body << "    { float4 m0 = " << p0 << ", m1 = " << p1 << ";\n";
        ps_assign(cx, a[0], "m0");
        ps_assign(cx, a[1], "m1");
        ps_assign(cx, a[2], op == "xmma" ? "m0 + m1" : "(r0.w > 0.5 ? m1 : m0)");
        cx.body << "    }\n";
    } else if (op == "xdm" || op == "xdd") {          // d0 = dot(s0,s1), d1 = s2*s3 | dot(s2,s3)
        cx.body << "    { float4 m0 = dot((" << S(2) << ").xyz, (" << S(3) << ").xyz).xxxx, m1 = "
                << (op == "xdm" ? S(4) + " * " + S(5) : "dot((" + S(4) + ").xyz, (" + S(5) + ").xyz).xxxx") << ";\n";
        ps_assign(cx, a[0], "m0");
        ps_assign(cx, a[1], "m1");
        cx.body << "    }\n";
    } else if (op == "xfc") {                          // final combiner: rgb = A*B + (1-A)*C + D, alpha = G
        cx.body << "    { prodv = " << S(4) << " * " << S(5) << ";\n";
        cx.body << "      sumv = r0 + v1;\n";
        cx.body << "      float4 A = " << S(0) << ", B = " << S(1) << ", C = " << S(2) << ", D = " << S(3) << ";\n";
        cx.body << "      outc = float4(saturate(A.xyz * B.xyz + (1 - A.xyz) * C.xyz + D.xyz), saturate((" << S(6)
                << ").w)); }\n";
        cx.final_done = true;
    } else {
        cx.warnings.push_back("unknown pixel op '" + op + "'");
        return false;
    }
    return true;
}

static const char *k_ps_prelude = R"(
cbuffer PSConst : register(b1) { float4 pc_in[8]; float4 fog_color; float4 alpha_test; float4 tex_scale[4];
                                 float4 fog_params;     // fog_params: mode+1 (0 = off), start, end, density
                                 float4 soft_params;    // soft particles: on, 1/range, 1 alpha | 2 colour too
                                 float4 lit_params;     // per-pixel extras: specular, spec power, rim, on
                                 float4 shadow_rows[4];  // lit draws: surface -> last frame's shadow map
                                 float4 shadow_params;  // on, strength, texel
                                 float4 relight_sun;     // relight draws: sun travel direction (object space), strength
                                 float4 view_rows[3];   // lit / relit draws: object -> view (rotation + translation)
                                 float4 fx_params;      // bump height, effect light count, effect light intensity
                                 float4 fx_lights[8];   // effect lights, view space: (pos, radius), (colour)
                                 float4 material; };    // class (1 water 2 lava 3 glow 4 foliage 5 cloth), seconds, strength
Texture2D<float> smap_px : register(t5);
SamplerComparisonState scmp_px : register(s5);
Texture2D<float> gdepth : register(t4);                 // G-buffer view depth (soft-particle draws only)
struct POut { float4 c : SV_Target0; float4 g : SV_Target1; float4 n : SV_Target2; };
float fog_factor(float d)
{
    int m = (int)fog_params.x;
    d = abs(d);
    if (m == 4) return saturate((fog_params.z - d) / max(fog_params.z - fog_params.y, 0.0001));   // linear
    if (m == 2) return saturate(exp(-d * fog_params.w));                                          // exp
    if (m == 3) { float e = d * fog_params.w; return saturate(exp(-e * e)); }                      // exp2
    if (m == 1) return saturate(d);                                                                // vertex fog
    return 1;
}
Texture2D tx0 : register(t0); Texture2D tx1 : register(t1); Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);
SamplerState sm0 : register(s0); SamplerState sm1 : register(s1); SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);
float4 sample_stage(int n, float4 uv)
{
    float2 p = uv.xy / (uv.w == 0 ? 1 : uv.w);       // projective coordinates
    if (n == 0) return tx0.Sample(sm0, p * tex_scale[0].xy);
    if (n == 1) return tx1.Sample(sm1, p * tex_scale[1].xy);
    if (n == 2) return tx2.Sample(sm2, p * tex_scale[2].xy);
    return tx3.Sample(sm3, p * tex_scale[3].xy);
}
bool alpha_pass(float a)
{
    int f = (int)alpha_test.z;  // GL func & 7: 0 never 1 less 2 equal 3 lequal 4 greater 5 notequal 6 gequal 7 always
    float r = alpha_test.y;
    return f == 7 || (f == 1 && a < r) || (f == 2 && a == r) || (f == 3 && a <= r) || (f == 4 && a > r) ||
           (f == 5 && a != r) || (f == 6 && a >= r);
}
)";

std::string translate_ps(const std::string &src, std::vector<std::string> *warnings, const VsLighting *lit)
{
    std::vector<std::string> lines = preprocess(src, nullptr);
    PsCtx cx;
    for (auto &l : lines)
        ps_instruction(cx, l);
    std::ostringstream h;
    h << k_ps_prelude;
    bool rl = lit && lit->relight && !lit->ok;          // ambient-only shader with a sun term
    bool pp = lit && lit->ok || rl;
    h << "struct PIn { " << k_io_fields << (pp ? k_io_lit : "") << " };\n";
    if (pp) {
        h << "cbuffer VSConst : register(b0) { float4 c[192]; float4 attr_const[16]; float4 rt_size; float4 vmat; };\n";
        h << "float4 cx(int n) { return c[clamp(n + 96, 0, 191)]; }   // Xbox constant slot\n";
        // relief from the texture: luminance as height, perturbing the normal with screen-space derivatives of
        // height and position (Mikkelsen, bump mapping unparametrized surfaces) - no tangents, no extra textures.
        // k = height in surface units (fx_params.x, [video] Bump)
        h << "float3 bump_normal(float3 N, float3 P, float4 uv, float k)\n"
             "{\n"
             "    float h = dot(sample_stage(0, uv).xyz, float3(0.3, 0.59, 0.11)) * k;\n"
             "    float3 dpdx = ddx(P), dpdy = ddy(P);\n"
             "    float3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);\n"
             "    float det = dot(dpdx, r1);\n"
             "    float3 nb = abs(det) * N - sign(det) * (ddx(h) * r1 + ddy(h) * r2);\n"
             "    return k > 0 && dot(nb, nb) > 1e-20 ? normalize(nb) : N;\n"
             "}\n";
    }
    h << "POut main(PIn " << (pp ? "iin" : "i") << ")\n{\n";
    if (rl) {                                          // baked colour x a sun factor (average unchanged)
        h << "    PIn i = iin;\n";
        h << "    float3 nn = bump_normal(normalize(iin.ln.xyz + 1e-6), iin.lp.xyz, iin.t0, fx_params.x);\n";
        h << "    int a = (int)floor(cx(" << lit->first << ").w + 0.5);\n";
        h << "    float4 col = cx(" << lit->emis << ") + cx(a) * cx(" << lit->amb << ");\n";
        h << "    float sf = 1 + relight_sun.w * (saturate(dot(nn, -relight_sun.xyz)) - 0.5);\n";
        h << "    i.d0 = saturate(col * iin.lc * float4(sf, sf, sf, 1));\n";
    } else if (pp) {                                   // the vertex shader's lighting, per pixel
        h << "    PIn i = iin;\n";
        h << "    float3 n = iin.ln.xyz; float nl = length(n); if (nl > 0) n *= iin.ln.w / nl;\n";
        // characters: no relief - skin / cloth / face textures carry painted shading, not height (a fixed height on
        // a head ~0.3 units wide turned the HD faces blotchy and darkened the painted eyes - 2026-10-10)
        h << "    int a = (int)floor(cx(" << lit->first << ").w + 0.5);\n";
        h << "    float4 col = cx(" << lit->emis << ") + cx(a) * cx(" << lit->amb << ");\n";
        // view vector: the camera in this surface's own space is put in c92 per draw (d3d_program.cpp - the
        // game's VS_CAMERA_POSITIONDIRECTION_LOCAL slot, never filled by the game)
        h << "    float3 nn = normalize(n), V = " << (lit->pos.empty() ? std::string("float3(0, 0, 1)")
                                                       : std::string("normalize(cx(92).xyz - iin.lp.xyz)")) << ";\n";
        h << "    float3 lit_spec = 0, lit_key = 0; float lit_keylum = -1;\n";
        // shadow on the key (brightest) light only: ambient and the other lights stay
        h << "    float vis = 1;\n";
        h << "    if (shadow_params.x > 0 && iin.lp.w > 0) {\n";
        h << "        float4 q = float4(iin.lp.xyz, 1);\n";
        h << "        float3 sp = float3(dot(shadow_rows[0], q), dot(shadow_rows[1], q), dot(shadow_rows[2], q));\n";
        h << "        float2 suv = sp.xy * float2(0.5, -0.5) + 0.5;\n";
        h << "        if (all(suv > 0) && all(suv < 1) && sp.z < 1) {\n";
        h << "            float zr = sp.z - 0.001, t = shadow_params.z;\n";
        h << "            vis = (smap_px.SampleCmpLevelZero(scmp_px, suv + float2(-t, -t), zr) +\n";
        h << "                   smap_px.SampleCmpLevelZero(scmp_px, suv + float2(t, -t), zr) +\n";
        h << "                   smap_px.SampleCmpLevelZero(scmp_px, suv + float2(-t, t), zr) +\n";
        h << "                   smap_px.SampleCmpLevelZero(scmp_px, suv + float2(t, t), zr)) * 0.25;\n";
        h << "            vis = lerp(1, vis, shadow_params.y);\n";
        h << "        }\n";
        h << "    }\n";
        h << "    int keyA = -1; { int ak = a; float best = -1;\n";
        h << "        [unroll] for (int l = 0; l < " << lit->nlights << "; l++) { ak = (int)floor(cx(ak).w + 0.5);\n";
        h << "            float lum = dot(cx(ak).xyz, float3(0.3, 0.59, 0.11)); if (lum > best) { best = lum; keyA = ak; } } }\n";
        h << "    [unroll] for (int l = 0; l < " << lit->nlights << "; l++) {\n";
        h << "        a = (int)floor(cx(a).w + 0.5);\n";
        h << "        float sv = a == keyA ? vis : 1;\n";
        h << "        col += max(dot(n, -cx(a + 1).xyz), 0) * cx(" << lit->diff << ") * cx(a) * sv;\n";
        h << "        float3 L = -normalize(cx(a + 1).xyz + 1e-6), H = normalize(L + V);\n";
        h << "        lit_spec += pow(saturate(dot(nn, H)), lit_params.y) * cx(a).xyz * saturate(dot(nn, L) * 4) * sv;\n";
        h << "        float lum = dot(cx(a).xyz, float3(0.3, 0.59, 0.11));\n";
        h << "        if (lum > lit_keylum) { lit_keylum = lum; lit_key = cx(a).xyz; }\n";
        h << "    }\n";
        h << "    i.d0 = saturate(col * iin.lc);\n";
        if (lit->env)
            h << "    i.t1.x = dot(float4(n, 1), cx(" << lit->wv << ")); i.t1.y = dot(float4(n, 1), cx(" << lit->wv + 1
              << "));\n";
    }
    h << "    float4 pc[8]; for (int k = 0; k < 8; k++) pc[k] = pc_in[k];\n";
    h << "    float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0, r0 = 0, r1 = 0, v0 = i.d0, v1 = i.d1, prodv = 0, sumv = 0;\n";
    h << "    float4 fogv = float4(fog_color.xyz, fog_factor(i.fog)), outc;   // Xbox fog.a = factor (1 = no fog)\n";
    std::string body = cx.body.str();
    if (pp && !rl && lit->env) {
        // armour reflections (the game's sphere-map texture at stage 1, masked by 1 - t0.a): Fresnel - subtle where
        // the surface faces the camera, stronger at grazing angles, like metal. The authored lookup stays.
        h << "    float env_f = lit_params.w > 0 ? lerp(0.7, 1.6, pow(1 - saturate(dot(nn, V)), 2)) : 1;\n";
        const std::string from = "    t1 = sample_stage(1, i.t1);\n", to = "    t1 = sample_stage(1, i.t1) * env_f;\n";
        size_t at = body.find(from);
        if (at != std::string::npos)
            body.replace(at, from.size(), to);
    }
    // materials by texture name (d3d_program.cpp material_of): flowing / rippling UVs before the combiners
    h << "    float mt = material.y, ms = material.z;\n"
         "    if (material.x == 1) { float wq = i.t0.w == 0 ? 1 : i.t0.w;   // water: ripples\n"
         "        i.t0.xy += float2(sin(i.t0.y / wq * 14 + mt * 1.3), cos(i.t0.x / wq * 11 + mt * 1.1)) * 0.005 * ms * wq; }\n"
         "    if (material.x == 2) i.t0.xy += float2(mt * 0.011, mt * 0.007) * ms * (i.t0.w == 0 ? 1 : i.t0.w);   // lava flow\n";
    h << body;
    if (!cx.final_done)
        h << "    outc = saturate(r0);\n";
    h << "    if (alpha_test.x > 0 && !alpha_pass(outc.w)) discard;\n";
    h << "    if (material.x == 1) {                       // water: moving glints (the texture's own bright bits)\n"
         "        float4 uq = i.t0; uq.xy = uq.xy * 1.7 + float2(mt * 0.031, -mt * 0.023) * (uq.w == 0 ? 1 : uq.w);\n"
         "        float g = dot(sample_stage(0, uq).xyz, float3(0.3, 0.59, 0.11));\n"
         "        outc.xyz += pow(saturate(g * 1.15), 6) * 0.45 * ms * fogv.w;\n"
         "    }\n"
         "    if (material.x == 2) outc.xyz *= 1 + ms * (0.25 + 0.15 * sin(mt * 1.4 + i.t0.x * 5));   // lava glows\n"
         "    if (material.x == 3) outc.xyz *= 1 + ms * (0.15 + 0.1 * sin(mt * 9.1) * sin(mt * 5.3 + 1));   // flicker\n";
    if (pp)                                            // effect lights (fire, sparks, magic): last frame's strongest four
        h << "    if (fx_params.y > 0 && iin.lp.w > 0) {\n"
             "        float4 q = float4(iin.lp.xyz, 1);\n"
             "        float3 vp = float3(dot(view_rows[0], q), dot(view_rows[1], q), dot(view_rows[2], q));\n"
             "        float3 vn = float3(dot(view_rows[0].xyz, nn), dot(view_rows[1].xyz, nn), dot(view_rows[2].xyz, nn));\n"
             "        vn = normalize(vn + 1e-6);\n"
             "        float3 dyn = 0;\n"
             "        [unroll] for (int k = 0; k < 4; k++) {\n"
             "            if (k >= fx_params.y) break;\n"
             "            float3 Lv = fx_lights[k * 2].xyz - vp; float d = length(Lv);\n"
             "            float att = saturate(1 - d / fx_lights[k * 2].w); att *= att;\n"
             "            dyn += fx_lights[k * 2 + 1].xyz * att * saturate(dot(vn, Lv / max(d, 1e-3)) * 0.75 + 0.25);\n"
             "        }\n"
             "        outc.xyz = max(outc.xyz + t0.xyz * dyn * fx_params.z * fogv.w, 0);   // HDR: may exceed 1\n"
             "    }\n";
    if (pp && !rl)                                     // specular (shiny where the game's env-map mask says metal) + rim
        h << "    if (lit_params.w > 0) {\n"
             "        float mask = " << (lit->env ? "saturate(1 - t0.w) + 0.1" : "0.15") << ";\n"
             "        float rim = pow(1 - saturate(dot(nn, V)), 3) * lit_params.z;\n"
             "        outc.xyz = max(outc.xyz + (lit_spec * lit_params.x * mask + rim * lit_key) * fogv.w, 0);\n"
             "    }\n";
    h << "    if (soft_params.x > 0) {\n";
    h << "        float f = saturate((abs(gdepth.Load(int3(i.pos.xy, 0))) - i.pos.w) * soft_params.y);\n";
    h << "        outc.w *= f; if (soft_params.z > 1) outc.xyz *= f;\n";
    h << "    }\n";
    // negative depth = no sun shadow over it: lit draws (shade themselves) and glowing materials (self-lit)
    h << "    POut po; po.c = outc; po.g = float4(" << (pp && !rl ? "-" : "(material.x == 2 || material.x == 3 ? -1 : 1) * ")
      << "i.pos.w, 0, 0, 0);   // G-buffer: view depth (negative: lit draw, shadows itself)\n";
    if (pp)                                            // G-buffer normal: view space, alpha 1 = present
        h << "    { float3 vn = float3(dot(view_rows[0].xyz, nn), dot(view_rows[1].xyz, nn), dot(view_rows[2].xyz, nn));\n"
             "      po.n = dot(vn, vn) > 1e-8 ? float4(normalize(vn) * 0.5 + 0.5, 1) : float4(0, 0, 0, 0); }\n";
    else
        h << "    po.n = float4(0, 0, 0, 0);\n";
    h << "    return po;\n}\n";
    if (warnings)
        *warnings = cx.warnings;
    return h.str();
}
