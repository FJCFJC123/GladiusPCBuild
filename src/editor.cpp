// Level editor (gladius_editor.exe): the editor window, level reload, live prop editing and picking.
// Reload (Ctrl+R / Save + Reload): edited files go to the loose data folder, then the current game state is requested
// again, which the state manager (0x20E80) treats as a full change - so the level reloads with the edited files.
// .prp grammar: VERSION, then per prop: PROP "name", MODELS "path.pax" | No, LIGHTING ON|OFF, SHADOW ON|OFF,
// [SCRIPTNAME "..."], HEIGHTS f, [HEIGHTOFFSET f], POS x y z, ROT x y z (radians), ATTRIBUTES n, n x ATTRIBUTE
// "No_Stand" | "No_Cursor" | "invisible" | "No_Modify_Heights", PROP_END. Unknown lines are kept as they are.
#include "port.h"
#include <string>
#include <vector>
#include <algorithm>
#include <stdarg.h>
#include <math.h>
#include "overlay.h"
#include "editor.h"
#include <commctrl.h>

void port_mods_rescan();                                // mods.cpp
const char *port_game_dir();                            // xapi_file.cpp

// Reload the current level (or battle) with the edited files.
// ------------------------------------------------------------------------------------------------ reload
void port_editor_reload()
{
    if (!port_is_editor())
        return;
    port_mods_rescan();
    uint8_t *state = *(uint8_t *const *)(0x38E448 + 0x14);
    int id = state ? *(const int *)(state + 8) : 7;
    if (id == 12) {
        // a battle is rebuilt from pre-battle state 11 (re-requesting 12 crashes in the state factory). With byte 0x418400
        // set, state 11 reloads the encounter (0x394DF0), skips placement and goes on to 12.
        *(volatile uint8_t *)0x418400 = 1;
        *(volatile int *)0x38E43C = 11;
        port_log("editor: battle reload - encounter loaded again (pre-battle state 11, placement skipped)\n");
        return;
    }
    *(volatile int *)0x38E43C = id;                     // RequestState(current): fade + reload
    port_log("editor: reload - state %d requested again\n", id);
}

// ------------------------------------------------------------------------------------------------ file service
static CRITICAL_SECTION g_cs;
static std::vector<std::string> g_seen;                 // "data/levels/.../x.prp", newest last
static std::string g_enc;                               // the encounter (.enc) last loaded, "data/encounters/..."
static bool g_seen_changed;
static std::string g_auto_open;                         // a prop file the game just loaded: the window opens it
static volatile LONG g_frames;                          // port_editor_frame calls (the game thread is free to load)
static struct {
    std::string path;                                   // pending load ("" = none)
    std::string text;
    bool ok, done;
    HANDLE ev;
} g_req;

static std::string key_of(const char *name)            // "d:\data\Levels\x.prp" -> "data/levels/x.prp"
{
    std::string k;
    for (const char *p = name; *p; p++) {
        char c = *p == 0x5C ? '/' : (char)tolower((unsigned char)*p);
        if (c == '/' && (k.empty() || k.back() == '/')) continue;
        k += c;
    }
    if (k.size() > 2 && k[1] == ':') k.erase(0, 2);
    if (!k.empty() && k[0] == '/') k.erase(0, 1);
    return k;
}

void port_editor_note_file(const char *name)            // mods.cpp, every lookup (game threads)
{
    if (!port_is_editor() || !name) return;
    size_t n = strlen(name);
    if (n >= 4 && !_stricmp(name + n - 4, ".enc")) {   // the battle being loaded: its PROPSFILE is the prop file
        std::string k = key_of(name);
        EnterCriticalSection(&g_cs);
        if (g_enc != k) { g_enc = k; g_seen_changed = true; }
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (n < 4 || _stricmp(name + n - 4, ".prp")) return;
    std::string k = key_of(name);
    EnterCriticalSection(&g_cs);
    if (std::find(g_seen.begin(), g_seen.end(), k) == g_seen.end()) {
        g_seen.push_back(k);
        g_seen_changed = true;
    }
    LeaveCriticalSection(&g_cs);
}

bool port_read_game_file(const std::string &key, std::string &out);   // bec_read.cpp
typedef void(__cdecl *MemFree)(void *);

// Ground height (game thread), asked the way the game does: floor collide mask into [0x380680], then the scene's
// HeightCheck (0x194620 -> physics_Mesh::HeightCheck 0x1D38D0) for the highest floor polygon below a point.
static struct {
    float pos[3], height;
    bool pending, ok;
    HANDLE ev;
} g_hq;

// live move request (editor thread -> service_live on the game thread, see "live props" below)
static struct {
    bool pending;
    std::string file;
    int index;
    float pos[3], rot[3];
    bool ok;
} g_live;

static void service_live();
static void service_pick();
static const UINT WM_EDITOR_PICKED = WM_APP + 1;        // picked prop: wparam = index in the loaded list
static const UINT WM_EDITOR_MOVED = WM_APP + 2;         // dragged / turned live: wparam = index (g_drag.result)
static void service_drag();
static void draw_boxes();
static void service_grid();
static char g_view_file[MAX_PATH];                      // open prop file + selected prop, for the game thread
static volatile int g_view_sel = -1;                    // (loaded-list index of the selection)
static struct {
    volatile int index = -1;                            // the prop being dragged (loaded-list index), -1 = none
    volatile bool move_pending;
    float nx, ny;
    std::string file;
    volatile int wheel, wheel_index = -1;
    volatile int raise;                                 // numpad 1 / 3 steps of 0.05 units for the selected prop
    float lift;                                         // the dragged prop's height above the ground at the press
    float result[6];                                    // its new POS + ROT for the editor window
} g_drag;


static bool ground_at(float x, float y, float z_from, float &out)   // game thread
{
    // world map (state 7): scene [0x417D64], floor mask [0x417D58]; battle (12): scene [0x39D320], floor mask [0x39C2BC]
    void *poly = nullptr;
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    int id = state ? *(const int *)(state + 8) : -1;
    void *scene = id == 7 ? *(void **)0x417D64 : id == 12 ? *(void **)0x39D320 : nullptr;
    if (!scene) return false;
    *(volatile WORD *)0x380680 = *(const WORD *)(id == 7 ? 0x417D58 : 0x39C2BC);   // floor collide mask
    float p[3] = {x, y, z_from + 1.0f};
    return ((bool(__thiscall *)(void *, const float *, float *, void **))0x194620)(scene, p, &out, &poly);
}

static void service_height()
{
    if (!g_hq.pending) return;
    float h = 0;
    g_hq.ok = ground_at(g_hq.pos[0], g_hq.pos[1], g_hq.pos[2], h);
    g_hq.height = h;
    g_hq.pending = false;
    SetEvent(g_hq.ev);
}

static bool ground_height(float x, float y, float z_from, float &out)  // editor thread
{
    g_hq.pos[0] = x, g_hq.pos[1] = y, g_hq.pos[2] = z_from;
    ResetEvent(g_hq.ev);
    g_hq.pending = true;
    if (WaitForSingleObject(g_hq.ev, 2000) != WAIT_OBJECT_0 || !g_hq.ok) return false;
    out = g_hq.height;
    return true;
}

// x / y of the God Camera; z = the ground below it when found. false = no camera.
static bool camera_ground(float pos[3], bool &ground)
{
    const uint8_t *cam = *(volatile LONG *)0x4B2814 ? *(const uint8_t *const *)0x4B2810 : nullptr;
    if (!cam) return false;
    float cx = *(const float *)(cam + 0x130), cy = *(const float *)(cam + 0x134), cz = *(const float *)(cam + 0x138);
    float gz;
    ground = ground_height(cx, cy, cz, gz);
    pos[0] = cx, pos[1] = cy;
    if (ground) pos[2] = gz;
    return true;
}

// No random battles while editing: hold the world map script's "NoRandomEncounters" int variable (hash table
// 0x38EA88, lookup 0x25870 -> node +0x0C) at 1 every frame.
static void no_random_encounters()
{
    void *node = ((void *(__thiscall *)(void *, const char *))0x25870)((void *)0x38EA88, "NoRandomEncounters");
    if (node && *(int *)((uint8_t *)node + 0xC) != 1) {
        *(int *)((uint8_t *)node + 0xC) = 1;
        port_log("editor: random encounters off (NoRandomEncounters = 1)\n");
    }
}

// a few seconds after a prop file loads: how many of its props got a model (reload diagnostics)
static volatile LONG g_check_at;                        // g_frames value to check at (0 = none)
static void check_props();

void port_editor_frame()                                // game thread, once per frame
{
    if (!port_is_editor()) return;
    InterlockedIncrement(&g_frames);
    no_random_encounters();
    check_props();
    service_height();
    service_live();
    service_pick();
    service_drag();
    service_grid();
    grid_frame();
    coll_frame();
    mesh_frame();
    draw_boxes();
    EnterCriticalSection(&g_cs);
    std::string path = g_req.done ? std::string() : g_req.path;
    LeaveCriticalSection(&g_cs);
    if (path.empty()) return;
    // read directly (loose copy, else gladius.bec): the engine's loader must not run from this hook (bec_read.cpp)
    std::string text;
    bool ok = port_read_game_file(path, text);
    EnterCriticalSection(&g_cs);
    g_req.ok = ok;
    g_req.text.swap(text);
    g_req.done = true;
    LeaveCriticalSection(&g_cs);
    SetEvent(g_req.ev);
}

static bool load_text(const std::string &path, std::string &out)   // editor thread: wait for the game thread
{
    EnterCriticalSection(&g_cs);
    g_req.path = path;
    g_req.done = false;
    ResetEvent(g_req.ev);
    LeaveCriticalSection(&g_cs);
    if (WaitForSingleObject(g_req.ev, 5000) != WAIT_OBJECT_0) return false;
    EnterCriticalSection(&g_cs);
    out = g_req.text;
    bool ok = g_req.ok;
    g_req.path.clear();
    LeaveCriticalSection(&g_cs);
    return ok;
}

// ------------------------------------------------------------------------------------------------ .prp model
struct Prop {
    std::string name, models = "No", script;
    bool lighting = true, shadow = true, has_script = false, has_hoff = false;
    float heights = 0, hoff = 0, pos[3] = {}, rot[3] = {};
    std::vector<std::string> attrs, extra;              // extra: lines this editor does not know, kept as they are
    int live = -1;                                      // its index in the engine's loaded list (-1 = added here)
};
static std::vector<std::string> g_head;                 // lines before the first PROP (VERSION ...)
static std::vector<std::string> g_tail;                 // lines after the last PROP_END (ENDFILE: the game
                                                        // stops reading there - every retail .prp ends with it)
static std::vector<Prop> g_props;
static std::string g_file;                              // "data/levels/.../x.prp"
static std::string g_new_for_enc;                       // new prop file: this encounter gets a PROPSFILE line

static std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
static std::string unquote(const std::string &v)
{
    std::string t = trim(v);
    if (t.size() >= 2 && t.front() == '"') {
        size_t e = t.find('"', 1);
        return t.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    }
    return t;
}

static void parse_prp(const std::string &text)
{
    g_head.clear();
    g_tail.clear();
    g_props.clear();
    Prop *cur = nullptr;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string line = trim(text.substr(p, e == std::string::npos ? std::string::npos : e - p));
        p = e == std::string::npos ? text.size() : e + 1;
        size_t c = line.find(':');
        std::string key = c == std::string::npos ? line : line.substr(0, c);
        std::string val = c == std::string::npos ? std::string() : line.substr(c + 1);
        if (key == "PROP") {
            g_props.emplace_back();
            g_props.back().live = (int)g_props.size() - 1;
            cur = &g_props.back();
            cur->name = unquote(val);
            continue;
        }
        if (!cur) {
            if (!line.empty()) (g_props.empty() ? g_head : g_tail).push_back(line);
            continue;
        }
        if (key == "PROP_END") { cur = nullptr; continue; }
        if (key == "MODELS") cur->models = trim(val);
        else if (key == "LIGHTING") cur->lighting = trim(val) == "ON";
        else if (key == "SHADOW") cur->shadow = trim(val) == "ON";
        else if (key == "SCRIPTNAME") { cur->has_script = true; cur->script = unquote(val); }
        else if (key == "HEIGHTS") cur->heights = (float)atof(val.c_str());
        else if (key == "HEIGHTOFFSET") { cur->has_hoff = true; cur->hoff = (float)atof(val.c_str()); }
        else if (key == "POS") sscanf(val.c_str(), "%f %f %f", &cur->pos[0], &cur->pos[1], &cur->pos[2]);
        else if (key == "ROT") sscanf(val.c_str(), "%f %f %f", &cur->rot[0], &cur->rot[1], &cur->rot[2]);
        else if (key == "ATTRIBUTES") {}
        else if (key == "ATTRIBUTE") cur->attrs.push_back(unquote(val));
        else if (!line.empty()) cur->extra.push_back(line);
    }
}

