// Soft reboot (leaving the world map, quitting to the menu). The game calls XLaunchNewImage to reboot into
// itself. Here that starts a fresh gladius.exe with the launch data (launch.bin) and the window placement; the new
// window takes over when its first frame is up and the old process exits. Other titles are refused.
#include "port.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

enum { LAUNCH_DATA = 3072 };
static bool g_have_launch;
static DWORD g_launch_type;
static BYTE g_launch_data[LAUNCH_DATA];

void d3d_window_handoff(int l, int t, int r, int b, int show, bool borderless, HANDLE ev);   // d3d_core.cpp
bool d3d_window_state(int *l, int *t, int *r, int *b, int *show, bool *borderless);

// Folder of gladius.exe, with a trailing backslash.
static std::string exe_dir()
{
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    *(strrchr(p, 0x5C) + 1) = 0;
    return p;
}

// The command line without the program name and without this module's own switches and their values.
static std::string user_args()
{
    const char *c = GetCommandLineA();
    if (*c == '"') {
        c = strchr(c + 1, '"');
        c = c ? c + 1 : "";
    } else {
        while (*c && *c != ' ')
            c++;
    }
    std::string out;
    while (*c) {
        while (*c == ' ')
            c++;
        if (!*c)
            break;
        const char *s = c;
        bool q = false;
        while (*c && (q || *c != ' ')) {
            if (*c == '"')
                q = !q;
            c++;
        }
        std::string tok(s, c - s);
        if (tok == "-launchdata" || tok == "-handoff" || tok == "-winplace") {   // skip the value too
            while (*c == ' ')
                c++;
            q = false;
            while (*c && (q || *c != ' ')) {
                if (*c == '"')
                    q = !q;
                c++;
            }
            continue;
        }
        out += " " + tok;
    }
    return out;
}

