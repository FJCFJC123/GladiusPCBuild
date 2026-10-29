// Quest flow (the "Quests" tab): the game variables scripts keep their progress in (CreateIntVar / SetIntVar /
// GetIntVar; saved with the game), live and editable, a change history, snapshots to go back to, and a map of where
// every script sets, counts and checks each one - the flow points to jump between when retesting a quest.
//   variable table 0x38EA88: 64 buckets {head, tail}; hash 0x1878D0 (case-insensitive), a lookup compares the hash
//   only. Node +0 next, +4 prev, +8 char* name, +0xC hash, +0x10 value {+8 type 1 int / 2 float / 3 string,
//   +0xC int / float / interned char*}. CreateIntVar (0x263A0, thiscall table, name, value) never overwrites: it
//   returns false when the name exists - a reset script's OnInit leaves progress alone. String values are set with
//   0x23D20 (thiscall value, const char*: releases the old interned string).
// The map comes from the bytecode of every script (data/script/bin, script_names.inc + loose files; the loaded copy
// when one is in memory): calls are found by walking back over their arguments' stack effects.
#include "devtools.h"
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "imgui.h"

bool dogdbg_show_code(const std::string &file, uint32_t ip);    // dogdbg.cpp: Scripts tab at file / code offset

enum : uint32_t { VAR_TABLE = 0x38EA88, FILE_HASH = 0x483250, SHARED_BIT = 0x20000000, GLOBAL_OBJ = 0x40000000 };
enum { T_INT = 1, T_FLOAT = 2, T_STRING = 3 };

