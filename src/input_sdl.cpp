// Controllers through SDL2's GameController API (SDL2.dll, loaded at runtime): Xbox, PlayStation, Switch Pro and
// anything in SDL's database share one layout. All SDL calls run on one input thread; the game reads per-player
// snapshots in XInput layout. Without SDL2.dll the caller falls back to XInput.
#include "port.h"
#include "input_sdl.h"
#include <tlhelp32.h>

typedef int(__cdecl *SDL_Init_t)(unsigned);
typedef int(__cdecl *SDL_SetHint_t)(const char *, const char *);
typedef void(__cdecl *SDL_PumpEvents_t)(void);
typedef int(__cdecl *SDL_NumJoysticks_t)(void);
typedef int(__cdecl *SDL_IsGameController_t)(int);
typedef void *(__cdecl *SDL_GameControllerOpen_t)(int);
typedef void(__cdecl *SDL_GameControllerClose_t)(void *);
typedef int(__cdecl *SDL_GameControllerGetAttached_t)(void *);
typedef unsigned char(__cdecl *SDL_GameControllerGetButton_t)(void *, int);
typedef short(__cdecl *SDL_GameControllerGetAxis_t)(void *, int);
typedef const char *(__cdecl *SDL_GameControllerName_t)(void *);
typedef const char *(__cdecl *SDL_GameControllerNameForIndex_t)(int);
typedef int(__cdecl *SDL_GameControllerRumble_t)(void *, unsigned short, unsigned short, unsigned);
typedef int(__cdecl *SDL_JoystickGetDeviceInstanceID_t)(int);
typedef void *(__cdecl *SDL_GameControllerGetJoystick_t)(void *);
typedef int(__cdecl *SDL_JoystickInstanceID_t)(void *);
typedef const char *(__cdecl *SDL_GetError_t)(void);

static struct {
    SDL_Init_t Init;
    SDL_SetHint_t SetHint;
    SDL_PumpEvents_t PumpEvents;
    SDL_NumJoysticks_t NumJoysticks;
    SDL_IsGameController_t IsGameController;
    SDL_GameControllerOpen_t Open;
    SDL_GameControllerClose_t Close;
    SDL_GameControllerGetAttached_t GetAttached;
    SDL_GameControllerGetButton_t GetButton;
    SDL_GameControllerGetAxis_t GetAxis;
    SDL_GameControllerName_t Name;
    SDL_GameControllerNameForIndex_t NameForIndex;
    SDL_GameControllerRumble_t Rumble;
    SDL_JoystickGetDeviceInstanceID_t DeviceInstanceID;
    SDL_GameControllerGetJoystick_t GetJoystick;
    SDL_JoystickInstanceID_t InstanceID;
    SDL_GetError_t GetError;
} sdl;

enum { SDL_INIT_JOYSTICK = 0x200, SDL_INIT_GAMECONTROLLER = 0x2000, SDL_INIT_EVENTS = 0x4000 };
enum { B_A, B_B, B_X, B_Y, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LSHOULDER, B_RSHOULDER, B_UP, B_DOWN,
       B_LEFT, B_RIGHT };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };

struct Player {
    void *gc;                                            // SDL_GameController*
    int instance;
    PadState state;
    WORD rumble_lo, rumble_hi;                           // requested by the game
    WORD sent_lo, sent_hi;
    DWORD rumble_tick;
    bool active;                                         // first input logged
};
static Player g_players[4];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_count;
static bool g_ok;

// Skipped devices: [input] IgnoreControllers=name;name (substrings), plus DS4Windows' virtual "Xbox 360 Controller"
// copy of a PlayStation pad while the real pad is visible ([input] SkipDS4WindowsMirror=0 keeps it).
static char g_ignore[256];
static bool g_skip_mirror = true;

// Case-insensitive substring test.
static bool name_has(const char *name, const char *sub)
{
    if (!name || !sub || !*sub)
        return false;
    size_t n = strlen(sub);
    for (const char *p = name; *p; p++)
        if (!_strnicmp(p, sub, n))
            return true;
    return false;
}

static bool is_playstation(const char *name)
{
    return name_has(name, "PS3") || name_has(name, "PS4") || name_has(name, "PS5") || name_has(name, "DualSense") ||
           name_has(name, "DualShock") || name_has(name, "Sony");
}

// Is DS4Windows running? (checked at most every 2 s)
static bool ds4windows_running()
{
    static DWORD tick;
    static bool running;
    if (tick && GetTickCount() - tick < 2000)
        return running;
    tick = GetTickCount();
    running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32 pe = {sizeof pe};
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, "DS4Windows.exe"))
            running = true;
    CloseHandle(snap);
    return running;
}

