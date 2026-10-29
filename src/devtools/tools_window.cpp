// "GladiusPort Dev Tools" window: its own thread, Win32 window and OpenGL context, Dear ImGui. It keeps running
// when the game thread is stopped (script debugger breakpoints). It never touches game state directly: changes go
// through the console queue (con_submit), run on the game thread.
#include "devtools.h"
#include <windows.h>
#include <GL/gl.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl2.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static HWND g_wnd;
// OpenGL, not Direct3D 11: overlay tools that hook D3D11 for the whole process (RivaTuner Statistics Server patches
// ID3D11DeviceContext methods) crash when a second D3D11 device draws on another thread while the game draws.
static HDC g_dc;
static HGLRC g_gl;
static volatile LONG g_show, g_quit;
static const UINT WM_DT_TOGGLE = WM_APP + 1, WM_DT_SHOW = WM_APP + 2;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_CLOSE:                                      // closing only hides it (Ctrl+` brings it back)
        ShowWindow(h, SW_HIDE);
        InterlockedExchange(&g_show, 0);
        return 0;
    case WM_DT_SHOW:
        ShowWindow(h, SW_SHOW);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        InterlockedExchange(&g_show, 1);
        return 0;
    case WM_DT_TOGGLE:
        if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
        else { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); }
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void tools_window_toggle()
{
    if (g_wnd) PostMessageA(g_wnd, WM_DT_TOGGLE, 0, 0);
}

void tools_window_show()
{
    if (g_wnd) PostMessageA(g_wnd, WM_DT_SHOW, 0, 0);
}

// ------------------------------------------------------------------------------------------------ panels
static ImVec4 kind_colour(OutKind k)
{
    switch (k) {
    case OUT_ECHO: return ImVec4(1.0f, 0.88f, 0.5f, 1);
    case OUT_ERROR: return ImVec4(1.0f, 0.4f, 0.4f, 1);
    case OUT_HELP: return ImVec4(0.63f, 0.82f, 1.0f, 1);
    default: return ImVec4(0.88f, 0.88f, 0.88f, 1);
    }
}

static int g_hist_pos = -1;

static int input_callback(ImGuiInputTextCallbackData *d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        std::string text(d->Buf, d->BufTextLen);
        size_t sp = text.find_last_of(" ;=", d->CursorPos ? d->CursorPos - 1 : 0);
        size_t start = sp == std::string::npos ? 0 : sp + 1;
        std::string word = text.substr(start, d->CursorPos - start);
        if (word.empty()) return 0;
        std::vector<std::string> c = con_complete(word);
        if (c.empty()) return 0;
        std::string common = c[0];
        for (auto &s : c) {
            size_t i = 0;
            while (i < common.size() && i < s.size() && tolower((unsigned char)common[i]) == tolower((unsigned char)s[i])) i++;
            common.resize(i);
        }
        if (c.size() == 1) common = c[0] + " ";
        d->DeleteChars((int)start, (int)word.size());
        d->InsertChars(d->CursorPos, common.c_str());
        if (c.size() > 1) {
            std::string list;
            for (size_t i = 0; i < c.size() && i < 60; i++) list += c[i] + "  ";
            con_print(OUT_HELP, "%s", list.c_str());
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const std::vector<std::string> &h = con_history();
        if (h.empty()) return 0;
        if (d->EventKey == ImGuiKey_UpArrow) g_hist_pos = g_hist_pos < 0 ? (int)h.size() - 1 : (g_hist_pos > 0 ? g_hist_pos - 1 : 0);
        else if (g_hist_pos >= 0 && ++g_hist_pos >= (int)h.size()) g_hist_pos = -1;
        d->DeleteChars(0, d->BufTextLen);
        if (g_hist_pos >= 0) d->InsertChars(0, h[g_hist_pos].c_str());
    }
    return 0;
}