static std::string write_prp()
{
    std::string o;
    char b[256];
    for (auto &h : g_head) o += h + "\r\n";
    o += "\r\n";
    for (auto &pr : g_props) {
        o += "PROP: \"" + pr.name + "\" \r\n";
        o += "MODELS: " + pr.models + (pr.models != "No" ? " " : "") + "\r\n";
        o += std::string("LIGHTING: ") + (pr.lighting ? "ON" : "OFF") + "\r\n";
        o += std::string("SHADOW: ") + (pr.shadow ? "ON" : "OFF") + "\r\n";
        if (pr.has_script) o += "SCRIPTNAME: \"" + pr.script + "\"\r\n";
        _snprintf(b, sizeof b, "HEIGHTS: %.2f\r\n", pr.heights); o += b;
        if (pr.has_hoff) { _snprintf(b, sizeof b, "HEIGHTOFFSET: %.2f\r\n", pr.hoff); o += b; }
        for (auto &x : pr.extra) o += x + "\r\n";
        _snprintf(b, sizeof b, "POS: %.4f %.4f %.4f\r\n", pr.pos[0], pr.pos[1], pr.pos[2]); o += b;
        _snprintf(b, sizeof b, "ROT: %.4f %.4f %.4f\r\n", pr.rot[0], pr.rot[1], pr.rot[2]); o += b;
        _snprintf(b, sizeof b, "ATTRIBUTES: %u\r\n", (unsigned)pr.attrs.size()); o += b;
        for (auto &a : pr.attrs) o += "ATTRIBUTE: \"" + a + "\"\r\n";
        o += "PROP_END\r\n\r\n";
    }
    for (auto &t : g_tail) o += t + "\r\n";             // kept as the file had it (7 retail files have none)
    return o;
}

static bool save_file(const std::string &key, const std::string &text)
{
    std::string path = std::string(port_game_dir()) + "\\" + key;
    std::replace(path.begin(), path.end(), '/', (char)0x5C);
    for (size_t i = strlen(port_game_dir()) + 1; i < path.size(); i++)
        if (path[i] == 0x5C) { path[i] = 0; CreateDirectoryA(path.c_str(), nullptr); path[i] = 0x5C; }
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &n, nullptr) && n == text.size();
    CloseHandle(f);
    return ok;
}

// ------------------------------------------------------------------------------------------------ window
enum { ID_FILES = 100, ID_PROPS, ID_NAME, ID_MODEL, ID_POS, ID_ROT = ID_POS + 3, ID_HEIGHTS = ID_ROT + 3,
       ID_LIGHT, ID_SHADOW, ID_SCRIPT, ID_ATTRS, ID_APPLY, ID_DUP, ID_DEL, ID_CAM, ID_SAVE, ID_STATUS, ID_TIMER,
       ID_MODELPICK, ID_ADD, ID_GO_WORLD, ID_GO_MENU, ID_NEWFILE, ID_MESH, ID_TAB, ID_LEVEL_EDIT, ID_LEVEL_RESET };
static HWND g_wnd, g_files, g_list, g_status, g_models;
static int g_sel = -1;

static void status(const char *fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    b[sizeof b - 1] = 0;
    SetWindowTextA(g_status, b);
    port_log("editor: %s\n", b);
}

static void set_text(int id, const std::string &s) { SetDlgItemTextA(g_wnd, id, s.c_str()); }
static std::string get_text(int id)
{
    char b[512];
    GetDlgItemTextA(g_wnd, id, b, sizeof b);
    return b;
}
static void set_float(int id, float v)
{
    char b[32];
    _snprintf(b, sizeof b, "%.4f", v);
    set_text(id, b);
}