// True for devices to skip (ignore list, or a DS4Windows mirror when mirror_rule is on).
static bool ignored(const char *name, bool mirror_rule)
{
    char list[256];
    strcpy(list, g_ignore);
    for (char *t = strtok(list, ";"); t; t = strtok(nullptr, ";")) {
        while (*t == ' ')
            t++;
        if (name_has(name, t))
            return true;
    }
    return mirror_rule && name_has(name, "Xbox 360 Controller");
}

// Open newly attached controllers into free player slots, dropping skipped ones.
static void open_new()
{
    int n = sdl.NumJoysticks();
    bool sony = false;
    for (int i = 0; i < n; i++)
        sony |= sdl.IsGameController(i) && is_playstation(sdl.NameForIndex(i));
    bool mirror_rule = g_skip_mirror && sony && ds4windows_running();
    static int logged_skip[16], nlogged;
    for (int slot = 0; slot < 4; slot++) {
        Player &p = g_players[slot];
        if (p.gc && ignored(sdl.Name(p.gc), mirror_rule)) {
            port_log("input: player %d (%s) dropped - virtual copy (DS4Windows) / ignored\n", slot + 1, sdl.Name(p.gc));
            sdl.Close(p.gc);
            AcquireSRWLockExclusive(&g_lock);
            p = {};
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    for (int i = 0; i < n; i++) {
        if (!sdl.IsGameController(i))
            continue;
        int id = sdl.DeviceInstanceID(i);
        bool known = false;
        for (Player &p : g_players)
            known |= p.gc && p.instance == id;
        if (known)
            continue;
        const char *name = sdl.NameForIndex(i);
        if (ignored(name, mirror_rule)) {
            bool seen = false;
            for (int k = 0; k < nlogged; k++)
                seen |= logged_skip[k] == id;
            if (!seen && nlogged < 16) {
                logged_skip[nlogged++] = id;
                port_log("input: skipping %s - %s\n", name ? name : "controller",
                         mirror_rule ? "virtual copy of a PlayStation pad (DS4Windows)" : "[input] IgnoreControllers");
            }
            continue;
        }
        for (int slot = 0; slot < 4; slot++) {
            Player &p = g_players[slot];
            if (p.gc)
                continue;
            void *gc = sdl.Open(i);
            if (!gc)
                break;
            AcquireSRWLockExclusive(&g_lock);
            p = {};
            p.gc = gc;
            p.instance = sdl.InstanceID(sdl.GetJoystick(gc));
            ReleaseSRWLockExclusive(&g_lock);
            port_log("input: player %d = %s (SDL)\n", slot + 1, sdl.Name(gc) ? sdl.Name(gc) : "controller");
            break;
        }
    }
}

static short neg(short v) { return v == -32768 ? 32767 : (short)-v; }

// Read every open controller into its player snapshot and send rumble.
static void poll()
{
    LONG count = 0;
    for (int slot = 0; slot < 4; slot++) {
        Player &p = g_players[slot];
        if (!p.gc)
            continue;
        if (!sdl.GetAttached(p.gc)) {
            port_log("input: player %d disconnected\n", slot + 1);
            sdl.Close(p.gc);
            AcquireSRWLockExclusive(&g_lock);
            p = {};
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        PadState s = {};
        static const struct { int b; WORD bit; } map[] = {
            {B_UP, 0x0001}, {B_DOWN, 0x0002}, {B_LEFT, 0x0004}, {B_RIGHT, 0x0008}, {B_START, 0x0010},
            {B_BACK, 0x0020}, {B_LSTICK, 0x0040}, {B_RSTICK, 0x0080}, {B_LSHOULDER, 0x0100},
            {B_RSHOULDER, 0x0200}, {B_A, 0x1000}, {B_B, 0x2000}, {B_X, 0x4000}, {B_Y, 0x8000}};
        for (auto &m : map)
            if (sdl.GetButton(p.gc, m.b))
                s.wButtons |= m.bit;
        s.sThumbLX = sdl.GetAxis(p.gc, AX_LX);
        s.sThumbLY = neg(sdl.GetAxis(p.gc, AX_LY));      // SDL: +Y = down, XInput/Xbox: +Y = up
        s.sThumbRX = sdl.GetAxis(p.gc, AX_RX);
        s.sThumbRY = neg(sdl.GetAxis(p.gc, AX_RY));
        int lt = sdl.GetAxis(p.gc, AX_LT), rt = sdl.GetAxis(p.gc, AX_RT);   // 0..32767
        s.bLeftTrigger = (BYTE)(lt > 0 ? lt * 255 / 32767 : 0);
        s.bRightTrigger = (BYTE)(rt > 0 ? rt * 255 / 32767 : 0);
        AcquireSRWLockExclusive(&g_lock);
        p.state = s;
        if (!p.active && (s.wButtons || s.bLeftTrigger > 64 || s.bRightTrigger > 64)) {
            p.active = true;
            port_log("input: player %d (%s) first input - buttons %04X\n", slot + 1, sdl.Name(p.gc), s.wButtons);
        }
        WORD lo = p.rumble_lo, hi = p.rumble_hi;
        ReleaseSRWLockExclusive(&g_lock);
        // rumble: resend on change and keep a running effect alive (SDL effects expire)
        DWORD now = GetTickCount();
        if (sdl.Rumble && (lo != p.sent_lo || hi != p.sent_hi || ((lo || hi) && now - p.rumble_tick > 400))) {
            sdl.Rumble(p.gc, lo, hi, (lo || hi) ? 500 : 0);
            p.sent_lo = lo, p.sent_hi = hi, p.rumble_tick = now;
        }
        count = slot + 1 > count ? slot + 1 : count;
    }
    InterlockedExchange(&g_count, count);
}

// Input thread: SDL init, then pump events, hot-plug and poll at ~250 Hz.
static DWORD WINAPI input_thread(void *)
{
    // before SDL_Init: read pads in the background and enable the opt-in PlayStation drivers
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    GetPrivateProfileStringA("input", "IgnoreControllers", "", g_ignore, sizeof g_ignore, ini);
    g_skip_mirror = GetPrivateProfileIntA("input", "SkipDS4WindowsMirror", 1, ini) != 0;
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl.SetHint("SDL_JOYSTICK_HIDAPI_PS3", "1");
    // Xbox pads through XInput / WGI: Steam's Xbox driver claims the HID interface, and a HID-opened pad never reports input
    sdl.SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "0");
    sdl.SetHint("SDL_JOYSTICK_RAWINPUT", "0");
    sdl.SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    sdl.SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    if (sdl.Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        port_log("input: SDL_Init failed (%s) - using XInput\n", sdl.GetError());
        g_ok = false;
        return 0;
    }
    port_log("input: SDL2 game controllers (Xbox, PlayStation, Switch Pro, ...)\n");
    DWORD last_scan = 0;
    for (;;) {
        sdl.PumpEvents();
        DWORD now = GetTickCount();
        if (now - last_scan > 250) {
            open_new();
            last_scan = now;
        }
        poll();
        Sleep(4);                                        // ~250 Hz
    }
}

// Load SDL2.dll and start the input thread; false = use XInput.
bool sdl_input_start()
{
    static bool tried;
    if (tried)
        return g_ok;
    tried = true;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, 0x5C) + 1, "SDL2.dll");
    HMODULE m = LoadLibraryA(path);
    if (!m)
        return false;
#define LOAD(field, name) sdl.field = (decltype(sdl.field))GetProcAddress(m, name)
    LOAD(Init, "SDL_Init");
    LOAD(SetHint, "SDL_SetHint");
    LOAD(PumpEvents, "SDL_PumpEvents");
    LOAD(NumJoysticks, "SDL_NumJoysticks");
    LOAD(IsGameController, "SDL_IsGameController");
    LOAD(Open, "SDL_GameControllerOpen");
    LOAD(Close, "SDL_GameControllerClose");
    LOAD(GetAttached, "SDL_GameControllerGetAttached");
    LOAD(GetButton, "SDL_GameControllerGetButton");
    LOAD(GetAxis, "SDL_GameControllerGetAxis");
    LOAD(Name, "SDL_GameControllerName");
    LOAD(NameForIndex, "SDL_GameControllerNameForIndex");
    LOAD(Rumble, "SDL_GameControllerRumble");
    LOAD(DeviceInstanceID, "SDL_JoystickGetDeviceInstanceID");
    LOAD(GetJoystick, "SDL_GameControllerGetJoystick");
    LOAD(InstanceID, "SDL_JoystickInstanceID");
    LOAD(GetError, "SDL_GetError");
#undef LOAD
    if (!sdl.Init || !sdl.SetHint || !sdl.PumpEvents || !sdl.NumJoysticks || !sdl.IsGameController || !sdl.Open ||
        !sdl.Close || !sdl.GetAttached || !sdl.GetButton || !sdl.GetAxis || !sdl.DeviceInstanceID ||
        !sdl.GetJoystick || !sdl.InstanceID || !sdl.GetError || !sdl.Name || !sdl.NameForIndex) {
        port_log("input: SDL2.dll is missing GameController functions - using XInput\n");
        return false;
    }
    g_ok = true;
    CloseHandle(CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr));
    return true;
}

int sdl_player_count() { return g_ok ? (int)g_count : 0; }

bool sdl_read(int player, PadState *out)
{
    if (!g_ok || player < 0 || player > 3)
        return false;
    AcquireSRWLockShared(&g_lock);
    bool ok = g_players[player].gc != nullptr;
    if (ok)
        *out = g_players[player].state;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

void sdl_rumble(int player, WORD lo, WORD hi)
{
    if (!g_ok || player < 0 || player > 3)
        return;
    AcquireSRWLockExclusive(&g_lock);
    g_players[player].rumble_lo = lo;
    g_players[player].rumble_hi = hi;
    ReleaseSRWLockExclusive(&g_lock);
}
