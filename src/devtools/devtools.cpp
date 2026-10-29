// devtools.dll - GladiusPort developer tools ([devtools] Enabled=1): a console on the retail console engine (in game
// on the ` key, and in the tools window), the variable / command browser, the port log, graphs and the Dog script
// debugger in the "GladiusPort Dev Tools" window (Ctrl+` or [devtools] Window=1 at start).
// Online matches: anything that would change the game for one player is refused (console.cpp).
#include "devtools.h"
#include <windows.h>
#include <string.h>
#include <deque>
#include <mutex>

const DevHost *g_host;

// ------------------------------------------------------------------------------------------------ port log tap
static std::mutex g_log_lock;
static std::deque<std::string> g_log;
static size_t g_log_gen;

static void __cdecl log_line(const char *line)
{
    std::lock_guard<std::mutex> l(g_log_lock);
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    g_log.push_back(s);
    if (g_log.size() > 20000) g_log.pop_front();
    g_log_gen++;
}

bool log_tail(TailFn fn, void *ctx, size_t n)
{
    std::unique_lock<std::mutex> l(g_log_lock, std::try_to_lock);
    if (!l.owns_lock()) return false;
    for (size_t i = g_log.size() > n ? g_log.size() - n : 0; i < g_log.size(); i++) fn(g_log[i].c_str(), ctx);
    return true;
}

std::vector<std::string> log_lines(size_t *gen)
{
    std::lock_guard<std::mutex> l(g_log_lock);
    if (gen) *gen = g_log_gen;
    return std::vector<std::string>(g_log.begin(), g_log.end());
}

// ------------------------------------------------------------------------------------------------ in-game console
static std::mutex g_in_lock;
static bool g_open, g_was_open, g_eat_char;
static std::string g_input;
static int g_hist = -1;                                 // -1 = editing a new line
static int g_scroll;                                    // lines scrolled up from the bottom

bool ingame_open() { return g_open; }

static void complete_input()
{
    size_t sp = g_input.find_last_of(" ;=");
    std::string head = sp == std::string::npos ? std::string() : g_input.substr(0, sp + 1);
    std::string word = sp == std::string::npos ? g_input : g_input.substr(sp + 1);
    if (word.empty()) return;
    std::vector<std::string> c = con_complete(word);
    if (c.empty()) return;
    if (c.size() == 1) { g_input = head + c[0] + " "; return; }
    std::string common = c[0];                          // extend to the longest common prefix, list the rest
    for (auto &s : c) {
        size_t i = 0;
        while (i < common.size() && i < s.size() && tolower((unsigned char)common[i]) == tolower((unsigned char)s[i])) i++;
        common.resize(i);
    }
    if (common.size() > word.size()) g_input = head + common;
    std::string list;
    for (size_t i = 0; i < c.size() && i < 40; i++) list += c[i] + "  ";
    if (c.size() > 40) list += "...";
    con_print(OUT_HELP, "%s", list.c_str());
}

int ingame_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && dogdbg_stopped()) {         // the game is stopped at a script break: debugger keys
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wp == VK_F5) { dogdbg_command(DCMD_CONTINUE); return 1; }
        if (wp == VK_F10) { dogdbg_command(DCMD_OVER); return 1; }
        if (wp == VK_F11) { dogdbg_command(shift ? DCMD_OUT : DCMD_STEP); return 1; }
    }
    if (msg == WM_KEYDOWN && wp == VK_OEM_3) {          // ` toggles the console, Ctrl+` the tools window
        g_eat_char = true;
        if (GetKeyState(VK_CONTROL) & 0x8000) tools_window_toggle();
        else {
            std::lock_guard<std::mutex> l(g_in_lock);
            g_open = !g_open;
            g_hist = -1;
            g_scroll = 0;
        }
        return 1;
    }
    if (msg == WM_CHAR && g_eat_char) { g_eat_char = false; return 1; }
    if (!g_open) return 0;
    std::lock_guard<std::mutex> l(g_in_lock);
    switch (msg) {
    case WM_CHAR:
        if (wp >= 32 && wp < 127) g_input += (char)wp;
        return 1;
    case WM_KEYDOWN: {
        const std::vector<std::string> &hist = con_history();
        switch (wp) {
        case VK_RETURN:
            con_submit(g_input.c_str());
            g_input.clear();
            g_hist = -1;
            g_scroll = 0;
            break;
        case VK_ESCAPE: g_open = false; break;
        case VK_BACK: if (!g_input.empty()) g_input.pop_back(); break;
        case VK_TAB: complete_input(); break;
        case VK_UP:
            if (!hist.empty()) {
                g_hist = g_hist < 0 ? (int)hist.size() - 1 : (g_hist > 0 ? g_hist - 1 : 0);
                g_input = hist[g_hist];
            }
            break;
        case VK_DOWN:
            if (g_hist >= 0) {
                g_hist++;
                if (g_hist >= (int)hist.size()) { g_hist = -1; g_input.clear(); }
                else g_input = hist[g_hist];
            }
            break;
        case VK_PRIOR: g_scroll += 8; break;
        case VK_NEXT: g_scroll = g_scroll > 8 ? g_scroll - 8 : 0; break;
        case 'L': if (GetKeyState(VK_CONTROL) & 0x8000) con_clear(); break;
        }
        return 1;
    }
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSCHAR:
        return 1;
    }
    return 0;
}

