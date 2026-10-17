// XAPI memory -> Windows. Xbox "physical"/contiguous memory (GPU/APU-visible) is ordinary memory on PC:
// D3D8/DSOUND are replaced, so nothing needs physical addresses. Virtual* pass through.
#include "d3d.h"
#include "memstats.h"
#include "container.h"

static SIZE_T region_size(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof mbi) ? mbi.RegionSize : 0;
}

// "Physical" memory is allocated top-down in the large-address range (>= 0x80000000): the game turns
// resource Data fields back into pointers with Data | 0x80000000, which is then a no-op. Page granular.
// xphys_alloc_port: memory only the port sizes (back buffer / depth = this PC's window) - never in the container.
void *xphys_alloc_port(SIZE_T size)
{
    void *p = VirtualAlloc(nullptr, size ? size : 1, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    if (p && (uintptr_t)p < 0x80000000u) {
        static bool warned;
        if (!warned) {
            warned = true;
            port_log("WARNING: physical allocation below 2 GB (%p) - large-address space unavailable?\n", p);
        }
    }
    return p;
}

// game memory (XPhysicalAlloc, D3D resource data): the container's phys arena (0xA0000000, >= 2 GB as required)
// when it is on - zero-filled like the fresh VirtualAlloc pages it replaces
static void *phys_alloc_aligned(SIZE_T size, SIZE_T align)
{
    if (container_on()) {
        if (void *p = ctr_phys_alloc(size, align)) {
            memset(p, 0, size);
            return p;
        }
    }
    return xphys_alloc_port(size);                      // VirtualAlloc: 64 KB aligned
}

void *xphys_alloc(SIZE_T size) { return phys_alloc_aligned(size, 4096); }

static SIZE_T phys_size(void *p) { return ctr_owns(p) ? ctr_size(p) : region_size(p); }

void xphys_free(void *p)
{
    if (ctr_owns(p))
        ctr_free(p);
    else if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

static void *phys_alloc(SIZE_T size, SIZE_T align)
{
    void *p = phys_alloc_aligned(size, align);
    memstats_alloc(MS_PHYS, p, (size + 0xFFF) & ~(SIZE_T)0xFFF);
    return p;
}

extern "C" {

// PVOID XPhysicalAlloc(SIZE_T size, ULONG_PTR max_address, ULONG_PTR alignment, DWORD protect)
void *__stdcall xapi_XPhysicalAlloc(SIZE_T size, ULONG_PTR max_address, ULONG_PTR alignment, DWORD protect)
{
    return phys_alloc(size, alignment);
}

// PVOID MmAllocateContiguousMemoryEx-style (size, lowest, highest, alignment, protect)
void *__stdcall xapi_XPhysicalAllocEx(SIZE_T size, ULONG_PTR lowest, ULONG_PTR highest, ULONG_PTR alignment, DWORD protect)
{
    return phys_alloc(size, alignment);
}

SIZE_T __stdcall xapi_XPhysicalSize(void *p) { return p ? phys_size(p) : 0; }

void __stdcall xapi_XPhysicalProtect(void *p, SIZE_T size, DWORD protect)
{
    // write-combine / no-cache attributes only matter to the GPU on Xbox
}

void __stdcall xapi_XPhysicalFree(void *p)
{
    if (p && memstats_enabled())
        memstats_free(MS_PHYS, phys_size(p));
    xphys_free(p);
}

static const DWORD XBOX_MEM_NOZERO = 0x800000;

void *__stdcall xapi_VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect)
{
    void *p = VirtualAlloc(addr, size, type & ~XBOX_MEM_NOZERO, protect & 0xFF ? protect & 0x7FF : PAGE_READWRITE);
    if (!addr)                                          // new region (commits inside a reserved one are not new)
        memstats_alloc(MS_VIRTUAL, p, (size + 0xFFF) & ~(SIZE_T)0xFFF);
    return p;
}
BOOL __stdcall xapi_VirtualFree(void *addr, SIZE_T size, DWORD type)
{
    if (addr && (type & MEM_RELEASE) && memstats_enabled())
        memstats_free(MS_VIRTUAL, region_size(addr));
    return VirtualFree(addr, size, type);
}
BOOL __stdcall xapi_VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old)
{
    DWORD dummy;
    return VirtualProtect(addr, size, protect & 0x7FF, old ? old : &dummy);
}
SIZE_T __stdcall xapi_VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len)
{
    return VirtualQuery(addr, mbi, len);
}

// Real machine numbers (no app code calls this; only Xbox libraries do).
void __stdcall xapi_GlobalMemoryStatus(MEMORYSTATUS *ms)
{
    ms->dwLength = sizeof *ms;
    GlobalMemoryStatus(ms);
}

}
