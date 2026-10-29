// Xbox XInput / device enumeration -> SDL2 game controllers or Windows XInput, plus the keyboard for player 1.
// Xbox pads report 8 analog buttons (A B X Y Black White LT RT) and digital d-pad / start / back / thumbs.
#include "d3d.h"
#include "container.h"
#include "handles.h"
#include <initializer_list>
#include "input_sdl.h"

#define XBE_DEVICE_TYPE_GAMEPAD  0x332D74
#define XBE_DEVICE_TYPE_MU       0x332CF8

#pragma pack(push, 1)
struct XGamepad {
    WORD wButtons;
    BYTE bAnalogButtons[8];
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XInputState {
    DWORD dwPacketNumber;
    XGamepad Gamepad;
};
struct XCapabilities {
    BYTE SubType;
    WORD Reserved;
    XGamepad In;
    WORD RumbleLeft, RumbleRight;
};
struct XFeedback {
    DWORD dwStatus;
    HANDLE hEvent;
    BYTE Reserved[58];
    WORD wLeftMotorSpeed, wRightMotorSpeed;
};
#pragma pack(pop)

enum { XB_DPAD_UP = 1, XB_DPAD_DOWN = 2, XB_DPAD_LEFT = 4, XB_DPAD_RIGHT = 8, XB_START = 0x10, XB_BACK = 0x20,
       XB_LTHUMB = 0x40, XB_RTHUMB = 0x80 };
enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LT, XB_RT };

// Windows XInput (xinput1_4 / 1_3 / 9_1_0)
struct WinGamepad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct WinState {
    DWORD dwPacketNumber;
    WinGamepad Gamepad;
};
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, WinState *);
typedef DWORD (WINAPI *XInputSetStateFn)(DWORD, WORD *);
static XInputGetStateFn g_get;
static XInputSetStateFn g_set;

static bool g_sdl;                                    // SDL2 game controllers (input_sdl.cpp) instead of XInput

// Pick the controller backend: SDL2, else the first XInput DLL found.
static void load_xinput()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    if (sdl_input_start()) {
        g_sdl = true;
        return;
    }
    for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        HMODULE m = LoadLibraryA(dll);
        if (m) {
            g_get = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
            g_set = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
            port_log("input: %s\n", dll);
            return;
        }
    }
    port_log("input: no XInput DLL - keyboard only\n");
}

struct Port {
    DWORD packet;
    XGamepad last;
};
// Input state the game's logic depends on (packet numbers, device masks, the port handles). With the container on it
// lives in the shared page, so netplay state syncs keep it identical on every machine.
struct InputShared {
    Port ports[4];
    DWORD last_mask[2];
};
static InputShared g_input_local;
static InputShared *input_shared()
{
    static InputShared *s;
    if (!s)
        s = container_on() ? (InputShared *)container_shared_page() : &g_input_local;
    return s;
}
#define g_ports (input_shared()->ports)
#define g_last_mask (input_shared()->last_mask)

// Game port -> XInput index: connected pads fill game ports in order. Rescanned at most every 500 ms (XInputGetState
// on an empty slot is slow).
static int g_pad_of_port[4] = {-1, -1, -1, -1};
static int g_pad_count;
static DWORD g_scan_tick;

static void scan_pads()
{
    load_xinput();
    if (g_sdl) {                                      // SDL keeps players in stable slots itself
        g_pad_count = sdl_player_count();
        return;
    }
    DWORD now = GetTickCount();
    if (g_scan_tick && now - g_scan_tick < 500)
        return;
    g_scan_tick = now;
    int map[4] = {-1, -1, -1, -1}, n = 0;
    for (int u = 0; u < 4 && g_get; u++) {
        WinState s;
        if (g_get(u, &s) == ERROR_SUCCESS)
            map[n++] = u;
    }
    if (n != g_pad_count || memcmp(map, g_pad_of_port, sizeof map)) {
        port_log("input: %d controller(s):", n);
        for (int p = 0; p < n; p++)
            port_log(" player %d = XInput %d", p + 1, map[p]);
        port_log("\n");
    }
    memcpy(g_pad_of_port, map, sizeof map);
    g_pad_count = n;
}

// [input] Keyboard = auto (only while no controller is connected) | on | off
static int g_keyboard_mode = -1;                      // 0 off, 1 on, 2 auto