static void fill_list()
{
    SendMessageA(g_list, LB_RESETCONTENT, 0, 0);
    for (auto &p : g_props) {
        std::string m = p.models == "No" ? "(no model)" : unquote(p.models);
        size_t s = m.find_last_of("\\/");
        std::string line = p.name + "   " + (s == std::string::npos ? m : m.substr(s + 1));
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    g_sel = -1;
}

static void show_prop(int i)
{
    g_sel = i;
    g_view_sel = i >= 0 && i < (int)g_props.size() ? g_props[i].live : -1;
    if (i < 0 || i >= (int)g_props.size()) return;
    Prop &p = g_props[i];
    set_text(ID_NAME, p.name);
    set_text(ID_MODEL, p.models == "No" ? "" : unquote(p.models));
    for (int k = 0; k < 3; k++) { set_float(ID_POS + k, p.pos[k]); set_float(ID_ROT + k, p.rot[k]); }
    set_float(ID_HEIGHTS, p.heights);
    CheckDlgButton(g_wnd, ID_LIGHT, p.lighting ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_SHADOW, p.shadow ? BST_CHECKED : BST_UNCHECKED);
    set_text(ID_SCRIPT, p.has_script ? p.script : "");
    std::string a;
    for (auto &x : p.attrs) a += (a.empty() ? "" : ", ") + x;
    set_text(ID_ATTRS, a);
}

static void apply_prop()
{
    if (g_sel < 0 || g_sel >= (int)g_props.size()) { status("select a prop first"); return; }
    Prop &p = g_props[g_sel];
    p.name = get_text(ID_NAME);
    std::string m = trim(get_text(ID_MODEL));
    p.models = m.empty() ? "No" : "\"" + m + "\"";
    for (int k = 0; k < 3; k++) {
        p.pos[k] = (float)atof(get_text(ID_POS + k).c_str());
        p.rot[k] = (float)atof(get_text(ID_ROT + k).c_str());
    }
    p.heights = (float)atof(get_text(ID_HEIGHTS).c_str());
    p.lighting = IsDlgButtonChecked(g_wnd, ID_LIGHT) == BST_CHECKED;
    p.shadow = IsDlgButtonChecked(g_wnd, ID_SHADOW) == BST_CHECKED;
    std::string s = trim(get_text(ID_SCRIPT));
    p.has_script = !s.empty();
    p.script = s;
    p.attrs.clear();
    std::string a = get_text(ID_ATTRS);
    size_t q = 0;
    while (q <= a.size()) {
        size_t e = a.find(',', q);
        std::string t = trim(a.substr(q, e == std::string::npos ? std::string::npos : e - q));
        if (!t.empty()) p.attrs.push_back(t);
        if (e == std::string::npos) break;
        q = e + 1;
    }
    int keep = g_sel;
    fill_list();
    SendMessageA(g_list, LB_SETCURSEL, keep, 0);
    g_sel = keep;
    if (p.live >= 0) {                                  // the loaded prop too (game thread, next frame)
        g_live.file = g_file;
        g_live.index = p.live;
        for (int k = 0; k < 3; k++) g_live.pos[k] = p.pos[k], g_live.rot[k] = p.rot[k];
        g_live.pending = true;
        status("applied %s - moved live (Save to keep it)", p.name.c_str());
    } else {
        status("applied %s (not saved yet)", p.name.c_str());
    }
}

void port_mods_list(const char *ext, std::vector<std::string> &out);   // mods.cpp

// Models to offer: those the open file uses + every loose .pax in data\ (converted GLBs).
static void fill_models()
{
    std::vector<std::string> m;
    for (auto &p : g_props)
        if (p.models != "No") m.push_back(unquote(p.models));
    std::vector<std::string> loose;
    port_mods_list(".pax", loose);
    for (auto k : loose) {
        if (k.compare(0, 5, "data/") == 0) k.erase(0, 5);
        std::replace(k.begin(), k.end(), '/', (char)0x5C);
        m.push_back(k);
    }
    for (auto &x : m) std::transform(x.begin(), x.end(), x.begin(), ::tolower);
    std::sort(m.begin(), m.end());
    m.erase(std::unique(m.begin(), m.end()), m.end());
    char cur[MAX_PATH] = "";
    GetWindowTextA(g_models, cur, sizeof cur);
    SendMessageA(g_models, CB_RESETCONTENT, 0, 0);
    for (auto &x : m) SendMessageA(g_models, CB_ADDSTRING, 0, (LPARAM)x.c_str());
    SetWindowTextA(g_models, cur);
}

static bool camera_ground(float pos[3], bool &ground);

static void add_prop()
{
    char model[MAX_PATH] = "";
    GetWindowTextA(g_models, model, sizeof model);
    std::string m = trim(model);
    if (m.empty()) { status("pick or type a model (.pax) first"); return; }
    if (g_file.empty()) { status("no prop file open"); return; }
    size_t slash = m.find_last_of("\\/");
    std::string base = m.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4) base.erase(base.size() - 4);
    std::string name;
    for (int k = 1;; k++) {                             // unique name: <model>_<n>
        name = base + "_" + std::to_string(k);
        bool used = false;
        for (auto &p : g_props) used |= _stricmp(p.name.c_str(), name.c_str()) == 0;
        if (!used) break;
    }
    Prop p;
    p.name = name;
    p.models = "\"" + m + "\"";
    p.heights = 0.25f;
    bool ground = false;
    bool cam = camera_ground(p.pos, ground);
    g_props.push_back(p);
    fill_list();
    int i = (int)g_props.size() - 1;
    SendMessageA(g_list, LB_SETCURSEL, i, 0);
    show_prop(i);
    status("added %s %s - Save + Reload to see it", name.c_str(),
           !cam ? "at 0,0,0 (turn on the God Camera to place it there)" : ground ? "under the God Camera, on the ground"
                                                                      : "under the God Camera (no ground found)");
}

// Battles take their props from the encounter's PROPSFILE line. New prop file: <encounter>_props.prp next to the
// encounter; Save also writes a loose copy of the .enc pointing at it.
static void new_prop_file()
{
    EnterCriticalSection(&g_cs);
    std::string enc = g_enc;
    LeaveCriticalSection(&g_cs);
    if (enc.empty()) { status("no battle loaded - enter a battle first (its .enc names the prop file)"); return; }
    size_t slash = enc.find_last_of('/');
    std::string dir = enc.substr(0, slash + 1), base = enc.substr(slash + 1);
    if (base.size() > 4) base.erase(base.size() - 4);
    g_file = dir + base + "_props.prp";
    g_head.assign(1, "VERSION: 1");
    g_tail.assign(1, "ENDFILE");
    g_props.clear();
    g_new_for_enc = enc;
    fill_list();
    fill_models();
    SendMessageA(g_files, CB_SETCURSEL, (WPARAM)SendMessageA(g_files, CB_ADDSTRING, 0, (LPARAM)g_file.c_str()), 0);
    status("new %s for %s - Add props, then Save + Reload", g_file.c_str(), enc.c_str());
}

static bool patch_enc(const std::string &enc, const std::string &prp)
{
    std::string text;
    if (!load_text(enc, text)) { status("could not read %s", enc.c_str()); return false; }
    std::string rel = prp.compare(0, 5, "data/") == 0 ? prp.substr(5) : prp;
    std::replace(rel.begin(), rel.end(), '/', (char)0x5C);
    std::string want = "PROPSFILE: \t\"$d\\" + rel + "\"";
    std::vector<std::string> ls;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string l = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        ls.push_back(l);
        p = e == std::string::npos ? text.size() : e + 1;
    }
    int at = -1, after = -1;                            // header = everything before the first TEAM:
    for (int i = 0; i < (int)ls.size(); i++) {
        std::string t = trim(ls[i]);
        if (t.compare(0, 5, "TEAM:") == 0) break;
        if (t.compare(0, 10, "PROPSFILE:") == 0) at = i;
        if (t.compare(0, 9, "GRIDFILE:") == 0 || t.compare(0, 6, "SCENE:") == 0) after = i;
    }
    if (at >= 0) ls[at] = want;
    else if (after >= 0) ls.insert(ls.begin() + after + 1, want);
    else { status("%s has no SCENE / GRIDFILE header line - not changed", enc.c_str()); return false; }
    std::string out;
    for (auto &l : ls) out += l + "\r\n";
    if (!save_file(enc, out)) { status("could NOT write %s", enc.c_str()); return false; }
    port_log("editor: %s now uses %s\n", enc.c_str(), prp.c_str());
    return true;
}

static bool open_file(const std::string &key)
{
    std::string text;
    if (!load_text(key, text)) { status("could not read %s", key.c_str()); return false; }
    g_file = key;
    g_new_for_enc.clear();
    EnterCriticalSection(&g_cs);
    strncpy(g_view_file, key.c_str(), MAX_PATH - 1);
    LeaveCriticalSection(&g_cs);
    g_view_sel = -1;
    parse_prp(text);
    fill_list();
    fill_models();
    status("%s: %u props", key.c_str(), (unsigned)g_props.size());
    return true;
}

