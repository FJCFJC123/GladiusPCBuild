// Dog script VM fixes for frame rates above the Xbox's 60.
//
// The VM's variant multiply (0x191810, used by Op_MULTIPLY) takes the result type from the LEFT operand: an int on the
// left multiplies in float and then truncates the product with _ftol (0x273944). Scripts scale per-frame steps with
// `k * gb.mTimeDelta` (e.g. scene04v_battle.scb: `angle -= 90 * gb.mTimeDelta`); at 60 fps that is int(1.5) = 1 a
// frame, above 90 fps it is int(< 1) = 0 and such loops never finish (the Valens free-spin camera). The fix keeps the
// product as a float when the left operand is an int and the right one is this frame's delta (time_Delta,
// 0x49C218, which gb.mTimeDelta returns), so every time-scaled step moves at its intended speed at any frame rate;
// every other int * float keeps the retail truncation.
//
// [debug] DogTimeFix=0 disables it (A/B). [debug] ScriptDeltaFps=<fps> (debug builds) forces the engine's frame delta
// to 1/fps through its own time override (0x49C2B4 enable / 0x49C2B0 value), so a 60 Hz monitor reproduces a
// high-refresh panel's script timing (the game then runs in slow or fast motion).
#include "port.h"
#include <stdint.h>
#include <string.h>

namespace {

struct Variant { uint32_t type, v; };          // low byte = type (2 int, 3 float, 0x10 reference), 0x300 = indirect

const uint32_t MUL_VA = 0x191810, MUL_RESUME = 0x191815;
// time_Update (0x18EC20): RealDelta 0x49C214 = the measured frame time; TimeDelta 0x49C218 = the game's delta (the
// override if on, else RealDelta, x the time scale 0x37FA90, clamped to 0x33BB90) = what gb.mTimeDelta returns
const uint32_t REAL_DELTA = 0x49C214, TIME_DELTA = 0x49C218, TIME_OVERRIDE_ON = 0x49C2B4, TIME_OVERRIDE_DT = 0x49C2B0;

typedef Variant *(__fastcall *MulFn)(Variant *a, void *, Variant *out, Variant *b);   // = thiscall(a; out, b), ret 8
typedef Variant *(__fastcall *DerefFn)(void *ref, void *);                              // 0x18FA90 (thiscall)
typedef float (__fastcall *GetFloatFn)(Variant *v, void *);                             // 0x18F830 (thiscall, st0)

MulFn g_mulOrig;                               // the trampoline: the 5 displaced bytes, then jmp 0x191815
int g_fixes;
float g_forcedDt;

Variant *resolve(Variant *x)                   // the original's first step: a 0x10 reference names another variant
{
    if ((x->type & 0xFF) == 0x10) return ((DerefFn)0x18FA90)((void *)(x->v + 4), nullptr);
    return x;
}

Variant *__fastcall mul_hook(Variant *a, void *, Variant *out, Variant *b)
{
    Variant *ra = resolve(a), *rb = resolve(b);
    if ((ra->type & 0xFF) == 2 && (rb->type & 0xFF) == 3) {
        float fb = ((GetFloatFn)0x18F830)(rb, nullptr);
        float dt = *(volatile float *)TIME_DELTA, rdt = *(volatile float *)REAL_DELTA;
        if (fb > 0.0f && (fb == dt || fb == rdt)) {
            float fa = ((GetFloatFn)0x18F830)(ra, nullptr);
            out->type = 3;                         // what the float-left path writes (0x19186B)
            float r = fa * fb;
            memcpy(&out->v, &r, 4);
            if (g_fixes++ == 0) port_log("dog: int * gb.mTimeDelta kept as float (first: %g * %g = %g; retail gives %d)\n",
                                         fa, fb, r, (int)r);
            return out;
        }
        static bool noted;                         // a small fraction that is NOT this frame's delta: say so once
        if (!noted && fb > 0.0f && fb < 0.1f) {
            noted = true;
            port_log("dog: int * %g (frame delta %g) left to the retail truncation\n", fb, dt);
        }
    }
    return g_mulOrig(a, nullptr, out, b);
}

bool expect(uint32_t va, const uint8_t *bytes, int n)
{
    if (memcmp((const void *)va, bytes, n) == 0) return true;
    port_log("dog: unexpected bytes at %08X - script timing fix not installed\n", va);
    return false;
}

}  // namespace

// Called once a frame (d3d_present): keeps the debug delta override applied (the engine resets its time state).
void port_dog_frame()
{
    if (g_forcedDt > 0.0f) {
        *(volatile float *)TIME_OVERRIDE_DT = g_forcedDt;
        *(volatile uint32_t *)TIME_OVERRIDE_ON = 1;
    }
}

void port_dog_fixes_init(const char *ini)
{
#if !PORT_RELEASE
    int fps = GetPrivateProfileIntA("debug", "ScriptDeltaFps", 0, ini);
    if (fps > 0) {
        g_forcedDt = 1.0f / (float)fps;
        port_log("dog: debug frame delta forced to 1/%d s (ScriptDeltaFps)\n", fps);
    }
#endif
    if (!GetPrivateProfileIntA("debug", "DogTimeFix", 1, ini)) {
        port_log("dog: script timing fix off (DogTimeFix=0)\n");
        return;
    }
    static const uint8_t k_head[5] = {0x51, 0x80, 0x39, 0x10, 0x56};   // push ecx; cmp byte [ecx],10h; push esi
    if (!expect(MUL_VA, k_head, 5)) return;
    uint8_t *t = (uint8_t *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, k_head, 5);
    t[5] = 0xE9;
    *(int32_t *)(t + 6) = (int32_t)(MUL_RESUME - ((uint32_t)t + 10));
    g_mulOrig = (MulFn)t;
    DWORD old;
    VirtualProtect((void *)MUL_VA, 5, PAGE_EXECUTE_READWRITE, &old);
    uint8_t *p = (uint8_t *)MUL_VA;
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((uint32_t)mul_hook - (MUL_VA + 5));
    VirtualProtect((void *)MUL_VA, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)MUL_VA, 5);
    port_log("dog: script timing fix installed (int * gb.mTimeDelta keeps its fraction)\n");
}