// XLaunchNewImage replacement: relaunch this title and hand the window over.
static DWORD __stdcall launch_new_image(const char *path, const BYTE *data)
{
    port_log("reboot: XLaunchNewImage('%s')\n", path ? path : "(dashboard)");
    const char *base = path ? strrchr(path, 0x5C) : nullptr;
    base = base ? base + 1 : path;
    if (!base || (_stricmp(base, "default.xbe") && _stricmp(base, "gladius.xbe")))
        return ERROR_NOT_SUPPORTED;                     // the dashboard / other titles: nothing to launch on a PC
    std::string dir = exe_dir(), file = dir + "launch.bin";
    if (FILE *f = fopen(file.c_str(), "wb")) {          // LDT_TITLE + the 3072-byte launch data, like the console
        DWORD type = 0;
        BYTE buf[LAUNCH_DATA] = {};
        if (data)
            memcpy(buf, data, sizeof buf);
        fwrite(&type, 4, 1, f);
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
    }
    char ev_name[64];
    _snprintf(ev_name, sizeof ev_name, "Local\\GladiusPortHandoff_%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, ev_name);
    int l = 0, t = 0, r = 0, b = 0, show = SW_SHOWNORMAL;
    bool bl = false;
    d3d_window_state(&l, &t, &r, &b, &show, &bl);
    char exe[MAX_PATH], extra[256];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    _snprintf(extra, sizeof extra, " -launchdata \"%s\" -handoff %s -winplace %d,%d,%d,%d,%d,%d", file.c_str(), ev_name,
              l, t, r, b, show, bl ? 1 : 0);
    std::string cmd = std::string("\"") + exe + "\"" + user_args() + extra;
    {   // keep this run's log: the new process starts a fresh port.log
        SYSTEMTIME st;
        GetLocalTime(&st);
        char dst[MAX_PATH];
        _snprintf(dst, sizeof dst, "%slogs\\port_%04d%02d%02d_%02d%02d%02d_reboot.log", dir.c_str(), st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA((dir + "logs").c_str(), nullptr);
        CopyFileA((dir + "port.log").c_str(), dst, FALSE);
    }
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::string cmdbuf = cmd;
    if (!CreateProcessA(exe, &cmdbuf[0], nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi)) {
        port_log("reboot: relaunch failed (%lu)\n", GetLastError());
        return ERROR_NOT_SUPPORTED;
    }
    port_log("reboot: relaunched (pid %lu), waiting for its window\n", pi.dwProcessId);
    AllowSetForegroundWindow(pi.dwProcessId);
    HANDLE w[2] = {ev, pi.hProcess};
    DWORD r2 = WaitForMultipleObjects(2, w, FALSE, 60000);
    port_log("reboot: %s - this process exits\n", r2 == WAIT_OBJECT_0 ? "new window is up" : "handoff timed out / ended");
    TerminateProcess(GetCurrentProcess(), 0);           // game threads are mid-shutdown: no orderly exit needed
    return 0;
}

// XGetLaunchInfo replacement: the launch data passed by a soft reboot, once.
static DWORD __stdcall get_launch_info(DWORD *type, BYTE *data)
{
    if (!g_have_launch)
        return 0x490;                                   // ERROR_NOT_FOUND: a cold boot
    *type = g_launch_type;
    memcpy(data, g_launch_data, LAUNCH_DATA);
    g_have_launch = false;
    port_log("reboot: XGetLaunchInfo -> type %lu, first dword %lu\n", g_launch_type, *(DWORD *)g_launch_data);
    return 0;
}

// Value of a command-line switch ("" when absent).
static std::string arg_value(const char *name)
{
    const char *c = strstr(GetCommandLineA(), name);
    if (!c)
        return "";
    c += strlen(name);
    while (*c == ' ')
        c++;
    std::string v;
    if (*c == '"') {
        const char *e = strchr(c + 1, '"');
        v.assign(c + 1, e ? e - c - 1 : strlen(c + 1));
    } else {
        while (*c && *c != ' ')
            v += *c++;
    }
    return v;
}

// Hook XLaunchNewImage / XGetLaunchInfo and read the launch data when started by a soft reboot.
void port_reboot_init()
{
    static const BYTE launch_pro[] = {0xB8, 0x00, 0xBF, 0x37, 0x00};   // mov eax, 0x37BF00
    static const BYTE info_pro[] = {0xA1, 0x08, 0xB2, 0x33, 0x00};     // mov eax, [LaunchDataPage thunk]
    if (memcmp((void *)0x16A5F6, launch_pro, sizeof launch_pro) || memcmp((void *)0x16A719, info_pro, sizeof info_pro)) {
        port_log("reboot: XLaunchNewImage / XGetLaunchInfo code unexpected - soft reboot not handled\n");
        return;
    }
    *(BYTE *)0x16A5F6 = 0xE9;
    *(int32_t *)0x16A5F7 = (int32_t)((DWORD)launch_new_image - (0x16A5F6 + 5));
    *(BYTE *)0x16A719 = 0xE9;
    *(int32_t *)0x16A71A = (int32_t)((DWORD)get_launch_info - (0x16A719 + 5));
    std::string file = arg_value("-launchdata ");
    if (!file.empty()) {                                // started by a soft reboot
        if (FILE *f = fopen(file.c_str(), "rb")) {
            g_have_launch = fread(&g_launch_type, 4, 1, f) == 1 && fread(g_launch_data, 1, LAUNCH_DATA, f) > 0;
            fclose(f);
        }
        DeleteFileA(file.c_str());
        int l, t, r, b, show, bl;
        std::string ev = arg_value("-handoff "), wp = arg_value("-winplace ");
        if (!ev.empty() && sscanf(wp.c_str(), "%d,%d,%d,%d,%d,%d", &l, &t, &r, &b, &show, &bl) == 6)
            d3d_window_handoff(l, t, r, b, show, bl != 0, OpenEventA(EVENT_MODIFY_STATE, FALSE, ev.c_str()));
        port_log("reboot: started by a soft reboot (launch data %s)\n", g_have_launch ? "read" : "MISSING");
    }
}