static uint32_t kind_colour(OutKind k)
{
    switch (k) {
    case OUT_ECHO: return 0xFFFFE080;
    case OUT_ERROR: return 0xFFFF6060;
    case OUT_HELP: return 0xFFA0D0FF;
    default: return 0xFFE0E0E0;
    }
}

void ingame_draw()                                      // game thread
{
    bool open;
    std::string input;
    int scroll;
    {
        std::lock_guard<std::mutex> l(g_in_lock);
        open = g_open, input = g_input, scroll = g_scroll;
    }
    if (!open) {
        if (g_was_open) {
            g_host->ov_begin();
            g_host->ov_end(false);
            g_host->ov_panel(false);
            g_was_open = false;
        }
        return;
    }
    g_was_open = true;
    const float H = 230, LH = 11;
    g_host->ov_begin();
    g_host->ov_box(0, 0, 640, H, 0xD0101018);
    g_host->ov_box(0, H, 640, 2, 0xFF6080C0);
    std::vector<OutLine> lines = con_lines(nullptr);
    int rows = (int)((H - 24) / LH);
    int last = (int)lines.size() - 1 - scroll;
    float y = H - 22;
    for (int i = last; i >= 0 && i > last - rows; i--, y -= LH)
        g_host->ov_text(lines[i].text.c_str(), 6, y, 0, kind_colour(lines[i].kind), true);
    std::string shown = std::string(con_online() ? "[online] > " : "> ") + input + ((GetTickCount() / 400) & 1 ? "_" : " ");
    g_host->ov_text(shown.c_str(), 6, H - 8, 0, 0xFFFFFFFF, true);
    if (scroll) g_host->ov_text("(scrolled - PgDn)", 634, H - 8, 2, 0xFF909090, true);
    g_host->ov_end(true);
    g_host->ov_panel(true);
}

// ------------------------------------------------------------------------------------------------ host interface
static void __cdecl frame()
{
    crash_frame();
    graphs_frame();
    dogdbg_frame();
    flow_frame();
    con_frame();
    ingame_draw();
}

static int __cdecl wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return ingame_wndproc(h, msg, wp, lp); }
static bool __cdecl keyboard_captured() { return g_open; }

static void __cdecl on_crash(EXCEPTION_POINTERS *ep, const char *path, const char *why) { crash_report(ep, path, why); }

static void __cdecl shutdown()                          // online lobby: everything off (game thread)
{
    g_open = false;
    dogdbg_shutdown();
    gamelog_shutdown();
    con_shutdown();
    tools_window_stop();
    g_host->prof_live(false);
    g_host->log("devtools: hooks removed, tools window closed\n");
}

static const DevToolsApi k_api = {DEVTOOLS_ABI, frame, wndproc, log_line, keyboard_captured, on_crash, shutdown};

extern "C" __declspec(dllexport) const DevToolsApi *__cdecl devtools_api(const DevHost *host)
{
    if (!host || host->abi != DEVTOOLS_ABI) return nullptr;
    g_host = host;
    con_init();
    gamelog_init();
    dogdbg_init();
    tools_window_start(GetPrivateProfileIntA("devtools", "Window", 0, host->ini) != 0);
    host->log("devtools: ready (` console, Ctrl+` tools window)\n");
    return &k_api;
}
