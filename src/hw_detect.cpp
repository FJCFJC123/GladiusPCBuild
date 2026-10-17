// Hardware detection for the Video settings presets (options_video.cpp): the strongest GPU (DXGI, hardware adapters
// only - most dedicated video memory), CPU cores and the primary monitor, and the preset tier they suggest.
#include "port.h"
#include "hw_detect.h"
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

static HwInfo g_hw;
static bool g_done;

// Video memory from the display-class registry key of the adapter with this name (64-bit qwMemorySize). DXGI's
// DedicatedVideoMemory is a SIZE_T - 32 bits in this 32-bit process - so 11 GB read as 3 GB (2026-10-10).
static UINT registry_vram_mb(const char *name)
{
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &root) != ERROR_SUCCESS)
        return 0;
    UINT best = 0;
    char sub[16];
    for (DWORD i = 0; RegEnumKeyA(root, i, sub, sizeof sub) == ERROR_SUCCESS; i++) {
        HKEY k;
        if (RegOpenKeyExA(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;
        char desc[128] = {};
        DWORD len = sizeof desc - 1;
        if (RegQueryValueExA(k, "DriverDesc", nullptr, nullptr, (BYTE *)desc, &len) == ERROR_SUCCESS &&
            !_stricmp(desc, name)) {
            unsigned long long q = 0;
            DWORD ql = sizeof q;
            if (RegQueryValueExA(k, "HardwareInformation.qwMemorySize", nullptr, nullptr, (BYTE *)&q, &ql) != ERROR_SUCCESS) {
                DWORD d = 0;
                ql = sizeof d;
                if (RegQueryValueExA(k, "HardwareInformation.MemorySize", nullptr, nullptr, (BYTE *)&d, &ql) == ERROR_SUCCESS)
                    q = d;
            }
            if ((UINT)(q >> 20) > best)
                best = (UINT)(q >> 20);
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return best;
}

static int physical_cores()
{
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (!len)
        return 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(len);
    int n = 0;
    if (buf && GetLogicalProcessorInformation(buf, &len))
        for (DWORD i = 0; i < len / sizeof *buf; i++)
            n += buf[i].Relationship == RelationProcessorCore;
    free(buf);
    return n;
}

const HwInfo &port_hw_info()
{
    if (g_done)
        return g_hw;
    g_done = true;
    HwInfo &h = g_hw;
    strcpy(h.gpu, "unknown GPU");
    IDXGIFactory1 *f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)) && f) {
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
                UINT mb = (UINT)(d.DedicatedVideoMemory >> 20);
                if (!h.vendor || mb > h.vram_mb) {
                    h.vendor = d.VendorId;
                    h.vram_mb = mb;
                    WideCharToMultiByte(CP_ACP, 0, d.Description, -1, h.gpu, sizeof h.gpu, nullptr, nullptr);
                }
            }
            a->Release();
        }
        f->Release();
    }
    if (UINT mb = registry_vram_mb(h.gpu))
        if (mb > h.vram_mb)
            h.vram_mb = mb;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    h.threads = (int)si.dwNumberOfProcessors;
    h.cores = physical_cores();
    if (!h.cores)
        h.cores = h.threads;
    h.screen_w = GetSystemMetrics(SM_CXSCREEN);
    h.screen_h = GetSystemMetrics(SM_CYSCREEN);
    // Tier: video memory is the best single signal for what this renderer costs (shadow maps, the 2x-tall scene,
    // HDR targets). Integrated Intel graphics and anything under 2 GB: Low; under 6 GB: Medium; else High. Fewer
    // than 4 cores caps it at Medium (shadow casting costs CPU on the game core).
    bool integrated = h.vendor == 0x8086 || h.vram_mb < 1024;
    h.tier = integrated || h.vram_mb < 2048 ? HW_LOW : h.vram_mb < 6144 ? HW_MEDIUM : HW_HIGH;
    if (h.cores < 4 && h.tier == HW_HIGH)
        h.tier = HW_MEDIUM;
    static const char *names[] = {"Low", "Medium", "High"};
    port_log("hardware: %s (vendor %04X, %u MB video memory), %d cores / %d threads, screen %dx%d -> %s preset\n",
             h.gpu, h.vendor, h.vram_mb, h.cores, h.threads, h.screen_w, h.screen_h, names[h.tier]);
    return h;
}
