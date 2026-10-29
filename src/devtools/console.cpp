// Console front end on the retail console engine. Retail still has the whole engine (registry, parser, expression
// evaluator, aliases, key binds) but its input, drawing and every line of output were compiled out. Lines typed here
// are queued and run on the game thread through console_ExecuteString (0x181500); output the retail commands no longer
// print (help, variable values, usage) is produced here from the registry.
//
// Registry (retail VAs): list heads commands 0x481CF4, vars 0x481CFC, aliases 0x481D04 (next at +0xC). Item: +0 name,
// +4 desc, +8 u16 flags, +0xC next. Var: +0x14 type, +0x18 count, +0x1C float min, +0x20 float max, +0x24 data.
// Alias: +0x14 command text. console_ShowUsage 0x17D790 (a bare `ret` in retail, 47 callers) is hooked for usage text.
#include "devtools.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <mutex>

bool g_log_console;                                     // [devtools] LogConsole=1: console output also to port.log
static std::mutex g_out_lock;
static std::deque<OutLine> g_out;
static size_t g_out_gen;
static const size_t OUT_MAX = 4000;

static std::mutex g_q_lock;
static std::vector<std::string> g_queue;
static std::vector<std::string> g_history;

static std::mutex g_reg_lock;
static std::vector<RegEntry> g_reg;
static size_t g_reg_gen;
static int g_reg_counts[3] = {-1, -1, -1};

#define AT(T, p, off) (*(T *)((uint8_t *)(p) + (off)))

// ------------------------------------------------------------------------------------------------ output
void con_print(OutKind kind, const char *fmt, ...)
{
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, a);
    va_end(a);
    buf[sizeof buf - 1] = 0;
    if (g_log_console) g_host->log("con: %s\n", buf);
    std::lock_guard<std::mutex> l(g_out_lock);
    for (char *p = buf, *e; *p; p = e) {               // one entry per line
        e = strchr(p, '\n');
        std::string s = e ? std::string(p, e) : std::string(p);
        if (e) e++; else e = p + strlen(p);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        g_out.push_back({kind, s});
        if (g_out.size() > OUT_MAX) g_out.pop_front();
    }
    g_out_gen++;
}

bool con_tail(TailFn fn, void *ctx, size_t n)
{
    std::unique_lock<std::mutex> l(g_out_lock, std::try_to_lock);
    if (!l.owns_lock()) return false;
    for (size_t i = g_out.size() > n ? g_out.size() - n : 0; i < g_out.size(); i++) fn(g_out[i].text.c_str(), ctx);
    return true;
}

std::vector<OutLine> con_lines(size_t *gen)
{
    std::lock_guard<std::mutex> l(g_out_lock);
    if (gen) *gen = g_out_gen;
    return std::vector<OutLine>(g_out.begin(), g_out.end());
}

void con_clear()
{
    std::lock_guard<std::mutex> l(g_out_lock);
    g_out.clear();
    g_out_gen++;
}

const std::vector<std::string> &con_history() { return g_history; }

bool con_online() { return g_host->net_running && g_host->net_running(); }

// ------------------------------------------------------------------------------------------------ registry
static const DWORD HEADS[3] = {0x481CF4, 0x481CFC, 0x481D04};

static int list_count(int k)
{
    int n = 0;
    for (uint8_t *p = *(uint8_t **)HEADS[k]; p && n < 100000; p = AT(uint8_t *, p, 0xC)) n++;
    return n;
}

static std::string cstr(const char *s) { return s ? std::string(s) : std::string(); }

// A command's usage text: its handler passes it to console_ShowUsage (0x17D790) as `push imm32; call rel32`. Retail
// compiled the text output out but kept these strings, so the syntax of every command can be shown up front.
static std::string handler_usage(const uint8_t *cb)
{
    std::string out;
    if (!cb || (uintptr_t)cb < 0x12000 || (uintptr_t)cb > 0x300000) return out;
    for (int i = 0; i < 0x900; i++) {
        if (cb[i] != 0x68 || cb[i + 5] != 0xE8) continue;
        const uint8_t *target = cb + i + 10 + *(const int32_t *)(cb + i + 6);
        if (target != (const uint8_t *)0x17D790) continue;
        const char *s = *(const char *const *)(cb + i + 1);
        if ((uintptr_t)s < 0x300000 || (uintptr_t)s > 0x400000) continue;
        std::string t = s;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        while (!t.empty() && t[0] == '\n') t.erase(0, 1);
        if (t.empty() || out.find(t) != std::string::npos) continue;
        if (!out.empty()) out += "\n";
        out += t;
    }
    return out;
}