static bool keyboard_active()
{
    if (g_keyboard_mode < 0) {
        char ini[MAX_PATH], val[16];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
        GetPrivateProfileStringA("input", "Keyboard", "auto", val, sizeof val, ini);
        g_keyboard_mode = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 0 : 2;
    }
    return g_keyboard_mode == 1 || (g_keyboard_mode == 2 && g_pad_count == 0);
}

static DWORD injected_ports();                    // ports used by [debug] Press=
// Connected ports as a bit mask (port 0 always: pad or keyboard).
static DWORD gamepad_mask()
{
    scan_pads();
    DWORD m = 1 | injected_ports();
    for (int p = 1; p < g_pad_count; p++)
        m |= 1u << p;
    return m;
}

static bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// [debug] Press=600:START,1400:A,P2B,... holds a button for 6 frames from that frame (testing aid, dev builds)
struct InjectedPress {
    DWORD frame;
    int digital, analog;                               // XB_* digital mask or analog index (-1 = none)
    DWORD port;                                        // "P2A" = port 2 (default port 1)
};
static DWORD g_press_ports;                            // ports that injected presses use (reported as present)
static InjectedPress g_presses[96];
static int g_npresses = -1;

static void load_presses()
{
    g_npresses = 0;
    if (PORT_RELEASE) return;                           // no scripted input in release builds
    char ini[MAX_PATH], val[2048];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    GetPrivateProfileStringA("debug", "Press", "", val, sizeof val, ini);
    const struct { const char *name; int digital, analog; } names[] = {
        {"START", XB_START, -1}, {"BACK", XB_BACK, -1}, {"UP", XB_DPAD_UP, -1}, {"DOWN", XB_DPAD_DOWN, -1},
        {"LEFT", XB_DPAD_LEFT, -1}, {"RIGHT", XB_DPAD_RIGHT, -1}, {"A", 0, XB_A}, {"B", 0, XB_B}, {"X", 0, XB_X},
        {"Y", 0, XB_Y}, {"BLACK", 0, XB_BLACK}, {"WHITE", 0, XB_WHITE}, {"LT", 0, XB_LT}, {"RT", 0, XB_RT}};
    for (char *t = strtok(val, ", "); t && g_npresses < 96; t = strtok(nullptr, ", ")) {
        char *colon = strchr(t, ':');
        if (!colon)
            continue;
        *colon = 0;
        const char *btn = colon + 1;
        DWORD port = 0;
        if ((btn[0] == 'P' || btn[0] == 'p') && btn[1] >= '1' && btn[1] <= '4') {
            port = btn[1] - '1';
            btn += 2;
        }
        for (auto &n : names)
            if (!_stricmp(btn, n.name)) {
                g_presses[g_npresses++] = {strtoul(t, nullptr, 10), n.digital, n.analog, port};
                g_press_ports |= 1u << port;
            }
    }
    if (g_npresses)
        port_log("debug: %d injected button presses\n", g_npresses);
}

// Read the controller behind a game port.
static bool get_pad(DWORD port, WinState *s)
{
    if (port > 3)
        return false;
    if (g_sdl) {
        s->dwPacketNumber = 0;
        return sdl_read((int)port, (PadState *)&s->Gamepad);
    }
    int pad = g_pad_of_port[port];
    return pad >= 0 && g_get(pad, s) == ERROR_SUCCESS;
}

static DWORD injected_ports()
{
    if (g_npresses < 0)
        load_presses();
    return g_press_ports;
}