// ------------------------------------------------------------------------------------------------ guarded access
static bool rd(uint32_t addr, void *out, size_t n)
{
    if (!addr) return false;
    __try {
        memcpy(out, (const void *)(uintptr_t)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static uint32_t rd32(uint32_t a) { uint32_t v = 0; return rd(a, &v, 4) ? v : 0; }
static bool wr(uint32_t addr, const void *in, size_t n)
{
    __try {
        memcpy((void *)(uintptr_t)addr, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static std::string rd_str(uint32_t a, size_t cap = 200)
{
    std::string s;
    for (char c; s.size() < cap && rd(a + (uint32_t)s.size(), &c, 1) && c;) s += c >= 32 && c < 127 ? c : '?';
    return s;
}
static std::string lower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string stem(const std::string &p)            // "d:\data\script\Bin\X.scb" -> "X"
{
    size_t k = p.find_last_of("\\/");
    std::string s = k == std::string::npos ? p : p.substr(k + 1);
    size_t d = s.rfind('.');
    return d == std::string::npos ? s : s.substr(0, d);
}

// ------------------------------------------------------------------------------------------------ the variables
struct GVar {
    std::string name;
    int type;                                           // T_INT / T_FLOAT / T_STRING (0 = other)
    int32_t i;
    float f;
    std::string s;
    uint32_t val;                                       // the value object
};
static std::string fmt(const GVar &v)
{
    char b[64];
    if (v.type == T_INT) sprintf(b, "%d", v.i);
    else if (v.type == T_FLOAT) sprintf(b, "%g", v.f);
    else if (v.type == T_STRING) return "\"" + v.s + "\"";
    else return "?";
    return b;
}
static bool read_var(uint32_t node, GVar &v)
{
    uint32_t name = rd32(node + 8), val = rd32(node + 0x10);
    if (!name || !val) return false;
    v.name = rd_str(name, 120);
    v.val = val;
    v.type = (int)rd32(val + 8);
    uint32_t raw = rd32(val + 0xC);
    v.i = (int32_t)raw;
    memcpy(&v.f, &raw, 4);
    v.s = v.type == T_STRING && raw ? rd_str(raw, 200) : "";
    if (v.type < T_INT || v.type > T_STRING) v.type = 0;
    return true;
}
static std::vector<GVar> read_vars()
{
    std::vector<GVar> out;
    for (int b = 0; b < 64; b++) {
        int n = 0;
        for (uint32_t node = rd32(VAR_TABLE + b * 8); node && n < 5000; node = rd32(node), n++) {
            GVar v;
            if (read_var(node, v)) out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end(), [](const GVar &a, const GVar &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

// game thread (or parked): write a value; a variable that does not exist yet is created (int / float)
typedef bool(__fastcall *CreateIntFn)(uint32_t table, uint32_t edx, const char *name, int32_t value);
typedef bool(__fastcall *CreateFloatFn)(uint32_t table, uint32_t edx, const char *name, float value);
typedef bool(__fastcall *SetStrFn)(uint32_t val, uint32_t edx, const char *s);
static uint32_t find_val(const std::string &name)
{
    for (int b = 0; b < 64; b++) {
        int n = 0;
        for (uint32_t node = rd32(VAR_TABLE + b * 8); node && n < 5000; node = rd32(node), n++)
            if (!_stricmp(rd_str(rd32(node + 8), 120).c_str(), name.c_str())) return rd32(node + 0x10);
    }
    return 0;
}
static bool write_var(const std::string &name, int type, int32_t i, float f, const std::string &s)
{
    uint32_t val = find_val(name);
    __try {
        if (!val) {
            if (type == T_INT) return ((CreateIntFn)0x263A0)(VAR_TABLE, 0, name.c_str(), i);
            if (type == T_FLOAT) return ((CreateFloatFn)0x26410)(VAR_TABLE, 0, name.c_str(), f);
            return false;                               // strings: only existing ones
        }
        uint32_t t = rd32(val + 8);
        if (t != (uint32_t)type) return false;
        if (type == T_INT) return wr(val + 0xC, &i, 4);
        if (type == T_FLOAT) return wr(val + 0xC, &f, 4);
        return ((SetStrFn)0x23D20)(val, 0, s.c_str());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// writes go to the game thread between frames (or straight in while the debugger has it parked)
static std::mutex g_job_lock;
static std::vector<std::function<void()>> g_jobs;
static void game_job(std::function<void()> fn)
{
    if (dogdbg_stopped()) { fn(); return; }
    std::lock_guard<std::mutex> l(g_job_lock);
    g_jobs.push_back(std::move(fn));
}
static char g_msg[400];                                 // last write / restore result
static void set_var(const std::string &name, int type, int32_t i, float f, const std::string &s, const char *why)
{
    if (con_online()) { strcpy(g_msg, "online: game variables are read-only"); return; }
    game_job([=] {
        bool ok = write_var(name, type, i, f, s);
        GVar v{name, type, i, f, s, 0};
        _snprintf(g_msg, sizeof g_msg - 1, ok ? "%s = %s%s%s" : "could not set %s to %s%s%s", name.c_str(), fmt(v).c_str(),
                  why ? "  (" : "", why ? why : "");
        if (why && ok) strcat(g_msg, ")");
    });
}

// ------------------------------------------------------------------------------------------------ change history
struct Change {
    char when[12];
    std::string name, from, to;                         // from "" = created
    int type;
    int32_t old_i;
    float old_f;
    std::string old_s;
    int bulk;                                           // > 0: this many changed at once (a save loaded) - no details
};
static std::mutex g_hist_lock;
static std::deque<Change> g_hist;
static volatile LONG g_hist_gen;
static std::unordered_map<uint32_t, GVar> g_last;        // node -> value at the last poll (game thread)
static bool g_have_last;

static void poll_changes()                               // game thread
{
    std::unordered_map<uint32_t, GVar> now;
    for (int b = 0; b < 64; b++) {
        int n = 0;
        for (uint32_t node = rd32(VAR_TABLE + b * 8); node && n < 5000; node = rd32(node), n++) {
            GVar v;
            if (read_var(node, v)) now.emplace(node, std::move(v));
        }
    }
    if (g_have_last) {
        std::vector<Change> ch;
        for (auto &kv : now) {
            auto it = g_last.find(kv.first);
            const GVar &v = kv.second;
            if (it != g_last.end() && it->second.name == v.name && it->second.type == v.type && it->second.i == v.i &&
                it->second.s == v.s)
                continue;
            Change c{};
            c.name = v.name, c.to = fmt(v), c.type = v.type;
            if (it != g_last.end() && it->second.name == v.name) {
                c.from = fmt(it->second);
                c.old_i = it->second.i, c.old_f = it->second.f, c.old_s = it->second.s;
            }
            ch.push_back(c);
        }
        if (!ch.empty()) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char when[12];
            sprintf(when, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
            std::lock_guard<std::mutex> l(g_hist_lock);
            if (ch.size() > 40) {                       // a save loaded / new game: one line
                Change c{};
                c.bulk = (int)ch.size();
                strcpy(c.when, when);
                g_hist.push_back(c);
            } else
                for (auto &c : ch) {
                    strcpy(c.when, when);
                    g_hist.push_back(c);
                }
            while (g_hist.size() > 500) g_hist.pop_front();
            InterlockedIncrement(&g_hist_gen);
        }
    }
    g_last.swap(now);
    g_have_last = true;
}

void flow_frame()                                       // game thread, per frame
{
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> l(g_job_lock);
        jobs.swap(g_jobs);
    }
    for (auto &j : jobs) j();
    static int n;
    if (++n % 6 == 0 || !jobs.empty()) poll_changes();
}

// ------------------------------------------------------------------------------------------------ the map (bytecode)
enum {
    OP_ARG = 2, OP_STATEMENT = 3, OP_CALL = 9, OP_PUSHINT = 42, OP_PUSHFLOAT = 43, OP_PUSHSTRING = 44, OP_PUSHVECTOR = 45,
    OP_PUSHSYMBOL = 46, OP_ARRAYREF = 52, OP_CALLFUNC = 53, OP_UNEG = 54, OP_LNEG = 55, OP_MULTIPLY = 56, OP_ADD = 59,
    OP_SUB = 60, OP_LT = 61, OP_NE = 66, OP_LOR = 71, OP_ASSIGN = 72, OP_FUNCLABEL = 74, OP_LINE = 77, OP_COUNT = 84
};
static const uint8_t k_nops[OP_COUNT] = {           // operands (u32) of each op: scb.py NOPERANDS
    0, 0, 0, 0, 0, 0, 1, 1, 1, 3, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 3, 1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 0, 0, 0, 0, 1, 1};
static const char *const k_binop[] = {"*", "/", "%", "+", "-", "<", ">", "<=", ">=", "==", "!=", "&", "^", "|", "&&", "||"};

enum UseKind { U_CREATE, U_SET, U_COUNT, U_EXPR, U_CHECK, U_READ };
struct Use {
    uint8_t kind, vtype;                                // vtype T_INT / T_FLOAT / T_STRING
    std::string value;                                  // U_SET / U_CREATE: the constant; U_EXPR: the expression; U_CHECK: "== 2"
    int32_t num;                                        // the constant as a number (U_SET / U_CREATE / U_CHECK), delta (U_COUNT)
    bool has_num;
    std::string file, func, event;                      // event: func is this file's handler for it
    uint32_t ip;
};
struct VarInfo {
    std::string name;
    std::vector<Use> uses;
};
struct HandlerReg {
    std::string file, event, func;
};
struct FlowDb {
    std::map<std::string, VarInfo> vars;                // lower-case name
    std::vector<HandlerReg> regs;
    int files = 0, bad = 0;
};

struct Img {                                            // an SGOD image (see dog/scb.py)
    const uint8_t *d;
    uint32_t n;
    uint32_t nstr = 0, nstr_size = 0, sstr = 0, sstr_size = 0, symb = 0, nsymb = 0, sasy = 0, nsasy = 0, code = 0,
             code_size = 0;
    uint32_t u32(uint32_t o) const { uint32_t v = 0; if (o + 4 <= n) memcpy(&v, d + o, 4); return v; }
    std::string cstr(uint32_t base, uint32_t size, uint32_t off) const
    {
        std::string s;
        for (uint32_t p = base + off; off < size && p < n && d[p] && s.size() < 200; p++, off++) s += (char)d[p];
        return s;
    }
    std::string str(uint32_t off) const { return cstr(nstr, nstr_size, off); }
    std::string sym(uint32_t idx) const
    {
        bool sh = (idx & SHARED_BIT) != 0;
        uint32_t i = idx & ~SHARED_BIT, tab = sh ? sasy : symb, cnt = sh ? nsasy : nsymb;
        if (!tab || i >= cnt) return "?";
        std::string s = cstr(sstr, sstr_size, u32(tab + i * 12));
        size_t h = s.find('#');                         // locals: "x#N"
        return h == std::string::npos ? s : s.substr(0, h);
    }
    bool parse()
    {
        if (n < 16 || memcmp(d, "SGOD", 4)) return false;
        for (uint32_t p = 8; p + 8 <= n; p += 8) {
            char t[5] = {(char)d[p + 3], (char)d[p + 2], (char)d[p + 1], (char)d[p], 0};
            if (!isalpha((unsigned char)t[0])) break;
            uint32_t o = u32(p + 4);
            if (o == 0xFFFFFFFF || o + 12 > n) continue;
            if (!strcmp(t, "NSTR")) nstr = o + 12, nstr_size = u32(o + 4);
            else if (!strcmp(t, "SSTR")) sstr = o + 12, sstr_size = u32(o + 4);
            else if (!strcmp(t, "SYMB")) symb = o + 8, nsymb = u32(o + 4) / 12;
            else if (!strcmp(t, "SASY")) sasy = o + 8, nsasy = u32(o + 4) / 12;
            else if (!strcmp(t, "CODE")) code = o + 8, code_size = u32(o + 4);
        }
        if (code + code_size > n) code_size = code < n ? n - code : 0;
        return code && code_size;
    }
};

// net stack effect of one instruction inside an expression (false: not an expression instruction)
static bool stack_effect(const Img &m, uint32_t p, int &e)
{
    uint8_t op = m.d[m.code + p];
    if (op >= OP_PUSHINT && op <= OP_PUSHSYMBOL) e = 1;
    else if (op == OP_CALLFUNC) e = 1 - (int)m.u32(m.code + p + 5);
    else if ((op >= OP_MULTIPLY && op <= OP_LOR) || op == OP_ARRAYREF || op == OP_ASSIGN) e = -1;
    else if (op == OP_UNEG || op == OP_LNEG || op == OP_LINE || op == OP_ARG || op == OP_STATEMENT) e = 0;
    else return false;
    return true;
}

// the instruction ranges [first, end) of the argc arguments of the call at index c
static bool call_args(const Img &m, const std::vector<uint32_t> &at, int c, int argc, std::vector<std::pair<int, int>> &out)
{
    out.assign(argc, {0, 0});
    int e = c;
    for (int a = argc - 1; a >= 0; a--) {
        int sum = 0, i = e;
        while (sum < 1) {
            if (--i < 0 || c - i > 64) return false;
            int d;
            if (!stack_effect(m, at[i], d)) return false;
            sum += d;
        }
        out[a] = {i, e};
        e = i;
    }
    return true;
}

// an expression range as text ("day", "GetIntVar(\"x\") - 1")
static std::string expr_text(const Img &m, const std::vector<uint32_t> &at, int s, int e)
{
    std::vector<std::string> st;
    for (int i = s; i < e; i++) {
        uint32_t p = m.code + at[i];
        uint8_t op = m.d[p];
        char b[64];
        if (op == OP_PUSHINT) sprintf(b, "%d", (int32_t)m.u32(p + 1)), st.push_back(b);
        else if (op == OP_PUSHFLOAT) { float f; uint32_t r = m.u32(p + 1); memcpy(&f, &r, 4); sprintf(b, "%g", f); st.push_back(b); }
        else if (op == OP_PUSHSTRING) st.push_back("\"" + m.str(m.u32(p + 1)) + "\"");
        else if (op == OP_PUSHSYMBOL) st.push_back(m.sym(m.u32(p + 1)));
        else if (op == OP_PUSHVECTOR) st.push_back("<vector>");
        else if (op == OP_CALLFUNC) {
            int n = (int)m.u32(p + 5);
            uint32_t obj = m.u32(p + 9);
            std::string a;
            for (int k = 0; k < n && !st.empty(); k++) a = st.back() + (k ? ", " + a : ""), st.pop_back();
            std::string o = obj == GLOBAL_OBJ ? "" : m.sym(obj) + ".";
            st.push_back(o + m.sym(m.u32(p + 1)) + "(" + a + ")");
        } else if (op >= OP_MULTIPLY && op <= OP_LOR && st.size() >= 2) {
            std::string r = st.back(); st.pop_back();
            std::string l = st.back(); st.pop_back();
            st.push_back(l + " " + k_binop[op - OP_MULTIPLY] + " " + r);
        } else if (op == OP_UNEG && !st.empty()) st.back() = "-" + st.back();
        else if (op == OP_LNEG && !st.empty()) st.back() = "!" + st.back();
    }
    std::string r;
    for (auto &x : st) r += (r.empty() ? "" : " ") + x;
    return r.size() > 90 ? r.substr(0, 87) + "..." : r;
}

static int var_native(const std::string &n, int *argc)   // 0 none, else kind | type << 4
{
    static const struct { const char *name; int kind, type, argc; } k[] = {
        {"SetIntVar", U_SET, T_INT, 2}, {"SetFloatVar", U_SET, T_FLOAT, 2}, {"SetStringVar", U_SET, T_STRING, 2},
        {"GetIntVar", U_READ, T_INT, 1}, {"GetFloatVar", U_READ, T_FLOAT, 1}, {"GetStringVar", U_READ, T_STRING, 1},
        {"CreateIntVar", U_CREATE, T_INT, 2}, {"CreateFloatVar", U_CREATE, T_FLOAT, 2},
        {"CreateStringVar", U_CREATE, T_STRING, 2}};
    for (auto &x : k)
        if (!_stricmp(n.c_str(), x.name)) { *argc = x.argc; return 0x100 | x.kind | x.type << 4; }
    return 0;
}

static void scan_image(const Img &m, const std::string &file, FlowDb &db)
{
    std::vector<uint32_t> at;
    for (uint32_t p = 0; p < m.code_size;) {
        at.push_back(p);
        uint8_t op = m.d[m.code + p];
        p += 1 + 4 * (op < OP_COUNT ? k_nops[op] : 0);
    }
    struct Pending { std::string var; Use u; };
    std::vector<Pending> uses;
    std::vector<HandlerReg> regs;
    std::string func;
    std::unordered_map<uint32_t, std::string> alias;    // local symbol -> the variable read into it (this function)
    auto single_str = [&](std::pair<int, int> r, std::string &s) {
        if (r.second - r.first != 1 || m.d[m.code + at[r.first]] != OP_PUSHSTRING) return false;
        s = m.str(m.u32(m.code + at[r.first] + 1));
        return true;
    };
    auto next = [&](int i) { do i++; while (i < (int)at.size() && m.d[m.code + at[i]] == OP_LINE); return i; };
    auto check_after = [&](int i, const std::string &var, int type) {  // value on the stack at i: "<value> op k"?
        int k = next(i - 1), c = next(k);
        if (c >= (int)at.size()) return false;
        uint8_t kop = m.d[m.code + at[k]], cop = m.d[m.code + at[c]];
        if ((kop != OP_PUSHINT && kop != OP_PUSHFLOAT && kop != OP_PUSHSTRING) || cop < OP_LT || cop > OP_NE) return false;
        Use u{};
        u.kind = U_CHECK, u.vtype = (uint8_t)type, u.file = file, u.func = func, u.ip = at[i - 1];
        u.value = std::string(k_binop[cop - OP_MULTIPLY]) + " " + expr_text(m, at, k, k + 1);
        u.num = (int32_t)m.u32(m.code + at[k] + 1);
        u.has_num = kop == OP_PUSHINT;
        uses.push_back({var, u});
        return true;
    };
    for (int i = 0; i < (int)at.size(); i++) {
        uint32_t p = m.code + at[i];
        uint8_t op = m.d[p];
        if (op == OP_FUNCLABEL) { func = m.sym(m.u32(p + 1)); alias.clear(); continue; }
        if (op == OP_PUSHSYMBOL && !alias.empty()) {
            auto it = alias.find(m.u32(p + 1));
            if (it != alias.end()) check_after(i + 1, it->second, T_INT);
            continue;
        }
        if (op != OP_CALLFUNC) continue;
        std::string name = m.sym(m.u32(p + 1));
        int argc = (int)m.u32(p + 5), want = 0;
        std::vector<std::pair<int, int>> args;
        if (!_stricmp(name.c_str(), "RegisterEventHandler") && argc == 3) {
            HandlerReg r{file, "", ""};
            if (call_args(m, at, i, 3, args) && single_str(args[0], r.event) && single_str(args[2], r.func)) regs.push_back(r);
            continue;
        }
        int nat = var_native(name, &want);
        if (!nat || argc != want || !call_args(m, at, i, argc, args)) continue;
        std::string var;
        if (!single_str(args[0], var)) continue;        // a computed name
        int kind = nat & 15, type = nat >> 4 & 15;
        Use u{};
        u.kind = (uint8_t)kind, u.vtype = (uint8_t)type, u.file = file, u.func = func, u.ip = at[i];
        if (kind == U_READ) {
            if (check_after(i + 1, var, type)) continue;
            int a = next(i), b = next(a);
            if (b < (int)at.size() && m.d[m.code + at[a]] == OP_PUSHINT &&
                (m.d[m.code + at[b]] == OP_ADD || m.d[m.code + at[b]] == OP_SUB))
                continue;                               // GetIntVar("x") +/- n: part of a counter
            if (a < (int)at.size() && m.d[m.code + at[a]] == OP_ASSIGN && args[0].first > 0 &&
                m.d[m.code + at[args[0].first - 1]] == OP_PUSHSYMBOL)
                alias[m.u32(m.code + at[args[0].first - 1] + 1)] = var;
            uses.push_back({var, u});
            continue;
        }
        int s = args[1].first, e = args[1].second;
        uint8_t v0 = m.d[m.code + at[s]];
        if (e - s == 1 && (v0 == OP_PUSHINT || v0 == OP_PUSHFLOAT || v0 == OP_PUSHSTRING)) {
            u.value = expr_text(m, at, s, e);
            u.num = (int32_t)m.u32(m.code + at[s] + 1);
            u.has_num = v0 == OP_PUSHINT;
        } else if (e - s == 4 && v0 == OP_PUSHSTRING && !_stricmp(m.str(m.u32(m.code + at[s] + 1)).c_str(), var.c_str()) &&
                   m.d[m.code + at[s + 1]] == OP_CALLFUNC && m.d[m.code + at[s + 2]] == OP_PUSHINT &&
                   (m.d[m.code + at[s + 3]] == OP_ADD || m.d[m.code + at[s + 3]] == OP_SUB)) {
            u.kind = U_COUNT;
            int32_t d = (int32_t)m.u32(m.code + at[s + 2] + 1);
            u.num = m.d[m.code + at[s + 3]] == OP_SUB ? -d : d;
            char b[24];
            sprintf(b, "%+d", u.num);
            u.value = b;
        } else {
            if (kind == U_SET) u.kind = U_EXPR;
            u.value = expr_text(m, at, s, e);
        }
        uses.push_back({var, u});
    }
    for (auto &r : regs)                                // mark the uses in this file's handler functions
        for (auto &x : uses)
            if (!_stricmp(x.u.func.c_str(), r.func.c_str())) x.u.event = r.event;
    for (auto &x : uses) {
        VarInfo &vi = db.vars[lower(x.var)];
        if (vi.name.empty()) vi.name = x.var;
        vi.uses.push_back(x.u);
    }
    db.regs.insert(db.regs.end(), regs.begin(), regs.end());
}

static const char *const k_script_names[] = {
#include "script_names.inc"
};

struct ScanJob {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> loaded;   // stem -> image of the files in memory
};
static std::mutex g_db_lock;
static std::shared_ptr<const FlowDb> g_db;
static volatile LONG g_scanning, g_db_gen;

static DWORD WINAPI scan_thread(void *p)
{
    std::unique_ptr<ScanJob> job((ScanJob *)p);
    auto db = std::make_shared<FlowDb>();
    std::map<std::string, bool> done;                   // lower-case stems
    auto scan = [&](const std::string &st, const uint8_t *d, uint32_t n) {
        Img m{d, n};
        if (m.parse()) scan_image(m, st, *db), db->files++;
        else db->bad++;
        done[lower(st)] = true;
    };
    for (auto &l : job->loaded) scan(l.first, l.second.data(), (uint32_t)l.second.size());
    std::vector<std::string> names(std::begin(k_script_names), std::end(k_script_names));
    if (g_host->game_dir) {                             // loose / modded scripts
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((std::string(g_host->game_dir) + "\\data\\script\\bin\\*.scb").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do names.push_back(lower(fd.cFileName));
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    for (auto &n : names) {
        std::string st = stem(n);
        if (done.count(lower(st))) continue;
        uint32_t size = 0;
        void *buf = g_host->read_game_file(("data/script/bin/" + n).c_str(), &size);
        if (!buf) { done[lower(st)] = true; continue; }
        // proper case for display: the name the script itself is known by is unknown here; keep the file name
        scan(st, (const uint8_t *)buf, size);
        g_host->free_game_file(buf);
    }
    {
        std::lock_guard<std::mutex> l(g_db_lock);
        g_db = db;
    }
    InterlockedIncrement(&g_db_gen);
    InterlockedExchange(&g_scanning, 0);
    return 0;
}

static void start_scan()                                // UI thread
{
    if (InterlockedCompareExchange(&g_scanning, 1, 0)) return;
    auto *job = new ScanJob;
    for (int b = 0; b < 64; b++) {                      // files in memory: what the game runs now (sandbox edits too)
        int n = 0;
        for (uint32_t node = rd32(FILE_HASH + b * 8); node && n < 1000; node = rd32(node), n++) {
            uint32_t f = rd32(node + 0x10), img = rd32(f + 0x84), size = rd32(f + 0x8C);
            uint32_t nb = rd32(f + 0x10);
            if (!img || !size || size > 0x400000 || !nb) continue;
            std::vector<uint8_t> v(size);
            if (rd(img, v.data(), size)) job->loaded.push_back({stem(rd_str(nb + 0xC)), std::move(v)});
        }
    }
    HANDLE h = CreateThread(nullptr, 0, scan_thread, job, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete job; InterlockedExchange(&g_scanning, 0); }
}

static std::shared_ptr<const FlowDb> flow_db(bool start)
{
    std::shared_ptr<const FlowDb> d;
    {
        std::lock_guard<std::mutex> l(g_db_lock);
        d = g_db;
    }
    if (!d && start) start_scan();
    return d;
}

// what handler function `func` of script file `file` does to the game variables, e.g. "ImperiaTownsToBeat -1" (the
// counters first; "" = nothing / map not built yet). Used by the Scripts tab's event handler panel and reset warnings.
std::string flow_effects(const std::string &file, const std::string &func, bool counters_only)
{
    auto db = flow_db(true);
    if (!db) return "";
    std::string cnt, set, st = stem(file);
    for (auto &kv : db->vars)
        for (auto &u : kv.second.uses) {
            if (_stricmp(u.file.c_str(), st.c_str()) || _stricmp(u.func.c_str(), func.c_str())) continue;
            if (u.kind == U_COUNT) cnt += (cnt.empty() ? "" : ", ") + kv.second.name + " " + u.value;
            else if (!counters_only && (u.kind == U_SET || u.kind == U_EXPR) && set.find(kv.second.name) == std::string::npos)
                set += (set.empty() ? "" : ", ") + kv.second.name;
        }
    if (set.size() > 80) set = set.substr(0, 77) + "...";
    return cnt + (cnt.empty() || set.empty() ? "" : "; ") + (set.empty() ? "" : "sets " + set);
}

// ------------------------------------------------------------------------------------------------ snapshots
struct Snapshot {
    std::string name, when;
    std::vector<GVar> vars;
};
static std::vector<Snapshot> g_snaps;
static bool g_snaps_loaded;

static std::string snap_dir()                           // <exe dir>\devtools\snapshots\ (created)
{
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    char *s = strrchr(p, '\\');
    if (s) s[1] = 0;
    std::string d = std::string(p) + "devtools";
    CreateDirectoryA(d.c_str(), nullptr);
    d += "\\snapshots";
    CreateDirectoryA(d.c_str(), nullptr);
    return d + "\\";
}
static std::string safe_name(const std::string &n)
{
    std::string r;
    for (char c : n) r += isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' ? c : '_';
    return r.empty() ? "snapshot" : r;
}
// file: "# when", then "i name value" / "f name value" / "s name value" (rest of the line)
static void save_snap(const Snapshot &s)
{
    FILE *f = fopen((snap_dir() + safe_name(s.name) + ".txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "# %s\n", s.when.c_str());
    for (auto &v : s.vars) {
        if (v.type == T_INT) fprintf(f, "i %s %d\n", v.name.c_str(), v.i);
        else if (v.type == T_FLOAT) fprintf(f, "f %s %.9g\n", v.name.c_str(), v.f);
        else if (v.type == T_STRING) fprintf(f, "s %s %s\n", v.name.c_str(), v.s.c_str());
    }
    fclose(f);
}
static void load_snaps()
{
    g_snaps_loaded = true;
    WIN32_FIND_DATAA fd;
    std::string dir = snap_dir();
    HANDLE h = FindFirstFileA((dir + "*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        FILE *f = fopen((dir + fd.cFileName).c_str(), "r");
        if (!f) continue;
        Snapshot s;
        s.name = stem(fd.cFileName);
        char line[600];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (line[0] == '#') { s.when = line + (line[1] == ' ' ? 2 : 1); continue; }
            char *sp = strchr(line, ' ');
            if (!sp || sp != line + 1) continue;
            char *sp2 = strchr(sp + 1, ' ');
            if (!sp2) continue;
            GVar v{};
            v.name.assign(sp + 1, sp2);
            const char *val = sp2 + 1;
            if (line[0] == 'i') v.type = T_INT, v.i = atoi(val);
            else if (line[0] == 'f') v.type = T_FLOAT, v.f = (float)atof(val);
            else if (line[0] == 's') v.type = T_STRING, v.s = val;
            else continue;
            s.vars.push_back(v);
        }
        fclose(f);
        g_snaps.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// game thread: put every variable back to the snapshot; ones created since are zeroed when zero_new
static void restore(const Snapshot &s, bool zero_new)
{
    std::unordered_map<std::string, const GVar *> want;
    for (auto &v : s.vars) want[lower(v.name)] = &v;
    int set = 0, created = 0, zeroed = 0, failed = 0;
    std::vector<GVar> now = read_vars();
    std::unordered_map<std::string, const GVar *> have;
    for (auto &v : now) have[lower(v.name)] = &v;
    for (auto &kv : want) {
        const GVar &w = *kv.second;
        auto it = have.find(kv.first);
        if (it != have.end() && it->second->type == w.type && it->second->i == w.i && it->second->s == w.s) continue;
        bool ok = write_var(w.name, w.type, w.i, w.f, w.s);
        (ok ? (it == have.end() ? created : set) : failed)++;
    }
    if (zero_new)
        for (auto &v : now) {
            if (want.count(lower(v.name)) || (v.type == T_INT && !v.i) || (v.type == T_FLOAT && !v.f) ||
                (v.type == T_STRING && v.s.empty()))
                continue;
            if (write_var(v.name, v.type, 0, 0.0f, "")) zeroed++;
        }
    _snprintf(g_msg, sizeof g_msg - 1, "restored \"%s\": %d changed back, %d created, %d newer ones zeroed%s", s.name.c_str(),
              set, created, zeroed, failed ? " - some could not be written (type changed?)" : "");
}

// ------------------------------------------------------------------------------------------------ UI
static const char *kind_text(int k)
{
    switch (k) {
    case U_CREATE: return "created";
    case U_SET: return "set to";
    case U_COUNT: return "counted";
    case U_EXPR: return "set to";
    case U_CHECK: return "checked";
    case U_READ: return "read";
    }
    return "?";
}

static void place(const Use &u)                          // "File.Func" link (+ handler event)
{
    std::string t = u.file + "." + (u.func.empty() ? "?" : u.func);
    ImGui::PushID((int)u.ip);
    ImGui::PushID(t.c_str());
    if (ImGui::SmallButton(t.c_str()) && !dogdbg_show_code(u.file, u.ip))
        _snprintf(g_msg, sizeof g_msg - 1, "%s is not loaded right now - its code shows in Scripts when the game loads it",
                  u.file.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("show the code (Scripts tab) - offset %04X", u.ip);
    ImGui::PopID();
    ImGui::PopID();
    if (!u.event.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "on %s", u.event.c_str());
    }
}

static int handler_count(const std::string &file, const std::string &func);   // dogdbg.cpp (below, via flow_handler_count)

static void details(const std::string &key, const GVar *live, const FlowDb *db)
{
    const VarInfo *vi = nullptr;
    if (db) {
        auto it = db->vars.find(key);
        if (it != db->vars.end()) vi = &it->second;
    }
    std::string name = live ? live->name : vi ? vi->name : key;
    int type = live ? live->type : T_INT;
    if (!live && vi)
        for (auto &u : vi->uses) type = u.vtype;
    ImGui::Text("%s", name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(type == T_INT ? "int" : type == T_FLOAT ? "float" : "string");
    bool online = con_online();
    ImGui::BeginDisabled(online);
    static std::string edit_for;
    static int ei;
    static float ef;
    static char es[200];
    if (edit_for != key || (live && !ImGui::IsAnyItemActive())) {
        edit_for = key;
        ei = live ? live->i : 0, ef = live ? live->f : 0;
        strncpy(es, live ? live->s.c_str() : "", sizeof es - 1);
    }
    // number fields apply on Enter / leaving the field (ImGui's InputScalar asserts on EnterReturnsTrue)
    bool apply = false;
    if (type == T_INT) {
        if (ImGui::SmallButton("-")) ei--, apply = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputInt("##ei", &ei, 0, 0);
        apply |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) ei++, apply = true;
    } else if (type == T_FLOAT) {
        ImGui::SetNextItemWidth(160);
        ImGui::InputFloat("##ef", &ef, 0, 0, "%g");
        apply = ImGui::IsItemDeactivatedAfterEdit();
    } else {
        ImGui::SetNextItemWidth(160);
        apply = ImGui::InputText("##es", es, sizeof es, ImGuiInputTextFlags_EnterReturnsTrue);
    }
    if (apply && (live || type != T_STRING)) set_var(name, type, ei, ef, es, live ? nullptr : "created");
    ImGui::SameLine();
    if (!live) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "not created yet (the script that creates it has not run)");
    else ImGui::TextDisabled("now %s", fmt(*live).c_str());
    ImGui::EndDisabled();

    if (!vi) {
        ImGui::TextDisabled(db ? "No script names this variable (built from a computed name, or only the save has it)."
                               : "Building the script map...");
        return;
    }
    // flow points: the constants it is set to, and the values it is checked against
    std::map<int32_t, std::vector<const Use *>> pts;
    std::vector<const Use *> other;
    for (auto &u : vi->uses) {
        if ((u.kind == U_SET || u.kind == U_CHECK || u.kind == U_CREATE) && u.has_num) pts[u.num].push_back(&u);
        else other.push_back(&u);
    }
    if (!pts.empty()) {
        ImGui::SeparatorText("Flow points");
        ImGui::TextDisabled("the values scripts set it to or check for - Set jumps the quest there");
        if (ImGui::BeginTable("pts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("value");
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("where", ImGuiTableColumnFlags_WidthStretch);
            for (auto &kv : pts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool cur = live && live->type == T_INT && live->i == kv.first;
                if (cur) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%d  <- now", kv.first);
                else ImGui::Text("%d", kv.first);
                ImGui::TableNextColumn();
                ImGui::PushID(kv.first);
                ImGui::BeginDisabled(online || type != T_INT);
                if (ImGui::SmallButton("Set")) set_var(name, T_INT, kv.first, 0, "", "flow point");
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::TableNextColumn();
                for (size_t k = 0; k < kv.second.size(); k++) {
                    const Use &u = *kv.second[k];
                    if (k) ImGui::SameLine(0, 14);
                    ImGui::TextDisabled(u.kind == U_CHECK ? "checked %s" : u.kind == U_CREATE ? "starts" : "set",
                                        u.kind == U_CHECK ? u.value.substr(0, 2).c_str() : "");
                    ImGui::SameLine();
                    place(u);
                }
            }
            ImGui::EndTable();
        }
    }
    if (!other.empty()) {
        ImGui::SeparatorText("Other uses");
        for (auto *u : other) {
            if (u->kind == U_COUNT) {
                ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "counted %s", u->value.c_str());
                if (!u->event.empty()) {
                    int n = handler_count(u->file, u->func);
                    if (n > 1) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "(handler registered x%d now: counts %+d per %s)", n,
                                           u->num * n, u->event.c_str());
                    }
                }
            } else
                ImGui::TextDisabled("%s %s", kind_text(u->kind), u->value.c_str());
            ImGui::SameLine();
            place(*u);
        }
    }
    // this variable's history
    std::vector<Change> mine;
    {
        std::lock_guard<std::mutex> l(g_hist_lock);
        for (auto &c : g_hist)
            if (!c.bulk && !_stricmp(c.name.c_str(), name.c_str())) mine.push_back(c);
    }
    if (!mine.empty()) {
        ImGui::SeparatorText("Changes");
        for (auto it = mine.rbegin(); it != mine.rend() && it - mine.rbegin() < 20; ++it)
            ImGui::TextDisabled("%s  %s -> %s", it->when, it->from.empty() ? "(created)" : it->from.c_str(), it->to.c_str());
    }
}

void panel_quests()
{
    if (!ImGui::BeginTabItem("Quests")) return;
    if (!g_snaps_loaded) load_snaps();
    auto db = flow_db(true);
    static std::vector<GVar> vars;
    static DWORD last;
    if (dogdbg_stopped() || GetTickCount() - last > 300) {
        vars = read_vars();
        last = GetTickCount();
    }
    // toolbar: map, snapshots
    if (g_scanning) ImGui::TextDisabled("Reading the scripts...");
    else if (db) ImGui::TextDisabled("%d scripts mapped, %d variables named in them", db->files, (int)db->vars.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(g_scanning != 0);
    if (ImGui::SmallButton("Rescan")) start_scan();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("read every script again (after a sandbox edit or a mod change)");
    ImGui::SameLine(0, 30);
    static char snap_name[64];
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##sn", "snapshot name", snap_name, sizeof snap_name);
    ImGui::SameLine();
    if (ImGui::Button("Take snapshot")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char when[40];
        sprintf(when, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        Snapshot s{*snap_name ? snap_name : std::string("snapshot ") + std::to_string(g_snaps.size() + 1), when, read_vars()};
        s.name = safe_name(s.name);
        g_snaps.erase(std::remove_if(g_snaps.begin(), g_snaps.end(), [&](const Snapshot &x) { return x.name == s.name; }),
                      g_snaps.end());
        save_snap(s);
        g_snaps.push_back(s);
        _snprintf(g_msg, sizeof g_msg - 1, "snapshot \"%s\": %d variables (devtools\\snapshots)", s.name.c_str(), (int)s.vars.size());
        *snap_name = 0;
    }
    static int snap_sel = -1;
    static bool zero_new = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (snap_sel >= (int)g_snaps.size()) snap_sel = -1;
    if (ImGui::BeginCombo("##snaps", snap_sel >= 0 ? g_snaps[snap_sel].name.c_str() : "snapshots")) {
        for (int k = 0; k < (int)g_snaps.size(); k++) {
            std::string label = g_snaps[k].name + "   " + g_snaps[k].when;
            if (ImGui::Selectable(label.c_str(), k == snap_sel)) snap_sel = k;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(snap_sel < 0 || con_online());
    if (ImGui::Button("Restore")) {
        Snapshot s = g_snaps[snap_sel];
        bool z = zero_new;
        game_job([s, z] { restore(s, z); });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("put every game variable back to the snapshot (the world, party and gold stay as they are)");
    ImGui::SameLine();
    ImGui::Checkbox("zero newer ones", &zero_new);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("variables that changed from 0 / were created after the snapshot go back to 0");
    ImGui::SameLine();
    ImGui::BeginDisabled(snap_sel < 0);
    if (ImGui::SmallButton("Delete")) {
        DeleteFileA((snap_dir() + g_snaps[snap_sel].name + ".txt").c_str());
        g_snaps.erase(g_snaps.begin() + snap_sel);
        snap_sel = -1;
    }
    ImGui::EndDisabled();
    if (con_online()) ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "online: read-only");
    else if (*g_msg) ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "%s", g_msg);
    else ImGui::TextDisabled("Game variables are saved with the game. Set one to a flow point, or snapshot before a quest step "
                             "and restore to play it again.");

    // left: the variables
    static char filter[64];
    static bool only_named, show_missing, only_changed;
    static std::string sel;
    ImGui::BeginChild("vars", ImVec2(ImGui::GetContentRegionAvail().x * 0.42f, 0), ImGuiChildFlags_ResizeX);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##vf", "filter (qstPriam, Towns, ...)", filter, sizeof filter);
    ImGui::Checkbox("in scripts", &only_named);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("only variables a script names");
    ImGui::SameLine();
    ImGui::Checkbox("not created yet", &show_missing);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("also list variables scripts use that the game does not have yet");
    ImGui::SameLine();
    ImGui::Checkbox("changed", &only_changed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("only ones changed this session");
    std::unordered_map<std::string, DWORD> changed;     // lower name -> recency rank
    {
        std::lock_guard<std::mutex> l(g_hist_lock);
        DWORD k = 0;
        for (auto &c : g_hist)
            if (!c.bulk) changed[lower(c.name)] = ++k;
    }
    struct Row { std::string key; const GVar *live; };
    std::vector<Row> rows;
    std::unordered_map<std::string, bool> seen;
    for (auto &v : vars) {
        std::string k = lower(v.name);
        seen[k] = true;
        rows.push_back({k, &v});
    }
    if (show_missing && db)
        for (auto &kv : db->vars)
            if (!seen.count(kv.first)) rows.push_back({kv.first, nullptr});
    if (show_missing)
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.key < b.key; });
    if (ImGui::BeginTable("gv", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("variable", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("uses", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();
        size_t total = changed.size();
        for (auto &r : rows) {
            const VarInfo *vi = nullptr;
            if (db) {
                auto it = db->vars.find(r.key);
                if (it != db->vars.end()) vi = &it->second;
            }
            std::string name = r.live ? r.live->name : vi ? vi->name : r.key;
            if (only_named && !vi) continue;
            auto ch = changed.find(r.key);
            if (only_changed && ch == changed.end()) continue;
            if (*filter && !StrStrIA(name.c_str(), filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool counter = false;
            if (vi)
                for (auto &u : vi->uses) counter |= u.kind == U_COUNT;
            if (ch != changed.end() && total && ch->second + 5 > total)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1, 0.5f, 1));
            else if (!r.live) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1));
            else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            if (ImGui::Selectable(name.c_str(), sel == r.key, ImGuiSelectableFlags_SpanAllColumns)) sel = r.key;
            ImGui::PopStyleColor();
            if (counter && ImGui::IsItemHovered()) ImGui::SetTooltip("a counter: scripts add to / subtract from it");
            ImGui::TableNextColumn();
            if (r.live) ImGui::TextUnformatted(fmt(*r.live).c_str());
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (vi) {
                if (counter) ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%d+", (int)vi->uses.size());
                else ImGui::TextDisabled("%d", (int)vi->uses.size());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // right: the selected one, then the session's changes
    ImGui::BeginChild("detail", ImVec2(0, 0));
    if (!sel.empty()) {
        const GVar *live = nullptr;
        for (auto &v : vars)
            if (lower(v.name) == sel) live = &v;
        ImGui::BeginChild("one", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.62f), ImGuiChildFlags_ResizeY);
        details(sel, live, db.get());
        ImGui::EndChild();
    } else
        ImGui::TextDisabled("Pick a variable to see its flow points and where scripts set and check it.");
    ImGui::SeparatorText("Changes this session");
    std::vector<Change> hist;
    {
        std::lock_guard<std::mutex> l(g_hist_lock);
        hist.assign(g_hist.begin(), g_hist.end());
    }
    if (hist.empty()) ImGui::TextDisabled("none yet");
    else if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> l(g_hist_lock);
        g_hist.clear();
    }
    ImGui::BeginChild("hist");
    for (int k = (int)hist.size() - 1; k >= 0; k--) {
        const Change &c = hist[k];
        ImGui::PushID(k);
        if (c.bulk) ImGui::TextDisabled("%s  %d variables changed at once (a save loaded / new game)", c.when, c.bulk);
        else {
            ImGui::TextDisabled("%s", c.when);
            ImGui::SameLine();
            if (ImGui::Selectable(c.name.c_str(), sel == lower(c.name), 0, ImVec2(ImGui::CalcTextSize(c.name.c_str()).x, 0)))
                sel = lower(c.name);
            ImGui::SameLine();
            ImGui::Text("%s -> %s", c.from.empty() ? "(created)" : c.from.c_str(), c.to.c_str());
            if (!c.from.empty()) {
                ImGui::SameLine();
                ImGui::BeginDisabled(con_online());
                if (ImGui::SmallButton("undo")) set_var(c.name, c.type, c.old_i, c.old_f, c.old_s, "undo");
                ImGui::EndDisabled();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndTabItem();
}

// how many times file.func is registered as an event handler right now (dogdbg.cpp reads the list)
int dogdbg_handler_count(const std::string &file, const std::string &func);
static int handler_count(const std::string &file, const std::string &func) { return dogdbg_handler_count(file, func); }

// the handlers file registers that count ("OnBeatTown (BeatTown): ImperiaTownsToBeat -1"), "" = none / map not built
std::string flow_counting_handlers(const std::string &file)
{
    auto db = flow_db(true);
    if (!db) return "";
    std::string out, st = stem(file);
    for (auto &r : db->regs) {
        if (_stricmp(r.file.c_str(), st.c_str())) continue;
        std::string e = flow_effects(st, r.func, true);
        if (!e.empty()) out += (out.empty() ? "" : ", ") + r.func + " (" + r.event + "): " + e;
    }
    return out;
}

// console: "gamevar [text]" lists, "gamevar <name> <value>" sets (game thread: written now)
void flow_gamevar(const std::string &arg)
{
    size_t sp = arg.find(' ');
    if (sp != std::string::npos && arg.find_first_not_of(' ', sp) != std::string::npos) {
        if (con_online()) { con_print(OUT_ERROR, "gamevar: read-only online"); return; }
        std::string name = arg.substr(0, sp), val = arg.substr(arg.find_first_not_of(' ', sp));
        uint32_t v = find_val(name);
        int type = v ? (int)rd32(v + 8) : (val.find('.') != std::string::npos ? T_FLOAT : T_INT);
        if (type == T_STRING && val.size() > 1 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        bool ok = write_var(name, type, atoi(val.c_str()), (float)atof(val.c_str()), val);
        GVar g{name, type, atoi(val.c_str()), (float)atof(val.c_str()), val, 0};
        if (ok) con_print(OUT_TEXT, "%s%s = %s", name.c_str(), v ? "" : " (created)", fmt(g).c_str());
        else con_print(OUT_ERROR, "gamevar: could not set %s", name.c_str());
        return;
    }
    int n = 0;
    for (auto &v : read_vars())
        if (arg.empty() || StrStrIA(v.name.c_str(), arg.c_str())) {
            con_print(OUT_HELP, "  %-36s = %s", v.name.c_str(), fmt(v).c_str());
            n++;
        }
    con_print(OUT_TEXT, "%d game variables", n);
}

// console: "gameflow <name>" - the map of one variable (the first use waits for the scripts to be read)
void flow_gameflow(const std::string &arg)
{
    if (arg.empty()) { con_print(OUT_ERROR, "usage: gameflow <variable name>"); return; }
    auto db = flow_db(true);
    for (int t = 0; !db && t < 300; t++) {
        Sleep(50);
        db = flow_db(false);
    }
    if (!db) { con_print(OUT_ERROR, "gameflow: the script map is not ready yet"); return; }
    auto it = db->vars.find(lower(arg));
    if (it == db->vars.end()) {
        con_print(OUT_TEXT, "no script names %s (%d scripts mapped)", arg.c_str(), db->files);
        return;
    }
    uint32_t v = find_val(arg);
    GVar now;
    std::string cur = "not created";
    if (v) {
        for (int b = 0; b < 64; b++)
            for (uint32_t node = rd32(VAR_TABLE + b * 8); node; node = rd32(node))
                if (rd32(node + 0x10) == v && read_var(node, now)) cur = fmt(now);
    }
    con_print(OUT_TEXT, "%s = %s  (%d uses in %d scripts mapped)", it->second.name.c_str(), cur.c_str(),
              (int)it->second.uses.size(), db->files);
    for (auto &u : it->second.uses) {
        int n = u.kind == U_COUNT && !u.event.empty() ? dogdbg_handler_count(u.file, u.func) : 0;
        con_print(OUT_HELP, "  %-8s %-14s %s.%s @%04X%s%s%s", kind_text(u.kind), u.value.c_str(), u.file.c_str(),
                  u.func.c_str(), u.ip, u.event.empty() ? "" : " on ", u.event.c_str(),
                  n > 1 ? "  (handler registered twice or more now!)" : "");
    }
}