static void registry_refresh()                          // game thread
{
    int c[3] = {list_count(0), list_count(1), list_count(2)};
    if (!memcmp(c, g_reg_counts, sizeof c)) return;
    std::vector<RegEntry> r;
    for (int k = 0; k < 3; k++)
        for (uint8_t *p = *(uint8_t **)HEADS[k]; p; p = AT(uint8_t *, p, 0xC)) {
            RegEntry e;
            e.kind = (RegEntry::Kind)k;
            e.name = cstr(AT(const char *, p, 0));
            e.desc = cstr(AT(const char *, p, 4));
            e.flags = AT(uint16_t, p, 8);
            if (k == RegEntry::VAR) {
                e.vartype = AT(int, p, 0x14);
                e.count = AT(int, p, 0x18);
                e.min = AT(float, p, 0x1C);
                e.max = AT(float, p, 0x20);
                e.data = AT(void *, p, 0x24);
            } else if (k == RegEntry::CMD)
                e.usage = handler_usage(AT(const uint8_t *, p, 0x14));
            else if (k == RegEntry::ALIAS)
                e.alias_text = cstr(AT(const char *, p, 0x14));
            if (!e.name.empty()) r.push_back(e);
        }
    std::sort(r.begin(), r.end(), [](const RegEntry &a, const RegEntry &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::lock_guard<std::mutex> l(g_reg_lock);
    g_reg.swap(r);
    g_reg_gen++;
    memcpy(g_reg_counts, c, sizeof c);
}

std::vector<RegEntry> con_registry(size_t *gen)
{
    std::lock_guard<std::mutex> l(g_reg_lock);
    if (gen) *gen = g_reg_gen;
    return g_reg;
}

static const RegEntry *find_entry(const std::vector<RegEntry> &r, const std::string &name)
{
    for (auto &e : r)
        if (!_stricmp(e.name.c_str(), name.c_str())) return &e;
    return nullptr;
}

std::string con_var_value(const RegEntry &v)
{
    if (!v.data) return "?";
    std::string out;
    int n = v.count > 1 ? (v.count > 8 ? 8 : v.count) : 1;
    for (int i = 0; i < n; i++) {
        char b[64];
        uint8_t *d = (uint8_t *)v.data;
        switch (v.vartype) {
        case 0: sprintf(b, "%u", d[i]); break;
        case 1: sprintf(b, "%d", ((int8_t *)d)[i]); break;
        case 2: sprintf(b, "%u", ((uint16_t *)d)[i]); break;
        case 3: sprintf(b, "%d", ((int16_t *)d)[i]); break;
        case 4: sprintf(b, "%u", ((uint32_t *)d)[i]); break;
        case 5: sprintf(b, "%d", ((int32_t *)d)[i]); break;
        case 6: sprintf(b, "%g", ((float *)d)[i]); break;
        case 7: sprintf(b, "0x%X", ((uint32_t *)d)[i]); break;
        default: sprintf(b, "?"); break;
        }
        if (i) out += ", ";
        out += b;
    }
    if (v.count > 8) out += ", ...";
    return out;
}

static const char *type_name(int t)
{
    static const char *const k[] = {"ubyte", "sbyte", "ushort", "sshort", "uint", "int", "float", "hex"};
    return t >= 0 && t < 8 ? k[t] : "?";
}

// ------------------------------------------------------------------------------------------------ built-in commands
struct Builtin { const char *name, *desc; };
static const Builtin k_builtins[] = {
    {"help", "help [text]: commands and variables (optionally containing text); 'cmd ?' shows a command's usage"},
    {"find", "find text: search names and descriptions"},
    {"cmds", "cmds [text]: list the game's commands"},
    {"vars", "vars [text]: list the game's variables with their values"},
    {"aliases", "aliases: list aliases"},
    {"cls", "cls: clear the console"},
    {"crashreport", "crashreport: write a crash-style state report now (crash\\manual_report.txt)"},
    {"crashtest", "crashtest: crash the game on purpose (tests the crash report and dump)"},
    {"scriptbreak", "scriptbreak: switch the script debugger on and stop at the next script instruction (Scripts tab)"},
    {"scriptswap", "scriptswap <file> <path>: replace a loaded script with new code (.scp compiled, or .scb) - sandbox, "
     "until the game restarts"},
    {"scriptreset", "scriptreset <file> [<file> ...] [-noinit] [-kill]: reset scripts together - fresh variables, "
     "running ones stopped, OnInit run again (not with -noinit), OnKill first with -kill (never where it changes the "
     "game state)"},
    {"scriptdedup", "scriptdedup: remove duplicate script event handlers (a reset without OnKill registers them again)"},
    {"gamevar", "gamevar [text]: list the game variables (quest progress, saved with the game) containing text; "
     "gamevar <name> <value>: set one (created if missing)"},
    {"gameflow", "gameflow <name>: where scripts create, set, count and check a game variable (its flow points)"},
};

static bool is_builtin(const std::string &n)
{
    for (auto &b : k_builtins)
        if (!_stricmp(b.name, n.c_str())) return true;
    return false;
}

static bool contains_i(const std::string &hay, const std::string &needle)
{
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != hay.end();
}

static void list_entries(const std::vector<RegEntry> &r, int kind, const std::string &filter, bool desc_too)
{
    int n = 0;
    for (auto &e : r) {
        if (kind >= 0 && e.kind != kind) continue;
        if (!contains_i(e.name, filter) && !(desc_too && contains_i(e.desc, filter))) continue;
        if (e.kind == RegEntry::VAR)
            con_print(OUT_HELP, "  %-28s = %-12s %s [%g..%g] %s", e.name.c_str(), con_var_value(e).c_str(),
                      type_name(e.vartype), e.min, e.max, e.desc.c_str());
        else if (e.kind == RegEntry::ALIAS)
            con_print(OUT_HELP, "  %-28s alias: %s", e.name.c_str(), e.alias_text.c_str());
        else {
            con_print(OUT_HELP, "  %-28s %s", e.name.c_str(), e.desc.c_str());
            if (!e.usage.empty()) {
                std::string u = e.usage;
                for (size_t q = 0; (q = u.find('\n', q)) != std::string::npos; q += 7) u.replace(q, 1, "\n      ");
                con_print(OUT_TEXT, "      %s", u.c_str());
            }
        }
        n++;
    }
    con_print(OUT_TEXT, "%d found", n);
}

static void run_builtin(const std::string &cmd, const std::string &arg)
{
    std::vector<RegEntry> r = con_registry(nullptr);
    if (!_stricmp(cmd.c_str(), "cls")) con_clear();
    else if (dt_release() && (!_stricmp(cmd.c_str(), "crashreport") || !_stricmp(cmd.c_str(), "crashtest")))
        con_print(OUT_ERROR, "%s: not in release builds (no crash reports or dumps)", cmd.c_str());
    else if (!_stricmp(cmd.c_str(), "crashreport")) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        strcpy(slash ? slash + 1 : path, "crash\\manual_report.txt");
        crash_report(nullptr, path, "manual");
        con_print(OUT_TEXT, "wrote %s", path);
    } else if (!_stricmp(cmd.c_str(), "crashtest")) {
        if (con_online()) { con_print(OUT_ERROR, "crashtest: not in an online match"); return; }
        g_host->log("devtools: crashtest - writing to address 0 on purpose\n");
        *(volatile int *)(uintptr_t)arg.size() = 1;      // NULL (+0) write on the game thread
    }
    else if (!_stricmp(cmd.c_str(), "scriptbreak")) {
        if (con_online()) { con_print(OUT_ERROR, "scriptbreak: not in an online match"); return; }
        dogdbg_break_next();
        con_print(OUT_TEXT, "script debugger on - stopping at the next script instruction");
    }
    else if (!_stricmp(cmd.c_str(), "scriptreset")) {
        if (con_online()) { con_print(OUT_ERROR, "scriptreset: not in an online match"); return; }
        std::vector<std::string> names;
        bool init = true, kill = false;
        for (size_t a = 0; a < arg.size();) {
            size_t b = arg.find(' ', a);
            std::string w = arg.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!_stricmp(w.c_str(), "-noinit")) init = false;
            else if (!_stricmp(w.c_str(), "-kill")) kill = true;
            else if (!w.empty()) names.push_back(w);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (names.empty()) { con_print(OUT_ERROR, "usage: scriptreset <file> [<file> ...] [-noinit] [-kill]"); return; }
        con_print(OUT_TEXT, "%s", dogdbg_reset_files(names, kill, init).c_str());
    }
    else if (!_stricmp(cmd.c_str(), "scriptdedup")) {
        if (con_online()) { con_print(OUT_ERROR, "scriptdedup: not in an online match"); return; }
        dogdbg_dedup_all();
        con_print(OUT_TEXT, "removing duplicate event handlers at the next frame (result in the log)");
    }
    else if (!_stricmp(cmd.c_str(), "gamevar")) flow_gamevar(arg);
    else if (!_stricmp(cmd.c_str(), "gameflow")) flow_gameflow(arg);
    else if (!_stricmp(cmd.c_str(), "scriptswap")) {
        if (con_online()) { con_print(OUT_ERROR, "scriptswap: not in an online match"); return; }
        size_t sp = arg.find(' ');
        if (sp == std::string::npos) { con_print(OUT_ERROR, "usage: scriptswap <file> <path to .scp or .scb>"); return; }
        std::string path = arg.substr(sp + 1);
        if (path.size() > 1 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        con_print(OUT_TEXT, "%s", dogdbg_swap_file(arg.substr(0, sp), path).c_str());
    }
    else if (!_stricmp(cmd.c_str(), "cmds")) list_entries(r, RegEntry::CMD, arg, false);
    else if (!_stricmp(cmd.c_str(), "vars")) list_entries(r, RegEntry::VAR, arg, false);
    else if (!_stricmp(cmd.c_str(), "aliases")) list_entries(r, RegEntry::ALIAS, arg, false);
    else if (!_stricmp(cmd.c_str(), "find")) list_entries(r, -1, arg, true);
    else {                                              // help
        if (arg.empty()) {
            con_print(OUT_HELP, "Console commands:");
            for (auto &b : k_builtins) con_print(OUT_HELP, "  %s", b.desc);
            con_print(OUT_HELP, "Game: %d commands, %d variables, %d aliases. 'name' shows a variable, 'name=value' sets "
                      "it (expressions allowed), ';' separates commands, Tab completes.",
                      (int)std::count_if(r.begin(), r.end(), [](const RegEntry &e) { return e.kind == RegEntry::CMD; }),
                      (int)std::count_if(r.begin(), r.end(), [](const RegEntry &e) { return e.kind == RegEntry::VAR; }),
                      (int)std::count_if(r.begin(), r.end(), [](const RegEntry &e) { return e.kind == RegEntry::ALIAS; }));
        } else
            list_entries(r, -1, arg, true);
    }
}

// ------------------------------------------------------------------------------------------------ online safety
// Online, only commands that do not change the game are allowed (each ';' part, aliases expanded); no assignments.
static const char *const k_online_ok[] = {"mem", "mempool", "panlist", "paxlist", "fx", "EventHandlers", "files"};
// commands whose 'list' form only reads
static const char *const k_online_list[] = {"heap", "gvar", "wmWarp", "wmEntity", "wmProp", "protiushelp"};

static std::vector<std::string> split_parts(const std::string &line)
{
    std::vector<std::string> parts;
    std::string cur;
    bool quote = false;
    for (char ch : line) {
        if (ch == '"') quote = !quote;
        if (ch == ';' && !quote) { parts.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    parts.push_back(cur);
    for (auto &p : parts) {
        size_t a = p.find_first_not_of(" \t"), b = p.find_last_not_of(" \t");
        p = a == std::string::npos ? std::string() : p.substr(a, b - a + 1);
    }
    parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
    return parts;
}

static std::string first_token(const std::string &part)
{
    size_t e = part.find_first_of(" \t=,[");
    return part.substr(0, e);
}

static bool online_allowed(const std::vector<RegEntry> &r, const std::string &part, int depth, std::string &why)
{
    std::string name = first_token(part);
    if (is_builtin(name)) return true;
    const RegEntry *e = find_entry(r, name);
    if (part.find('=') != std::string::npos) { why = "changing variables"; return false; }
    if (e && e->kind == RegEntry::VAR) return part == name;   // reading a variable
    if (e && e->kind == RegEntry::ALIAS) {
        if (depth > 4) { why = "nested aliases"; return false; }
        for (auto &p : split_parts(e->alias_text))
            if (!online_allowed(r, p, depth + 1, why)) return false;
        return true;
    }
    for (auto *ok : k_online_ok)
        if (!_stricmp(ok, name.c_str())) return true;
    std::string rest = part.substr(name.size());
    size_t a0 = rest.find_first_not_of(" 	,");
    bool list = a0 != std::string::npos && !_strnicmp(rest.c_str() + a0, "list", 4);
    for (auto *ok : k_online_list)
        if (!_stricmp(ok, name.c_str()) && list) return true;
    why = "'" + name + "'";
    return false;
}

// ------------------------------------------------------------------------------------------------ execution
void con_submit(const char *line)
{
    std::string s = line;
    if (s.empty()) return;
    std::lock_guard<std::mutex> l(g_q_lock);
    g_queue.push_back(s);
    if (g_history.empty() || g_history.back() != s) g_history.push_back(s);
    if (g_history.size() > 200) g_history.erase(g_history.begin());
}

static bool alias_ok(const std::vector<RegEntry> &r, const std::string &part, int depth);

static void execute(const std::string &line)            // game thread
{
    con_print(OUT_ECHO, "> %s", line.c_str());
    std::vector<RegEntry> r = con_registry(nullptr);
    bool online = con_online();
    for (auto &part : split_parts(line)) {
        std::string name = first_token(part);
        std::string arg = part.size() > name.size() ? part.substr(name.size()) : std::string();
        size_t a = arg.find_first_not_of(" \t");
        arg = a == std::string::npos ? std::string() : arg.substr(a);
        std::string why;
        if (online && !online_allowed(r, part, 0, why)) {
            con_print(OUT_ERROR, "online match: %s is blocked (it would change the game for one player only)", why.c_str());
            continue;
        }
        if (is_builtin(name)) { run_builtin(name, arg); continue; }
        const RegEntry *e = find_entry(r, name);
        if (!e) { con_print(OUT_ERROR, "unknown command or variable '%s' (try help or find)", name.c_str()); continue; }
        if (!alias_ok(r, part, 0)) { con_print(OUT_ERROR, "alias '%s' expands into itself (would crash the game)", name.c_str()); continue; }
        if (e->kind == RegEntry::VAR && part == name) {  // show a value: retail prints nothing
            con_print(OUT_TEXT, "%s = %s  (%s [%g..%g]) %s", e->name.c_str(), con_var_value(*e).c_str(),
                      type_name(e->vartype), e->min, e->max, e->desc.c_str());
            continue;
        }
        std::vector<std::string> args;                  // the retail parser splits on spaces and commas
        if (e->kind == RegEntry::CMD) {
            std::string cur;
            for (char ch : arg + " ") {
                if (ch == ' ' || ch == ',' || ch == '	') { if (!cur.empty()) args.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            if (!cmdout_pre(e->name, args)) continue;
        }
        char buf[256];
        strncpy(buf, part.c_str(), sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        g_host->log("devtools: exec '%s'\n", buf);
        InterlockedIncrement(&g_exec_depth);
        ((void(__cdecl *)(char *))0x181500)(buf);
        InterlockedDecrement(&g_exec_depth);
        if (e->kind == RegEntry::VAR) {                 // assignment: echo the (clamped) result
            con_print(OUT_TEXT, "%s = %s", e->name.c_str(), con_var_value(*e).c_str());
            g_host->log("devtools: %s = %s\n", e->name.c_str(), con_var_value(*e).c_str());
        } else if (e->kind == RegEntry::CMD)
            cmdout_post(e->name, args);
    }
}

// ------------------------------------------------------------------------------------------------ re-registered commands
// Commands whose code is intact in retail but which never get registered: the flag-1 ones InitGameConsoleVars skips
// (0x6CFA2..), the dead unit/KeroOrder registrations, and the console built-ins console_Init (0x1820D0, no callers)
// would have added. console_AddCommand 0x181CA0 (name, flags, cb, desc) only skips on flags & 1. None of these is in
// the online allow list, so all are blocked in online matches.
struct Rereg { const char *name; uint32_t cb; const char *desc; };
static const Rereg k_rereg[] = {
    {"unit", 0xDAAC0, "unit changehp|changeaff|setcorestat|setstackval|move|anim|mhide|mshow,... (battle units)"},
    {"KeroOrder", 0x7C790, "KeroOrder Attack,<unit> | Move,<x>,<y> (battle order phase)"},
    {"StoryEvent", 0x41F20, "StoryEvent <name> | set,<name> | next: fire story events"},
    {"ChangeState", 0x1F580, "ChangeState <STATE>: switch game state next frame (risky)"},
    {"TweakUnit", 0xD7F60, "TweakUnit <unit>,<field>,<value>: write a unit field directly (risky)"},
    {"calc", 0x1F4FA0, "calc expr[,expr]: evaluate expressions"},
    {"echo", 0x1F5030, "echo text"},
    {"alias", 0x181D80, "alias name,\"cmd %1%;cmd2\": define an alias"},
    {"delete", 0x181E70, "delete alias,name: remove an alias"},
};

static void register_hidden()
{
    std::vector<RegEntry> r = con_registry(nullptr);
    int added = 0;
    for (auto &c : k_rereg) {
        if (find_entry(r, c.name)) continue;
        ((void(__cdecl *)(const char *, int, uint32_t, const char *))0x181CA0)(c.name, 0, c.cb, c.desc);
        added++;
    }
    g_host->log("devtools: registered %d commands the release build leaves out\n", added);
    registry_refresh();
}

// Retail ExecuteCommand expands aliases with no depth limit: `alias a,"a"` then `a` overflows the game thread's stack
static bool alias_ok(const std::vector<RegEntry> &r, const std::string &part, int depth)
{
    if (depth > 8) return false;
    const RegEntry *e = find_entry(r, first_token(part));
    if (!e || e->kind != RegEntry::ALIAS) return true;
    for (auto &p : split_parts(e->alias_text))
        if (!alias_ok(r, p, depth + 1)) return false;
    return true;
}

// [devtools] Startup=cmd;cmd...: run once, StartupFrame frames after the game starts (testing)
static std::string g_startup;
static int g_startup_frame = 600;

void con_frame()
{
    static int tick;
    static bool reregistered;
    if (!reregistered && tick % 30 == 0) {              // after the game's own registrations (gvar = GlobalVarInit)
        std::vector<RegEntry> r = con_registry(nullptr);
        if (find_entry(r, "gvar")) { register_hidden(); reregistered = true; }
    }
    if (tick == g_startup_frame && !g_startup.empty()) con_submit(g_startup.c_str());
    if (tick++ % 30 == 0) registry_refresh();
    std::vector<std::string> q;
    {
        std::lock_guard<std::mutex> l(g_q_lock);
        q.swap(g_queue);
    }
    if (!q.empty()) registry_refresh();
    for (auto &line : q) execute(line);
}

std::vector<std::string> con_complete(const std::string &prefix)
{
    std::vector<std::string> out;
    for (auto &b : k_builtins)
        if (!_strnicmp(b.name, prefix.c_str(), prefix.size())) out.push_back(b.name);
    for (auto &e : con_registry(nullptr))
        if (!_strnicmp(e.name.c_str(), prefix.c_str(), prefix.size())) out.push_back(e.name);
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ------------------------------------------------------------------------------------------------ usage text hook
// console_ShowUsage(const char *usage): the current item's name is at [0x48065C]
static void __cdecl show_usage(const char *usage)
{
    const char *name = *(const char **)0x48065C;
    con_print(OUT_HELP, "%s: %s", name ? name : "usage", usage ? usage : "");
}

void con_init()
{
    char buf[1024];
    g_log_console = GetPrivateProfileIntA("devtools", "LogConsole", 0, g_host->ini) != 0;
    if (GetPrivateProfileStringA("devtools", "Startup", "", buf, sizeof buf, g_host->ini)) g_startup = buf;
    g_startup_frame = GetPrivateProfileIntA("devtools", "StartupFrame", 600, g_host->ini);
    uint8_t *p = (uint8_t *)0x17D790;
    if (p[0] == 0xC3 && p[1] == 0xCC && p[2] == 0xCC && p[3] == 0xCC && p[4] == 0xCC) {
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE9;
        *(int32_t *)(p + 1) = (int32_t)((uint8_t *)show_usage - (p + 5));
        VirtualProtect(p, 5, old, &old);
    } else
        g_host->log("devtools: console_ShowUsage not the expected stub - usage text off\n");
    con_print(OUT_HELP, "GladiusPort developer console. Type help. Tab completes, Up/Down history.");
}

void con_shutdown()                                     // the usage hook back to retail's stub (ret)
{
    static const uint8_t stub[5] = {0xC3, 0xCC, 0xCC, 0xCC, 0xCC};
    uint8_t *p = (uint8_t *)0x17D790;
    if (p[0] != 0xE9) return;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, stub, 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}