// One port's Xbox gamepad state: injected presses, controller, and the keyboard on port 0.
static void read_port(DWORD port, XGamepad *g)
{
    memset(g, 0, sizeof *g);
    if (g_npresses < 0)
        load_presses();
    for (int i = 0; i < g_npresses; i++)
        if (g_presses[i].port == port && g_d3d.frame >= g_presses[i].frame && g_d3d.frame < g_presses[i].frame + 6) {
                g->wButtons |= g_presses[i].digital;
                if (g_presses[i].analog >= 0)
                    g->bAnalogButtons[g_presses[i].analog] = 255;
            }
    scan_pads();
    WinState s;
    if (get_pad(port, &s)) {
        WORD b = s.Gamepad.wButtons;
        if (b & 0x0001) g->wButtons |= XB_DPAD_UP;
        if (b & 0x0002) g->wButtons |= XB_DPAD_DOWN;
        if (b & 0x0004) g->wButtons |= XB_DPAD_LEFT;
        if (b & 0x0008) g->wButtons |= XB_DPAD_RIGHT;
        if (b & 0x0010) g->wButtons |= XB_START;
        if (b & 0x0020) g->wButtons |= XB_BACK;
        if (b & 0x0040) g->wButtons |= XB_LTHUMB;
        if (b & 0x0080) g->wButtons |= XB_RTHUMB;
        // analog buttons combine with injected presses / keyboard by max
        const struct { WORD bit; int idx; } pads[] = {{0x0100, XB_WHITE}, {0x0200, XB_BLACK}, {0x1000, XB_A},
                                                      {0x2000, XB_B}, {0x4000, XB_X}, {0x8000, XB_Y}};
        for (auto &p : pads)
            if (b & p.bit)
                g->bAnalogButtons[p.idx] = 255;
        if (s.Gamepad.bLeftTrigger > g->bAnalogButtons[XB_LT])
            g->bAnalogButtons[XB_LT] = s.Gamepad.bLeftTrigger;
        if (s.Gamepad.bRightTrigger > g->bAnalogButtons[XB_RT])
            g->bAnalogButtons[XB_RT] = s.Gamepad.bRightTrigger;
        g->sThumbLX = s.Gamepad.sThumbLX;
        g->sThumbLY = s.Gamepad.sThumbLY;
        g->sThumbRX = s.Gamepad.sThumbRX;
        g->sThumbRY = s.Gamepad.sThumbRY;
    }
    bool port_devtools_keyboard_captured();            // devtools_host.cpp: typing in the in-game console
    if (port == 0 && keyboard_active() && g_d3d.hwnd && GetForegroundWindow() == g_d3d.hwnd &&
        !port_devtools_keyboard_captured()) {           // player 1
        if (key(VK_UP)) g->wButtons |= XB_DPAD_UP;
        if (key(VK_DOWN)) g->wButtons |= XB_DPAD_DOWN;
        if (key(VK_LEFT)) g->wButtons |= XB_DPAD_LEFT;
        if (key(VK_RIGHT)) g->wButtons |= XB_DPAD_RIGHT;
        if (key(VK_RETURN)) g->wButtons |= XB_START;
        if (key(VK_BACK)) g->wButtons |= XB_BACK;    // Backspace (Esc quits the game, d3d_core.cpp)
        const struct { int vk, idx; } map[] = {{'Z', XB_A}, {'X', XB_B}, {'A', XB_X}, {'S', XB_Y}, {'Q', XB_WHITE},
                                               {'W', XB_BLACK}, {VK_SHIFT, XB_LT}, {VK_CONTROL, XB_RT}};
        for (auto &m : map)
            if (key(m.vk))
                g->bAnalogButtons[m.idx] = 255;
    }
}

// Frame hook (gn_hooks.cpp): sample this machine's controllers into 4 slots (+0 connected, +4 XGamepad). During a
// lockstep session or replay the engine reads the agreed slots instead (port_pads_override).
static bool g_pads_override;
static BYTE g_pads[4][24];

void port_pads_sample(BYTE *slots)
{
    DWORD mask = gamepad_mask();
    for (DWORD port = 0; port < 4; port++) {
        BYTE *s = slots + port * 24;
        XGamepad g;
        read_port(port, &g);
        *(DWORD *)s = (mask >> port) & 1;
        memcpy(s + 4, &g, sizeof g);
    }
}

void port_pads_override(const BYTE *slots)
{
    if (slots)
        memcpy(g_pads, slots, sizeof g_pads);
    g_pads_override = slots != nullptr;
}

extern "C" {

void __stdcall xapi_XInitDevices(DWORD count, void *types) { load_xinput(); }

DWORD __stdcall xapi_XGetDevices(DWORD type)
{
    if (type != XBE_DEVICE_TYPE_GAMEPAD)
        return 0;                                       // no memory units
    if (g_pads_override) {
        DWORD m = 0;
        for (int p = 0; p < 4; p++)
            if (*(DWORD *)g_pads[p])
                m |= 1u << p;
        return m;
    }
    return gamepad_mask();
}

BOOL __stdcall xapi_XGetDeviceChanges(DWORD type, DWORD *ins, DWORD *rem)
{
    int i = type == XBE_DEVICE_TYPE_GAMEPAD ? 0 : 1;
    DWORD now = xapi_XGetDevices(type), before = g_last_mask[i];
    g_last_mask[i] = now;
    *ins = now & ~before;
    *rem = before & ~now;
    return (*ins | *rem) != 0;
}

HANDLE __stdcall xapi_XInputOpen(DWORD type, DWORD port, DWORD slot, void *params)
{
    if (type != XBE_DEVICE_TYPE_GAMEPAD || port > 3)
        return nullptr;
    return (HANDLE)&g_ports[port];
}

void __stdcall xapi_XInputClose(HANDLE h) {}

DWORD __stdcall xapi_XInputGetCapabilities(HANDLE h, XCapabilities *caps)
{
    memset(caps, 0, sizeof *caps);
    caps->SubType = 1;                                  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    caps->In.wButtons = 0xFF;
    memset(caps->In.bAnalogButtons, 0xFF, 8);
    caps->In.sThumbLX = caps->In.sThumbLY = caps->In.sThumbRX = caps->In.sThumbRY = (SHORT)0xFFFF;
    caps->RumbleLeft = caps->RumbleRight = 0xFFFF;
    return ERROR_SUCCESS;
}

}   // extern "C"