static void refresh_files()
{
    EnterCriticalSection(&g_cs);
    std::vector<std::string> seen = g_seen;
    g_seen_changed = false;
    LeaveCriticalSection(&g_cs);
    SendMessageA(g_files, CB_RESETCONTENT, 0, 0);
    for (auto &s : seen) SendMessageA(g_files, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    int cur = -1;
    for (size_t i = 0; i < seen.size(); i++) if (seen[i] == g_file) cur = (int)i;
    SendMessageA(g_files, CB_SETCURSEL, cur, 0);
    if (cur < 0 && !seen.empty() && g_file.empty()) {
        SendMessageA(g_files, CB_SETCURSEL, 0, 0);
        open_file(seen[0]);
    }
}

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id,
                           nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER: {
        EnterCriticalSection(&g_cs);
        bool changed = g_seen_changed;
        std::string follow = g_auto_open;
        LeaveCriticalSection(&g_cs);
        if (changed) refresh_files();
        static LONG frames_seen;
        bool running = g_frames != frames_seen;         // not inside a loading screen: the file read gets serviced
        frames_seen = g_frames;
        if (!follow.empty() && running) {               // a level loaded its prop file: open it (retried until read)
            // re-read even when it is the open file: a reload builds the props again from the saved file
            if (open_file(follow)) {
                EnterCriticalSection(&g_cs);
                if (g_auto_open == follow) g_auto_open.clear();
                LeaveCriticalSection(&g_cs);
                refresh_files();
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_FILES && code == CBN_SELCHANGE) {
            char b[MAX_PATH];
            int i = (int)SendMessageA(g_files, CB_GETCURSEL, 0, 0);
            if (i >= 0 && SendMessageA(g_files, CB_GETLBTEXT, i, (LPARAM)b) > 0) open_file(b);
        } else if (id == ID_PROPS && code == LBN_SELCHANGE) {
            show_prop((int)SendMessageA(g_list, LB_GETCURSEL, 0, 0));
        } else if (id == ID_APPLY) {
            apply_prop();
        } else if (id == ID_DUP && g_sel >= 0) {
            Prop p = g_props[g_sel];
            p.name += "_copy";
            p.live = -1;                                // not loaded yet: shows after Save + Reload
            g_props.insert(g_props.begin() + g_sel + 1, p);
            int keep = g_sel + 1;
            fill_list();
            SendMessageA(g_list, LB_SETCURSEL, keep, 0);
            show_prop(keep);
            status("duplicated %s (not saved yet)", p.name.c_str());
        } else if (id == ID_DEL && g_sel >= 0) {
            std::string n = g_props[g_sel].name;
            g_props.erase(g_props.begin() + g_sel);
            fill_list();
            status("deleted %s (not saved yet)", n.c_str());
        } else if (id == ID_CAM) {
            // the God Camera's position (+0x130), or the game's camera when it is off
            if (g_sel < 0) { status("select a prop first"); break; }
            float pos[3] = {g_props[g_sel].pos[0], g_props[g_sel].pos[1], g_props[g_sel].pos[2]};
            bool ground = false;
            if (!camera_ground(pos, ground)) { status("turn on the God Camera (Ctrl+G) first"); break; }
            for (int k = 0; k < 3; k++) set_float(ID_POS + k, pos[k]);
            apply_prop();
            if (ground) status("%s moved under the God Camera, onto the ground (z %.2f)", g_props[g_sel].name.c_str(), pos[2]);
            else status("%s moved under the God Camera - no ground found there, height kept", g_props[g_sel].name.c_str());
        } else if (id == ID_GO_WORLD || id == ID_GO_MENU) {
            // another level: the file list starts over
            EnterCriticalSection(&g_cs);
            g_seen.clear();
            g_seen_changed = true;
            LeaveCriticalSection(&g_cs);
            g_file.clear();
            g_props.clear();
            fill_list();
            port_mods_rescan();
            *(volatile int *)0x38E43C = id == ID_GO_WORLD ? 7 : 21;
            status(id == ID_GO_WORLD ? "going to the world map" : "going to the main menu (Versus -> an arena to edit it)");
        } else if (id == ID_MESH) {
            if (g_sel < 0 || g_sel >= (int)g_props.size()) { status("select a prop first"); break; }
            const Prop &p = g_props[g_sel];
            if (p.live < 0) { status("%s is not loaded yet - Save + Reload first", p.name.c_str()); break; }
            if (p.models == "No") { status("%s has no model", p.name.c_str()); break; }
            mesh_edit_prop(g_file, p.live, unquote(p.models), p.name);
            int others = 0;
            for (auto &q : g_props) others += &q != &p && !_stricmp(q.models.c_str(), p.models.c_str());
            if (others)
                status("%d other prop(s) here use this model: they show the edits too until Save, which gives %s its own copy",
                       others, p.name.c_str());
        } else if (id == ID_LEVEL_EDIT) {
            mesh_edit_level();
        } else if (id == ID_LEVEL_RESET) {
            mesh_reset_level();
        } else if (id == ID_NEWFILE) {
            new_prop_file();
        } else if (id == ID_ADD) {
            add_prop();
        } else if (id == ID_SAVE) {
            if (g_file.empty()) { status("no prop file open"); break; }
            if (g_sel >= 0) apply_prop();               // fields not applied yet count too
            if (!g_new_for_enc.empty()) {               // a new prop file: the encounter has to name it
                if (!patch_enc(g_new_for_enc, g_file)) break;
                g_new_for_enc.clear();
            }
            if (!save_file(g_file, write_prp())) { status("could NOT write %s", g_file.c_str()); break; }
            const uint8_t *st = *(const uint8_t *const *)(0x38E448 + 0x14);
            if (st && *(const int *)(st + 8) == 12)
                status("saved %s - leave the battle and enter it again to see it", g_file.c_str());
            else
                status("saved %s - reloading", g_file.c_str());
            port_editor_reload();
        }
        return 0;
    }
    case WM_EDITOR_MOVED:
        for (int i = 0; i < (int)g_props.size(); i++)
            if (g_props[i].live == (int)wp) {
                for (int k = 0; k < 3; k++) g_props[i].pos[k] = g_drag.result[k], g_props[i].rot[k] = g_drag.result[3 + k];
                if (i == g_sel)
                    for (int k = 0; k < 3; k++) set_float(ID_POS + k, g_props[i].pos[k]), set_float(ID_ROT + k, g_props[i].rot[k]);
                status("%s at %.2f %.2f %.2f, turned %.0f deg - Save to keep it", g_props[i].name.c_str(),
                       g_props[i].pos[0], g_props[i].pos[1], g_props[i].pos[2], g_props[i].rot[2] * 57.29578f);
                break;
            }
        return 0;
    case WM_EDITOR_PICKED:
        for (int i = 0; i < (int)g_props.size(); i++)
            if (g_props[i].live == (int)wp) {
                SendMessageA(g_list, LB_SETCURSEL, i, 0);
                show_prop(i);
                status("picked %s", g_props[i].name.c_str());
                break;
            }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);                         // the game keeps running; Ctrl+E shows it again
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ the window
// The "Gladius Editor" window: Level / Character / Prop pages on the left, mesh tools (mesh_edit.cpp) on the right.
// Topmost tool window, so it stays in front while the game window has focus.
static HWND g_frame, g_tab, g_pages[3];
enum { PAGE_LEVEL, PAGE_CHARACTER, PAGE_PROP };
static const int PAGE_W = 410, MESH_W = 380, FRAME_H = 960;

static void show_page(int k)
{
    for (int i = 0; i < 3; i++) ShowWindow(g_pages[i], i == k ? SW_SHOW : SW_HIDE);
    TabCtrl_SetCurSel(g_tab, k);
}

static LRESULT CALLBACK page_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)   // Level / Character pages
{
    if (msg == WM_COMMAND && grid_command(LOWORD(wp), HIWORD(wp))) return 0;          // the grid painter's
    if (msg == WM_COMMAND && coll_command(LOWORD(wp), HIWORD(wp))) return 0;          // the collision painter's
    if (msg == WM_COMMAND && help_command(LOWORD(wp), HIWORD(wp))) return 0;          // the guide's buttons
    if (msg == WM_COMMAND && g_wnd) return SendMessageA(g_wnd, WM_COMMAND, wp, lp);   // handled with the props
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK frame_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY:
        if (((NMHDR *)lp)->hwndFrom == g_tab && ((NMHDR *)lp)->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(g_tab));
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);                         // the game keeps running; Ctrl+E shows it again
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static HWND page_add(HWND page, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, page, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static DWORD WINAPI editor_thread(void *)
{
    HINSTANCE inst = GetModuleHandleA(nullptr);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSA wc = {};
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = frame_wndproc;
    wc.lpszClassName = "GladiusEditorFrame";
    RegisterClassA(&wc);
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = "GladiusEditorPanel";
    RegisterClassA(&wc);
    wc.lpfnWndProc = page_wndproc;
    wc.lpszClassName = "GladiusEditorPage";
    RegisterClassA(&wc);
    RECT r = {0, 0, PAGE_W + MESH_W + 16, FRAME_H};
    DWORD fs = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    AdjustWindowRectEx(&r, fs, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    g_frame = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, "GladiusEditorFrame", "Gladius Editor",
                              fs, 40, 20, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    g_tab = CreateWindowA(WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 6, 6, PAGE_W, 24, g_frame,
                          (HMENU)(INT_PTR)ID_TAB, inst, nullptr);
    SendMessageA(g_tab, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    const char *names[3] = {"Level", "Character", "Prop"};
    for (int i = 0; i < 3; i++) {
        TCITEMA it = {};
        it.mask = TCIF_TEXT;
        it.pszText = (char *)names[i];
        TabCtrl_InsertItem(g_tab, i, &it);
    }
    for (int i = 0; i < 3; i++)
        g_pages[i] = CreateWindowExA(WS_EX_CONTROLPARENT, i == PAGE_PROP ? "GladiusEditorPanel" : "GladiusEditorPage", "",
                                     WS_CHILD | WS_CLIPCHILDREN, 0, 34, PAGE_W + 12, FRAME_H - 34, g_frame, nullptr, inst, nullptr);
    g_wnd = g_pages[PAGE_PROP];

    // Level page
    HWND lp = g_pages[PAGE_LEVEL];
    int ly = 10;
    page_add(lp, "STATIC", "Go to:", 0, 10, ly + 4, 40, 16, -1);
    page_add(lp, "BUTTON", "World map", BS_PUSHBUTTON, 54, ly, 120, 24, ID_GO_WORLD);
    page_add(lp, "BUTTON", "Main menu (arenas)", BS_PUSHBUTTON, 180, ly, 140, 24, ID_GO_MENU);
    ly += 40;
    page_add(lp, "BUTTON", "Edit this level's geometry", BS_PUSHBUTTON, 10, ly, 390, 28, ID_LEVEL_EDIT);
    ly += 34;
    page_add(lp, "BUTTON", "Reset this level to the original (edited copy moved to editor_backups)", BS_PUSHBUTTON, 10, ly,
             390, 24, ID_LEVEL_RESET);
    ly += 34;
    page_add(lp, "STATIC",
             "Opens the scene's level model (.pao) in the mesh tools on the right: the world map or the battle arena you "
             "are in. Grab sub-mesh moves whole pieces; Save writes the level's loose .pao with its culling tree fitted "
             "around the edited geometry.", 0, 10, ly, 390, 60, -1);
    ly += 64;
    ly = grid_create_ui(lp, ly);
    ly = coll_create_ui(lp, ly + 6);
    help_create_guide(lp, ly + 6, FRAME_H - 34 - 48);

    // Character page
    page_add(g_pages[PAGE_CHARACTER], "STATIC",
             "Character mesh editing (skinned gladiators: edits in the bind pose through the bones, palettes and LOD "
             "masks kept valid) comes next.", 0, 10, 10, 390, 60, -1);

    int y = 10;
    add("BUTTON", "New prop file", BS_PUSHBUTTON, 282, y, 118, 24, ID_NEWFILE);
    y += 32;
    add("STATIC", "Prop file (loaded by this level):", 0, 10, y, 390, 16, -1); y += 18;
    g_files = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 10, y, 390, 300, ID_FILES); y += 30;
    g_list = add("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, y, 390, 230, ID_PROPS); y += 236;
    const char *lab[] = {"Name", "Model (.pax)"};
    for (int i = 0; i < 2; i++) {
        add("STATIC", lab[i], 0, 10, y + 3, 80, 16, -1);
        add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 90, y, 310, 20, i ? ID_MODEL : ID_NAME);
        y += 24;
    }
    const char *xyz[] = {"Position", "Rotation"};
    for (int r = 0; r < 2; r++) {
        add("STATIC", xyz[r], 0, 10, y + 3, 80, 16, -1);
        for (int k = 0; k < 3; k++)
            add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 90 + k * 104, y, 100, 20, (r ? ID_ROT : ID_POS) + k);
        y += 24;
    }
    add("STATIC", "Heights", 0, 10, y + 3, 80, 16, -1);
    add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 90, y, 100, 20, ID_HEIGHTS);
    add("BUTTON", "Lighting", BS_AUTOCHECKBOX, 200, y, 90, 20, ID_LIGHT);
    add("BUTTON", "Shadow", BS_AUTOCHECKBOX, 300, y, 90, 20, ID_SHADOW);
    y += 24;
    add("STATIC", "Script", 0, 10, y + 3, 80, 16, -1);
    add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 90, y, 310, 20, ID_SCRIPT);
    y += 24;
    add("STATIC", "Attributes", 0, 10, y + 3, 80, 16, -1);
    add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 90, y, 310, 20, ID_ATTRS);
    y += 30;
    add("STATIC", "New prop", 0, 10, y + 3, 80, 16, -1);
    g_models = add("COMBOBOX", "", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 90, y, 232, 300, ID_MODELPICK);
    add("BUTTON", "Add", BS_PUSHBUTTON, 328, y - 1, 72, 24, ID_ADD);
    y += 32;
    add("BUTTON", "Apply", BS_PUSHBUTTON, 10, y, 72, 26, ID_APPLY);
    add("BUTTON", "Duplicate", BS_PUSHBUTTON, 88, y, 72, 26, ID_DUP);
    add("BUTTON", "Delete", BS_PUSHBUTTON, 166, y, 72, 26, ID_DEL);
    add("BUTTON", "To camera", BS_PUSHBUTTON, 244, y, 72, 26, ID_CAM);
    add("BUTTON", "Save + Reload", BS_DEFPUSHBUTTON, 322, y, 78, 26, ID_SAVE);
    y += 32;
    add("BUTTON", "Edit this prop's mesh...", BS_PUSHBUTTON, 10, y, 390, 24, ID_MESH);
    y += 32;
    g_status = add("STATIC", "Waiting for the level's prop files...", SS_LEFTNOWORDWRAP, 10, y, 390, 16, ID_STATUS);
    SetTimer(g_wnd, ID_TIMER, 500, nullptr);
    mesh_create_window(inst, g_frame, PAGE_W + 16, 0, MESH_W, FRAME_H);
    help_apply_tips(g_frame);
    show_page(PAGE_PROP);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (mesh_window_message(&m)) continue;
        if (!IsDialogMessageA(g_frame, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ live props
// Live props: the engine's .prp loader (0x1E1ED0) appends each prop (0x84 bytes, ctor 0x1E11A0) to the file object's
// list (head +0x14). Prop: +0x08 -> 4x4 matrix, +0x0C flags (1 lighting off, 2 shadow off), +0x20 POS, +0x2C quaternion,
// +0x3C HEIGHTOFFSET, +0x54/+0x56 grid cell, +0x64 ROT, +0x70 HEIGHTS, +0x74 z. A live move repeats the loader's own
// steps on that prop. The file object (vtable 0x358A38) is tracked until its destructor (0x1E2680) runs.
struct PropFile { void *obj; std::string key; };
static std::vector<PropFile> g_pfiles;                  // game thread writes, editor reads under g_cs

static void __cdecl forget_propfile(void *obj)          // game thread: 0x1E2680 entry
{
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_pfiles.size(); i++)
        if (g_pfiles[i].obj == obj) {
            port_log("editor: prop file object %p (%s) destroyed\n", obj, g_pfiles[i].key.c_str());
            g_pfiles.erase(g_pfiles.begin() + i);
            break;
        }
    LeaveCriticalSection(&g_cs);
}

static __declspec(naked) void prp_dtor_hook()
{
    __asm {
        push ecx
        push ecx                        // prop file object
        call forget_propfile
        add esp, 4
        pop ecx
        push -1                         // original 0x1E2680: push -1; push 0x298BE8
        push 0x298BE8
        push 0x1E2687
        ret
    }
}

static void __cdecl note_propfile(void *obj, const char *name)
{
    std::string k = name ? name : "";
    if (k.size() > 2 && k[0] == '$' && (k[1] == 'd' || k[1] == 'D')) k = "data" + k.substr(2);
    k = key_of(k.c_str());
    EnterCriticalSection(&g_cs);
    for (auto &f : g_pfiles)
        if (f.obj == obj || f.key == k) {
            f = {obj, k};
            LeaveCriticalSection(&g_cs);
            goto logged;
        }
    g_pfiles.push_back({obj, k});
    LeaveCriticalSection(&g_cs);
logged:
    EnterCriticalSection(&g_cs);
    g_auto_open = k;                                    // the window follows the level
    LeaveCriticalSection(&g_cs);
    g_check_at = g_frames + 300;                        // ~5 s of frames after the load
    port_log("editor: prop file object %p = %s\n", obj, k.c_str());
}

static __declspec(naked) void prp_load_hook()
{
    __asm {
        push ecx
        push dword ptr [esp + 8]        // file name (arg 1)
        push ecx                        // prop file object
        call note_propfile
        add esp, 8
        pop ecx
        push -1                         // original 0x1E1ED0: push -1; push 0x298BB1
        push 0x298BB1
        push 0x1E1ED7
        ret
    }
}

static uint8_t *live_prop(const std::string &file, int index)   // game thread
{
    void *obj = nullptr;
    EnterCriticalSection(&g_cs);
    for (auto &f : g_pfiles) if (f.key == file) obj = f.obj;
    LeaveCriticalSection(&g_cs);
    if (!obj || index < 0) return nullptr;
    uint8_t *p = *(uint8_t **)((uint8_t *)obj + 0x14);
    for (int i = 0; p && i < index; i++) p = *(uint8_t **)p;
    return p;
}

static void check_props()                               // game thread
{
    if (!g_check_at || g_frames < g_check_at) return;
    g_check_at = 0;
    EnterCriticalSection(&g_cs);
    for (auto &f : g_pfiles) {
        int n = 0, statics = 0, inst = 0, model = 0, other = 0;
        for (const uint8_t *p = *(const uint8_t *const *)((const uint8_t *)f.obj + 0x14); p; p = *(const uint8_t *const *)p) {
            n++;
            short type = *(const short *)(p + 0x10);
            if (type != 1) { other++; continue; }
            statics++;
            const uint8_t *in = *(const uint8_t *const *)(p + 0x7C);
            if (!in) continue;
            inst++;
            if (*(const uint8_t *const *)(in + 0x18)) model++;
        }
        port_log("editor: check %s: %d props - %d static (%d with an instance, %d with a model), %d other types\n",
                 f.key.c_str(), n, statics, inst, model, other);
        int i = 0;                                      // every static prop in detail (compare a good / bad load)
        for (const uint8_t *p = *(const uint8_t *const *)((const uint8_t *)f.obj + 0x14); p; p = *(const uint8_t *const *)p, i++) {
            if (*(const short *)(p + 0x10) != 1) continue;
            const float *pos = (const float *)(p + 0x20), *M = *(const float *const *)(p + 8);
            const uint8_t *in = *(const uint8_t *const *)(p + 0x7C);
            const uint8_t *mo = in ? *(const uint8_t *const *)(in + 0x18) : nullptr;
            const uint8_t *pax = mo ? *(const uint8_t *const *)(mo + 0x1C) : nullptr;
            const char *nm = pax ? *(const char *const *)(pax + 0xC) : nullptr;
            const uint32_t *iw = (const uint32_t *)in;
            port_log("editor:   #%d flags %X pos (%.2f %.2f %.2f) M %p t (%.2f %.2f %.2f) inst %p [%08X %08X %08X %08X %08X "
                     "%08X %08X %08X] model %p r %.2f c (%.2f %.2f %.2f) box (%.2f %.2f %.2f / %.2f %.2f %.2f) geom %p %s\n",
                     i, *(const uint32_t *)(p + 0xC), pos[0], pos[1], pos[2], M, M ? M[12] : 0.f, M ? M[13] : 0.f,
                     M ? M[14] : 0.f, in, iw ? iw[0] : 0, iw ? iw[1] : 0, iw ? iw[2] : 0, iw ? iw[3] : 0, iw ? iw[4] : 0,
                     iw ? iw[5] : 0, iw ? iw[6] : 0, iw ? iw[7] : 0, mo, mo ? *(const float *)(mo + 0x4C) : 0.f,
                     mo ? *(const float *)(mo + 0x40) : 0.f, mo ? *(const float *)(mo + 0x44) : 0.f,
                     mo ? *(const float *)(mo + 0x48) : 0.f, mo ? *(const float *)(mo + 0x5C) : 0.f,
                     mo ? *(const float *)(mo + 0x60) : 0.f, mo ? *(const float *)(mo + 0x64) : 0.f,
                     mo ? *(const float *)(mo + 0x68) : 0.f, mo ? *(const float *)(mo + 0x6C) : 0.f,
                     mo ? *(const float *)(mo + 0x70) : 0.f, mo ? *(void *const *)(mo + 0x28) : nullptr,
                     (uintptr_t)nm > 0x10000 ? nm : "?");
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void log_matrix(const char *when, const uint8_t *prop)
{
    const float *m = *(const float *const *)(prop + 8);
    if (!m) return;
    port_log("editor:   %s matrix row3 (%.3f %.3f %.3f)  pos (%.3f %.3f %.3f)  quat (%.3f %.3f %.3f %.3f)\n", when,
             m[12], m[13], m[14], *(const float *)(prop + 0x20), *(const float *)(prop + 0x24),
             *(const float *)(prop + 0x28), *(const float *)(prop + 0x2C), *(const float *)(prop + 0x30),
             *(const float *)(prop + 0x34), *(const float *)(prop + 0x38));
}

static void service_live()                              // game thread
{
    if (!g_live.pending) return;
    g_live.pending = false;
    uint8_t *p = live_prop(g_live.file, g_live.index);
    g_live.ok = p != nullptr;
    if (!p) { port_log("editor: live move - no loaded prop %d in %s\n", g_live.index, g_live.file.c_str()); return; }
    log_matrix("before", p);
    for (int k = 0; k < 3; k++) {
        *(float *)(p + 0x20 + 4 * k) = g_live.pos[k];
        *(float *)(p + 0x64 + 4 * k) = g_live.rot[k];
    }
    ((void(__fastcall *)(void *))0x1E02F0)(p);          // grid cell from x/y
    ((void(__fastcall *)(void *))0x1E07F0)(p);          // orientation from ROT
    *(float *)(p + 0x74) = g_live.pos[2];
    *(float *)(p + 0x78) = *(float *)(p + 0x70) + g_live.pos[2];
    log_matrix("after ", p);
}

// ------------------------------------------------------------------------------------------------ picking
// Picking: left click in the game window casts a ray from the scene camera; the open file's prop closest to it (in
// front, within ~1 unit + 3% of the distance) is selected.
static struct {
    volatile bool pending;
    float nx, ny;                                       // click in -1..1 (y up)
    std::string file;
} g_pick;

// The 3D view's camera: the scene camera at 0x4B280C (cam_activecam 0x4B2808 is the 2D camera by our frame hook).
static const uint8_t *view_camera()
{
    const uint8_t *cam = *(const uint8_t *const *)0x4B280C;
    return cam ? cam : *(const uint8_t *const *)0x4B2808;
}

// Ray from the scene camera through a screen point (-1..1, y up): origin o, unit direction d.
static bool camera_ray(float nx, float ny, float o[3], float d[3])
{
    const uint8_t *cam = view_camera();
    if (!cam) return false;
    const float *c = (const float *)(cam + 0x130), *rt = (const float *)(cam + 0x140),
                *up = (const float *)(cam + 0x150), *bk = (const float *)(cam + 0x160);
    float nr = *(const float *)(cam + 0x274), hw = *(const float *)(cam + 0x27C), hh = *(const float *)(cam + 0x280);
    float len = 0;
    for (int k = 0; k < 3; k++) {
        o[k] = c[k];
        d[k] = -bk[k] * nr + rt[k] * nx * hw + up[k] * ny * hh;
        len += d[k] * d[k];
    }
    len = sqrtf(len);
    if (len <= 0) return false;
    for (int k = 0; k < 3; k++) d[k] /= len;
    return true;
}

// A static prop's box: model instance +0x7C -> model +0x18, vertex min / max at +0x5C / +0x68; world matrix at +0x08
// (row vectors), rebuilt by the engine every frame.
static bool prop_box(const uint8_t *p, float lo[3], float hi[3], const float **M)
{
    if (*(const short *)(p + 0x10) != 1) return false;
    const uint8_t *inst = *(const uint8_t *const *)(p + 0x7C);
    const uint8_t *model = inst ? *(const uint8_t *const *)(inst + 0x18) : nullptr;
    *M = *(const float *const *)(p + 8);
    if (!model || !*M) return false;
    memcpy(lo, model + 0x5C, 12);
    memcpy(hi, model + 0x68, 12);
    for (int k = 0; k < 3; k++)
        if (!(lo[k] <= hi[k]) || hi[k] - lo[k] > 2000.0f) return false;
    return true;
}

static void to_world(const float *M, const float l[3], float w[3])
{
    for (int k = 0; k < 3; k++) w[k] = l[0] * M[k] + l[1] * M[4 + k] + l[2] * M[8 + k] + M[12 + k];
}

// Camera space of the active camera: x right, y up, depth forward (the engine looks down -z).
static bool cam_frame(CamFrame &f)
{
    const uint8_t *cam = view_camera();
    if (!cam) return false;
    memcpy(f.c, cam + 0x130, 12); memcpy(f.rt, cam + 0x140, 12); memcpy(f.up, cam + 0x150, 12); memcpy(f.bk, cam + 0x160, 12);
    f.n = *(const float *)(cam + 0x274); f.hw = *(const float *)(cam + 0x27C); f.hh = *(const float *)(cam + 0x280);
    return f.n > 0 && f.hw > 0 && f.hh > 0;
}
static void to_cam(const CamFrame &f, const float w[3], float v[3])
{
    float d[3] = {w[0] - f.c[0], w[1] - f.c[1], w[2] - f.c[2]};
    v[0] = d[0] * f.rt[0] + d[1] * f.rt[1] + d[2] * f.rt[2];
    v[1] = d[0] * f.up[0] + d[1] * f.up[1] + d[2] * f.up[2];
    v[2] = -(d[0] * f.bk[0] + d[1] * f.bk[1] + d[2] * f.bk[2]);
}
static void cam_line(const CamFrame &f, float a[3], float b[3], float thick, uint32_t argb)   // overlay 640x480
{
    if (a[2] < f.n && b[2] < f.n) return;
    float A[3], B[3];
    memcpy(A, a, 12); memcpy(B, b, 12);
    if (A[2] < f.n) { float t = (f.n - A[2]) / (B[2] - A[2]); for (int k = 0; k < 3; k++) A[k] += (B[k] - A[k]) * t; }
    if (B[2] < f.n) { float t = (f.n - B[2]) / (A[2] - B[2]); for (int k = 0; k < 3; k++) B[k] += (A[k] - B[k]) * t; }
    auto sx = [&](const float *v) { return 320.0f + 320.0f * (v[0] / v[2]) * f.n / f.hw; };
    auto sy = [&](const float *v) { return 240.0f - 240.0f * (v[1] / v[2]) * f.n / f.hh; };
    ov_line(sx(A), sy(A), sx(B), sy(B), thick, argb);
}

bool ed_cam_frame(CamFrame &f) { return cam_frame(f); }
void ed_to_cam(const CamFrame &f, const float w[3], float v[3]) { to_cam(f, w, v); }
void ed_cam_line(const CamFrame &f, float a[3], float b[3], float thick, uint32_t argb) { cam_line(f, a, b, thick, argb); }
uint8_t *ed_live_prop(const std::string &file, int index) { return live_prop(file, index); }
bool ed_save_file(const std::string &key, const std::string &data) { return save_file(key, data); }
bool ed_ground_at(float x, float y, float z_from, float &out) { return ground_at(x, y, z_from, out); }
std::string ed_current_enc()
{
    EnterCriticalSection(&g_cs);
    std::string e = g_enc;
    LeaveCriticalSection(&g_cs);
    return e;
}

// Editor thread (mesh Save): point the prop at its own copy of the model, save the .prp, reload.
bool ed_set_prop_model(const std::string &file, int live, const std::string &model)
{
    if (file != g_file) return false;
    for (auto &p : g_props)
        if (p.live == live) {
            p.models = "\"" + model + "\"";
            if (!save_file(g_file, write_prp())) return false;
            int keep = g_sel;
            fill_list();
            if (keep >= 0) { SendMessageA(g_list, LB_SETCURSEL, keep, 0); show_prop(keep); }
            port_editor_reload();                       // world map: reloads now; battle: on re-entering it
            return true;
        }
    return false;
}

bool ed_prop_alive(const uint8_t *prop)                 // game thread
{
    EnterCriticalSection(&g_cs);
    bool found = false;
    for (auto &f : g_pfiles)
        for (const uint8_t *p = *(const uint8_t *const *)((const uint8_t *)f.obj + 0x14); p && !found;
             p = *(const uint8_t *const *)p)
            found = p == prop;
    LeaveCriticalSection(&g_cs);
    return found;
}

// Every frame: wire boxes around the open file's static props (dim cyan within 80 units, the selection yellow).
static void draw_boxes()
{
    char file[MAX_PATH];
    EnterCriticalSection(&g_cs);
    memcpy(file, g_view_file, MAX_PATH);
    void *obj = nullptr;
    for (auto &f : g_pfiles) if (file[0] && f.key == file) obj = f.obj;
    LeaveCriticalSection(&g_cs);
    static bool shown;
    CamFrame cf;
    bool mesh = mesh_active();
    if (!cam_frame(cf)) {
        if (shown) { ov_begin(); ov_end(false); shown = false; }
        return;
    }
    ov_begin();
    // camera readout (bottom left): position, heading, pitch, God Camera state
    {
        float yaw = atan2f(-cf.bk[1], -cf.bk[0]) * 57.29578f, pitch = asinf(fmaxf(-1.0f, fminf(1.0f, -cf.bk[2]))) * 57.29578f;
        char b[160];
        _snprintf(b, sizeof b, "camera  x %.2f  y %.2f  z %.2f   heading %.0f   pitch %.0f%s", cf.c[0], cf.c[1], cf.c[2],
                  yaw, pitch, *(volatile LONG *)0x4B2814 ? "   (God Camera)" : "");
        b[sizeof b - 1] = 0;
        ov_text(b, 12, 466, OV_LEFT, 0xE0FFFFFF, true);
    }
    grid_draw(cf);                                      // the battle grid painter (when on)
    coll_draw(cf);                                      // the collision painter (when on)
    if (mesh) {                                         // editing a mesh: its wireframe / selection instead
        mesh_draw(cf);
        ov_end(true);
        shown = true;
        return;
    }
    int i = 0;
    for (const uint8_t *p = obj ? *(const uint8_t *const *)((const uint8_t *)obj + 0x14) : nullptr; p;
         p = *(const uint8_t *const *)p, i++) {
        float lo[3], hi[3];
        const float *M;
        if (!prop_box(p, lo, hi, &M)) continue;
        bool sel = i == g_view_sel;
        float ctr[3], cv[3];
        for (int k = 0; k < 3; k++) ctr[k] = (lo[k] + hi[k]) / 2;
        float wc[3];
        to_world(M, ctr, wc);
        to_cam(cf, wc, cv);
        if (!sel && (cv[2] < 0 || cv[2] > 80.0f)) continue;
        float v[8][3];
        for (int c = 0; c < 8; c++) {
            float l[3] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]}, w[3];
            to_world(M, l, w);
            to_cam(cf, w, v[c]);
        }
        static const int E[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        for (auto &e : E)
            cam_line(cf, v[e[0]], v[e[1]], sel ? 1.5f : 0.75f, sel ? 0xFFFFD040 : 0x7040D0FF);
    }
    ov_end(true);
    shown = true;
}

// Ray (world) against a prop's box in its own space: -1 = miss, else the distance along the ray.
static float ray_box(const float o[3], const float d[3], const uint8_t *p)
{
    float lo[3], hi[3];
    const float *M;
    if (!prop_box(p, lo, hi, &M)) return -1;
    float lo_[3], ld[3];
    for (int k = 0; k < 3; k++) {                       // l_k = (w - T) . row_k / |row_k|^2
        const float *row = M + 4 * k;
        float rr = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        if (rr <= 0) return -1;
        lo_[k] = ((o[0] - M[12]) * row[0] + (o[1] - M[13]) * row[1] + (o[2] - M[14]) * row[2]) / rr;
        ld[k] = (d[0] * row[0] + d[1] * row[1] + d[2] * row[2]) / rr;
    }
    float t0 = 0, t1 = 1e30f;
    for (int k = 0; k < 3; k++) {
        if (fabsf(ld[k]) < 1e-9f) { if (lo_[k] < lo[k] || lo_[k] > hi[k]) return -1; continue; }
        float a = (lo[k] - lo_[k]) / ld[k], b = (hi[k] - lo_[k]) / ld[k];
        if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return -1;
    }
    return t0;
}

static void service_pick()                              // game thread
{
    if (!g_pick.pending) return;
    g_pick.pending = false;
    const uint8_t *cam = view_camera();
    if (!cam) return;
    const float *o = (const float *)(cam + 0x130), *rt = (const float *)(cam + 0x140),
                *up = (const float *)(cam + 0x150), *bk = (const float *)(cam + 0x160);
    float nr = *(const float *)(cam + 0x274), hw = *(const float *)(cam + 0x27C), hh = *(const float *)(cam + 0x280);
    float d[3], len = 0;
    for (int k = 0; k < 3; k++) {
        d[k] = -bk[k] * nr + rt[k] * g_pick.nx * hw + up[k] * g_pick.ny * hh;
        len += d[k] * d[k];
    }
    len = sqrtf(len);
    if (len <= 0) return;
    for (int k = 0; k < 3; k++) d[k] /= len;
    void *obj = nullptr;
    EnterCriticalSection(&g_cs);
    for (auto &f : g_pfiles) if (f.key == g_pick.file) obj = f.obj;
    LeaveCriticalSection(&g_cs);
    if (!obj) { port_log("editor: pick - the open prop file is not loaded in this level\n"); return; }
    int best = -1, i = 0, near_i = -1, box_i = -1;
    float box_t = 1e30f;
    float best_d = 1e30f, near_d = 1e30f, near_t = 0;
    for (uint8_t *p = *(uint8_t **)((uint8_t *)obj + 0x14); p; p = *(uint8_t **)p, i++) {
        float v[3], t = 0, dist = 0;
        for (int k = 0; k < 3; k++) { v[k] = *(const float *)(p + 0x20 + 4 * k) - o[k]; t += v[k] * d[k]; }
        if (t <= 0) continue;
        for (int k = 0; k < 3; k++) { float e = v[k] - d[k] * t; dist += e * e; }
        dist = sqrtf(dist);
        if (dist < near_d) { near_d = dist; near_i = i; near_t = t; }
        if (dist < 1.0f + 0.03f * t && dist < best_d) { best_d = dist; best = i; }
        float tb = ray_box(o, d, p);                    // boxes (static models) win over the base-point guess
        if (tb >= 0 && tb < box_t) { box_t = tb; box_i = i; }
    }
    if (box_i >= 0) best = box_i;
    port_log("editor: pick (%.2f %.2f) cam %p (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) near %.3f half %.3f x %.3f, "
             "%d props: closest %d at %.2f from the ray, %.2f ahead -> picked %d\n", g_pick.nx, g_pick.ny, cam, o[0], o[1],
             o[2], d[0], d[1], d[2], nr, hw, hh, i, near_i, near_d, near_t, best);
    g_drag.index = best;                                // a press on a prop starts a drag (until release)
    g_drag.lift = 0;
    if (best >= 0)
        if (uint8_t *bp = live_prop(g_pick.file, best)) {
            float gz, *bpos = (float *)(bp + 0x20);
            if (ground_at(bpos[0], bpos[1], bpos[2] + 0.5f, gz) && bpos[2] - gz > 0.01f) g_drag.lift = bpos[2] - gz;
        }
    if (best >= 0 && g_wnd) PostMessageA(g_wnd, WM_EDITOR_PICKED, best, 0);
}

// Drag: the held prop follows the cursor on the horizontal plane at its height, snapped onto the floor, live. Mouse
// wheel turns it 15 degrees per notch. In a battle each prop also has a height feature in the list at 0x3EE8A8 (read
// live), but the grid heights (0xA23C0) and pathfinder step heights (0xA2660) are sampled once at battle init, so they
// are rebuilt after props move. No_Cursor / No_Stand grid flags wait for the next reload.
static volatile bool g_grid_dirty;
static DWORD g_grid_moved;

static bool in_battle_file(const uint8_t *p)
{
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    const uint8_t *file = *(const uint8_t *const *)0x39D344;
    if (!state || *(const int *)(state + 8) != 12 || !file) return false;
    for (const uint8_t *q = *(const uint8_t *const *)(file + 0x14); q; q = *(const uint8_t *const *)q)
        if (q == p) return true;
    return false;
}

static void move_height_feature(int ox, int oy, float otop, const uint8_t *p)
{
    for (uint8_t *e = *(uint8_t **)0x3EE8A8; e; e = *(uint8_t **)e)
        if (e[8] == 4 && e[9] == (uint8_t)ox && e[10] == (uint8_t)oy && fabsf(*(float *)(e + 0xC) - otop) < 0.01f) {
            e[9] = (uint8_t)*(const short *)(p + 0x54);
            e[10] = (uint8_t)*(const short *)(p + 0x56);
            *(float *)(e + 0xC) = *(const float *)(p + 0x78);
            if (e[9] != ox || e[10] != oy)
                port_log("editor: grid height (%d,%d) -> (%d,%d) %.2f\n", ox, oy, e[9], e[10], *(float *)(e + 0xC));
            return;
        }
}

static void service_grid()                              // game thread: rebuild the battle's height tables
{
    if (!g_grid_dirty || GetTickCount() - g_grid_moved < 100) return;
    g_grid_dirty = false;
    const uint8_t *state = *(const uint8_t *const *)(0x38E448 + 0x14);
    uint8_t *b = *(uint8_t **)0x3D2970;
    if (!state || *(const int *)(state + 8) != 12 || !b) return;
    uint8_t *self = b + 0x10;
    DWORD t0 = GetTickCount();
    ((void(__cdecl *)(void *))0xA23C0)(self + 0x20);
    ((void(__cdecl *)(void *))0xA2660)(self + 0x24);
    ((void(__cdecl *)(void *))0xA2D20)(self + 0x20);
    port_log("editor: battle grid + path heights rebuilt (%lu ms)\n", GetTickCount() - t0);
}

static void move_live(uint8_t *p, const float pos[3], const float rot[3])   // game thread
{
    bool battle = in_battle_file(p);
    int ox = *(short *)(p + 0x54), oy = *(short *)(p + 0x56);
    float otop = *(float *)(p + 0x78);
    for (int k = 0; k < 3; k++) {
        *(float *)(p + 0x20 + 4 * k) = pos[k];
        *(float *)(p + 0x64 + 4 * k) = rot[k];
    }
    ((void(__fastcall *)(void *))0x1E02F0)(p);
    ((void(__fastcall *)(void *))0x1E07F0)(p);
    *(float *)(p + 0x74) = pos[2];
    *(float *)(p + 0x78) = *(float *)(p + 0x70) + pos[2];
    if (battle) {
        move_height_feature(ox, oy, otop, p);
        g_grid_moved = GetTickCount();
        g_grid_dirty = true;
    }
}

static void service_drag()                              // game thread
{
    if (g_drag.raise && g_drag.wheel_index >= 0) {      // numpad height steps
        int steps = g_drag.raise;
        g_drag.raise = 0;
        if (uint8_t *p = live_prop(g_drag.file, g_drag.wheel_index)) {
            float pos[3], rot[3];
            for (int k = 0; k < 3; k++) pos[k] = *(float *)(p + 0x20 + 4 * k), rot[k] = *(float *)(p + 0x64 + 4 * k);
            pos[2] += steps * 0.05f;
            move_live(p, pos, rot);
            for (int k = 0; k < 3; k++) g_drag.result[k] = pos[k], g_drag.result[3 + k] = rot[k];
            if (g_wnd) PostMessageA(g_wnd, WM_EDITOR_MOVED, g_drag.wheel_index, 0);
        }
    }
    if (g_drag.wheel && g_drag.wheel_index >= 0) {
        int notches = g_drag.wheel;
        g_drag.wheel = 0;
        if (uint8_t *p = live_prop(g_drag.file, g_drag.wheel_index)) {
            float pos[3], rot[3];
            for (int k = 0; k < 3; k++) pos[k] = *(float *)(p + 0x20 + 4 * k), rot[k] = *(float *)(p + 0x64 + 4 * k);
            rot[2] += notches * 0.2617994f;             // 15 degrees
            while (rot[2] > 3.1415927f) rot[2] -= 6.2831853f;
            while (rot[2] < -3.1415927f) rot[2] += 6.2831853f;
            move_live(p, pos, rot);
            for (int k = 0; k < 3; k++) g_drag.result[k] = pos[k], g_drag.result[3 + k] = rot[k];
            if (g_wnd) PostMessageA(g_wnd, WM_EDITOR_MOVED, g_drag.wheel_index, 0);
        }
    }
    if (!g_drag.move_pending || g_drag.index < 0) return;
    g_drag.move_pending = false;
    uint8_t *p = live_prop(g_drag.file, g_drag.index);
    float o[3], d[3];
    if (!p || !camera_ray(g_drag.nx, g_drag.ny, o, d) || fabsf(d[2]) < 1e-4f) return;
    float z0 = *(float *)(p + 0x28), t = (z0 - o[2]) / d[2];
    if (t <= 0) return;
    float pos[3] = {o[0] + d[0] * t, o[1] + d[1] * t, z0}, rot[3], gz;
    if (ground_at(pos[0], pos[1], z0 + 5.0f, gz)) pos[2] = gz + g_drag.lift;   // keeps a raised prop raised
    for (int k = 0; k < 3; k++) rot[k] = *(float *)(p + 0x64 + 4 * k);
    move_live(p, pos, rot);
    for (int k = 0; k < 3; k++) g_drag.result[k] = pos[k], g_drag.result[3 + k] = rot[k];
    if (g_wnd) PostMessageA(g_wnd, WM_EDITOR_MOVED, g_drag.index, 0);
}

void port_editor_drag(int x, int y, int w, int h)      // game window, mouse move with the left button down
{
    if (port_is_editor() && w > 0 && h > 0 && coll_active()) { coll_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, true); return; }
    if (port_is_editor() && w > 0 && h > 0 && grid_active()) { grid_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, true); return; }
    if (port_is_editor() && w > 0 && h > 0 && mesh_active()) { mesh_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, true); return; }
    if (!port_is_editor() || w <= 0 || h <= 0 || g_drag.index < 0) return;
    g_drag.nx = 2.0f * x / w - 1.0f;
    g_drag.ny = 1.0f - 2.0f * y / h;
    g_drag.move_pending = true;
}