static void panel_console()
{
    if (!ImGui::BeginTabItem("Console")) return;
    float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("out", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    static size_t seen;
    size_t gen;
    std::vector<OutLine> lines = con_lines(&gen);
    for (auto &l : lines) {
        ImGui::PushStyleColor(ImGuiCol_Text, kind_colour(l.kind));
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (gen != seen && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
    seen = gen;
    ImGui::EndChild();
    static char buf[256];
    if (con_online()) { ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "[online]"); ImGui::SameLine(); }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##in", buf, sizeof buf,
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                             ImGuiInputTextFlags_CallbackHistory,
                         input_callback)) {
        con_submit(buf);
        buf[0] = 0;
        g_hist_pos = -1;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndTabItem();
}

static const char *type_name(int t)
{
    static const char *const k[] = {"ubyte", "sbyte", "ushort", "sshort", "uint", "int", "float", "hex"};
    return t >= 0 && t < 8 ? k[t] : "?";
}

static bool match(const std::string &s, const char *filter)
{
    if (!*filter) return true;
    std::string a = s, b = filter;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

static void panel_vars(const std::vector<RegEntry> &reg)
{
    if (!ImGui::BeginTabItem("Variables")) return;
    static char filter[64];
    static bool toggles_only;
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##f", "filter (name or description)", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::Checkbox("on/off switches only", &toggles_only);
    if (con_online()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "online: read only"); }
    if (ImGui::BeginTable("vars", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Description");
        ImGui::TableHeadersRow();
        for (auto &e : reg) {
            if (e.kind != RegEntry::VAR) continue;
            bool toggle = e.count <= 1 && e.vartype != 6 && e.min == 0 && e.max == 1;
            if (toggles_only && !toggle) continue;
            if (!match(e.name, filter) && !match(e.desc, filter)) continue;
            ImGui::PushID(e.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn();
            std::string v = con_var_value(e);
            ImGui::BeginDisabled(con_online());
            if (toggle) {
                bool on = v != "0";
                if (ImGui::Checkbox("##v", &on)) {
                    char line[160];
                    sprintf(line, "%s=%d", e.name.c_str(), on ? 1 : 0);
                    con_submit(line);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(v.c_str());
            } else {
                char edit[64];
                strncpy(edit, v.c_str(), sizeof edit - 1);
                edit[sizeof edit - 1] = 0;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##v", edit, sizeof edit, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    char line[200];
                    _snprintf(line, sizeof line - 1, "%s=%s", e.name.c_str(), edit);
                    line[sizeof line - 1] = 0;
                    con_submit(line);
                }
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(type_name(e.vartype));
            ImGui::TableNextColumn();
            if (e.min != e.max) ImGui::Text("%g..%g", e.min, e.max);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.desc.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

static void panel_commands(const std::vector<RegEntry> &reg)
{
    if (!ImGui::BeginTabItem("Commands")) return;
    static char filter[64];
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##f", "filter", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::TextDisabled("Most commands need arguments: type them in the box, Run (or Enter). ? shows the usage.");
    static std::vector<std::pair<std::string, std::string>> args;   // per command
    auto arg_buf = [&](const std::string &n) -> std::string & {
        for (auto &a : args) if (a.first == n) return a.second;
        args.push_back({n, std::string()});
        return args.back().second;
    };
    if (ImGui::BeginTable("cmds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Description / alias");
        ImGui::TableHeadersRow();
        for (auto &e : reg) {
            if (e.kind == RegEntry::VAR) continue;
            if (!match(e.name, filter) && !match(e.desc, filter) && !match(e.alias_text, filter)) continue;
            ImGui::PushID(e.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string &a = arg_buf(e.name);
            bool run = ImGui::SmallButton("Run");
            ImGui::SameLine();
            if (e.kind == RegEntry::CMD && ImGui::SmallButton("?")) con_submit((e.name + " ?").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn();
            char buf[128];
            strncpy(buf, a.c_str(), sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            ImGui::SetNextItemWidth(-1);
            std::string hint = e.usage.substr(0, e.usage.find('\n'));
            if (ImGui::InputTextWithHint("##a", hint.c_str(), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue))
                run = true;
            a = buf;
            if (run) con_submit(a.empty() ? e.name.c_str() : (e.name + " " + a).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.kind == RegEntry::ALIAS ? ("alias: " + e.alias_text).c_str() : e.desc.c_str());
            if (!e.usage.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.75f, 0.9f, 1));
                ImGui::TextUnformatted(e.usage.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

static void panel_log()
{
    if (!ImGui::BeginTabItem("Log")) return;
    static char filter[64];
    static bool follow = true;
    static int which;                                   // 0 game output, 1 port.log
    ImGui::RadioButton("Game output", &which, 0);
    ImGui::SameLine();
    ImGui::RadioButton("port.log", &which, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##f", "filter", filter, sizeof filter);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &follow);
    if (which == 0) { ImGui::SameLine(); if (ImGui::SmallButton("Clear")) gamelog_clear(); }
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    size_t gen;
    std::vector<std::string> lines = which == 0 ? gamelog_lines(&gen) : log_lines(&gen);
    if (*filter) {
        for (auto &l : lines)
            if (match(l, filter)) ImGui::TextUnformatted(l.c_str());
    } else {
        ImGuiListClipper clip;
        clip.Begin((int)lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(lines[i].c_str());
    }
    if (follow) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndTabItem();
}

// ------------------------------------------------------------------------------------------------ thread
static DWORD WINAPI window_thread(void *)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "GladiusPortDevTools";
    RegisterClassA(&wc);
    g_wnd = CreateWindowA(wc.lpszClassName, "GladiusPort Dev Tools", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          1100, 750, nullptr, nullptr, wc.hInstance, nullptr);
    g_dc = GetDC(g_wnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd) || !(g_gl = wglCreateContext(g_dc)) || !wglMakeCurrent(g_dc, g_gl)) {
        g_host->log("devtools: tools window - no OpenGL context\n");
        return 0;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static char ini_path[MAX_PATH];                     // layout next to gladiusport.ini
    strncpy(ini_path, g_host->ini, MAX_PATH - 1);
    if (char *s = strrchr(ini_path, '\\')) strcpy(s + 1, "devtools_layout.ini");
    io.IniFilename = ini_path;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_wnd);
    ImGui_ImplOpenGL2_Init();
    if (g_show) ShowWindow(g_wnd, SW_SHOW);

    while (!g_quit) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (!IsWindowVisible(g_wnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // one window filling the client area, a tab per tool
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##tools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        std::vector<RegEntry> reg = con_registry(nullptr);
        if (ImGui::BeginTabBar("tabs")) {
            panel_console();
            panel_vars(reg);
            panel_commands(reg);
            panel_log();
            panel_graphs();
            panel_scripts();
            panel_quests();
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();
        RECT rc;
        GetClientRect(g_wnd, &rc);
        glViewport(0, 0, rc.right, rc.bottom);
        glClearColor(0.08f, 0.08f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(g_dc);
        Sleep(15);                                      // ~60 updates a second, no vsync dependency
    }
    HWND w = g_wnd;                                     // online lobby: gone for the rest of the run
    g_wnd = nullptr;
    ShowWindow(w, SW_HIDE);
    DestroyWindow(w);
    return 0;
}

void tools_window_stop()
{
    InterlockedExchange(&g_quit, 1);
    if (HWND w = g_wnd) PostMessageA(w, WM_NULL, 0, 0);  // wake its wait
}

void tools_window_start(bool show)
{
    g_show = show;
    CloseHandle(CreateThread(nullptr, 0, window_thread, nullptr, 0, nullptr));
}