// Player 1's Y newly pressed since the previous call (debug hooks).
bool port_player1_black_pressed()
{
    static bool was;
    bool now = g_ports[0].last.bAnalogButtons[XB_BLACK] > 0x40;
    bool edge = now && !was;
    was = now;
    return edge;
}

// Player 1's page buttons newly pressed: -1 LB / white (Q, PageUp), +1 RB / black (W, PageDown), 0 none.
int port_player1_page_pressed()
{
    static bool was_l, was_r;
    bool fg = g_d3d.hwnd && GetForegroundWindow() == g_d3d.hwnd;
    bool l = g_ports[0].last.bAnalogButtons[XB_WHITE] > 0x40 || fg && key(VK_PRIOR);
    bool r = g_ports[0].last.bAnalogButtons[XB_BLACK] > 0x40 || fg && key(VK_NEXT);
    int d = l && !was_l ? -1 : r && !was_r ? 1 : 0;
    was_l = l, was_r = r;
    return d;
}

// Player 1's left / right newly pressed (d-pad, stick, arrows): -1 left, +1 right, 0 none.
int port_player1_lr_pressed()
{
    static bool was_l, was_r;
    const XGamepad &g = g_ports[0].last;
    bool l = (g.wButtons & XB_DPAD_LEFT) || g.sThumbLX < -0x4000;
    bool r = (g.wButtons & XB_DPAD_RIGHT) || g.sThumbLX > 0x4000;
    int d = l && !was_l ? -1 : r && !was_r ? 1 : 0;
    was_l = l, was_r = r;
    return d;
}

// Player 1's up / down newly pressed (d-pad, stick, arrows): -1 up, +1 down, 0 none.
int port_player1_ud_pressed()
{
    static bool was_u, was_d;
    const XGamepad &g = g_ports[0].last;
    bool u = (g.wButtons & XB_DPAD_UP) || g.sThumbLY > 0x4000;
    bool d = (g.wButtons & XB_DPAD_DOWN) || g.sThumbLY < -0x4000;
    int r = u && !was_u ? -1 : d && !was_d ? 1 : 0;
    was_u = u, was_d = d;
    return r;
}

extern "C" {

DWORD __stdcall xapi_XInputGetState(HANDLE h, XInputState *st)
{
    Port *p = (Port *)h;
    DWORD port = (DWORD)(p - g_ports);
    XGamepad g;
    if (g_pads_override)
        memcpy(&g, g_pads[port & 3] + 4, sizeof g);
    else
        read_port(port, &g);
    if (memcmp(&g, &p->last, sizeof g)) {
        p->last = g;
        p->packet++;
    }
    st->dwPacketNumber = p->packet;
    st->Gamepad = g;
    return ERROR_SUCCESS;
}

DWORD __stdcall xapi_XInputSetState(HANDLE h, XFeedback *fb)
{
    DWORD port = (DWORD)((Port *)h - g_ports);
    int pad = port < 4 ? g_pad_of_port[port] : -1;
    if (g_sdl)
        sdl_rumble((int)port, fb->wLeftMotorSpeed, fb->wRightMotorSpeed);
    else if (g_set && pad >= 0) {
        WORD motors[2] = {fb->wLeftMotorSpeed, fb->wRightMotorSpeed};
        g_set(pad, motors);
    }
    fb->dwStatus = ERROR_SUCCESS;                       // completes immediately
    if (fb->hEvent)
        SetEvent(vh_real(fb->hEvent));
    return ERROR_IO_PENDING;
}

}