void port_editor_release()                              // game window, left button up
{
    if (port_is_editor() && coll_active()) coll_release();
    if (port_is_editor() && grid_active()) grid_release();
    g_drag.index = -1;
    if (port_is_editor() && mesh_active()) mesh_release();
}

void port_editor_hover(int x, int y, int w, int h)     // game window, mouse move without a button (brush cursor)
{
    if (port_is_editor() && w > 0 && h > 0 && coll_active()) { coll_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, false); return; }
    if (port_is_editor() && w > 0 && h > 0 && grid_active()) { grid_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, false); return; }
    if (port_is_editor() && w > 0 && h > 0 && mesh_active()) mesh_move(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, false);
}

bool port_editor_key(int vk)                            // game window: Ctrl+Z / Ctrl+Y, numpad keys - true = used
{
    if (!port_is_editor()) return false;
    if (mesh_active()) return mesh_key(vk);
    // numpad 3 / 1: raise / lower the selected prop (0.25 units, Shift 0.05), live
    if ((vk != VK_NUMPAD1 && vk != VK_NUMPAD3) || g_sel < 0 || g_sel >= (int)g_props.size() || g_props[g_sel].live < 0)
        return false;
    g_drag.file = g_file;
    g_drag.wheel_index = g_props[g_sel].live;
    g_drag.raise += (vk == VK_NUMPAD3 ? 1 : -1) * ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 5);
    return true;
}

void port_editor_wheel(int delta)                       // game window, mouse wheel
{
    if (port_is_editor() && mesh_active()) {
        mesh_wheel(delta / WHEEL_DELTA, (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return;
    }
    if (!port_is_editor() || g_sel < 0 || g_sel >= (int)g_props.size() || g_props[g_sel].live < 0) return;
    g_drag.file = g_file;
    g_drag.wheel_index = g_props[g_sel].live;
    g_drag.wheel += delta / WHEEL_DELTA;
}

void port_editor_pick(int x, int y, int w, int h)      // game window, left click
{
    if (port_is_editor() && w > 0 && h > 0 && coll_active()) {
        coll_press(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return;
    }
    if (port_is_editor() && w > 0 && h > 0 && grid_active()) {
        grid_press(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return;
    }
    if (port_is_editor() && w > 0 && h > 0 && mesh_active()) {
        mesh_press(2.0f * x / w - 1.0f, 1.0f - 2.0f * y / h, (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                   (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return;
    }
    if (!port_is_editor() || w <= 0 || h <= 0 || g_file.empty()) return;
    g_pick.file = g_file;
    g_drag.file = g_file;
    g_drag.index = -1;
    g_pick.nx = 2.0f * x / w - 1.0f;
    g_pick.ny = 1.0f - 2.0f * y / h;
    g_pick.pending = true;
}

void port_editor_init()                                 // port_main, editor mode only
{
    if (!port_is_editor()) return;
    InitializeCriticalSection(&g_cs);
    mesh_init();
    g_req.ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_hq.ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_req.done = true;
    static const uint8_t want[7] = {0x6A, 0xFF, 0x68, 0xB1, 0x8B, 0x29, 0x00};
    if (!memcmp((void *)0x1E1ED0, want, 7)) {         // live props: note each loaded prop file object
        uint8_t *w = (uint8_t *)0x1E1ED0;
        w[0] = 0xE9;
        *(int32_t *)(w + 1) = (int32_t)((uintptr_t)prp_load_hook - (0x1E1ED0 + 5));
        w[5] = w[6] = 0x90;
    } else {
        port_log("editor: .prp loader code unexpected - live moves off\n");
    }
    static const uint8_t want_dtor[7] = {0x6A, 0xFF, 0x68, 0xE8, 0x8B, 0x29, 0x00};
    if (!memcmp((void *)0x1E2680, want_dtor, 7)) {    // ...and forget it when the game destroys it
        uint8_t *w = (uint8_t *)0x1E2680;
        w[0] = 0xE9;
        *(int32_t *)(w + 1) = (int32_t)((uintptr_t)prp_dtor_hook - (0x1E2680 + 5));
        w[5] = w[6] = 0x90;
    } else {
        port_log("editor: prop file destructor code unexpected - live moves off\n");
        *(uint8_t *)0x1E1ED0 = 0x6A, *(uint16_t *)0x1E1ED1 = 0x68FF, *(uint32_t *)0x1E1ED3 = 0x00298BB1;
    }
    CloseHandle(CreateThread(nullptr, 0, editor_thread, nullptr, 0, nullptr));
    port_log("editor: window started (Ctrl+E shows it, Ctrl+G God Camera, Ctrl+R reload)\n");
}

void port_editor_show()                                 // Ctrl+E
{
    if (g_frame) ShowWindow(g_frame, SW_SHOWNORMAL), SetForegroundWindow(g_frame);
}
