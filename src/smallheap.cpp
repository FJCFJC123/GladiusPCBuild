// SmallHeap overflow. The engine gives most subsystems their own SmallHeap (ctor 0x177F00: name + byte size), a block
// allocator over one fixed array whose block sizes and offsets are 15-bit counts of 4-byte units, so no heap can pass
// ~128 KB. A full heap hands out NULL ("SmallHeap->Alloc(%d) failed (%s)") and callers don't check: past 1833 skills
// SkillDataFFHeap stops taking name-table nodes and the Learn Skill screen crashes on the lookup. Like the GC MEM2
// fallback, a failed Alloc (0x1759C0) is served from the game heap instead, and Free (0x176360) sends any pointer
// outside the heap's own array back there. Those two are the only functions that touch SmallHeap blocks.
#include "port.h"
#include "container.h"
#include <string.h>

static const DWORD k_alloc = 0x1759C0, k_free = 0x176360;
static const DWORD k_overflow_tag = 0x0F10F10F;          // header word in front of an overflow block

bool port_smallheap_installed;
void(__cdecl *port_pool_free_notify)(uint32_t p);       // pkgdiag.cpp: sees every Free (it used to patch Free itself)

static volatile LONG g_overflow_blocks;
static const void *g_logged[128];                        // heaps already reported (log once each)
static volatile LONG g_logged_n;

// original entries: the instructions the jmp replaced, then back into the function
static __declspec(naked) void alloc_orig()
{
    __asm {
        sub esp, 0x20C
        mov eax, dword ptr ds:[0x3891F0]
        push 0x1759CB
        ret
    }
}
static __declspec(naked) void free_orig()
{
    __asm {
        mov edx, [esp + 4]
        push ebx
        push ebp
        push 0x176366
        ret
    }
}

typedef void *(__fastcall *AllocFn)(void *heap, void *edx, DWORD size);
typedef void(__fastcall *FreeFn)(void *heap, void *edx, void *p);

static void note_overflow(void *heap, DWORD size)
{
    LONG n = InterlockedIncrement(&g_overflow_blocks);
    for (LONG i = 0; i < g_logged_n; i++)
        if (g_logged[i] == heap) return;
    LONG slot = InterlockedIncrement(&g_logged_n) - 1;
    if (slot < (LONG)(sizeof g_logged / sizeof g_logged[0])) g_logged[slot] = heap;
    const char *name = *(const char **)((BYTE *)heap + 0x70);
    port_log("smallheap: %.16s full (%u bytes) - growing into the game heap (alloc %u, %ld overflow blocks so far)\n",
             name ? name : "?", *(DWORD *)((BYTE *)heap + 0x6C) * 4, size, n);
}

// thiscall (ecx = heap, size on the stack, ret 4) == fastcall with an unused edx
static void *__fastcall alloc_hook(void *heap, void *edx, DWORD size)
{
    if (void *p = ((AllocFn)(void *)alloc_orig)(heap, edx, size)) return p;
    DWORD *b = (DWORD *)(container_on() ? ctr_alloc(size + 4, false)
                                        : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 4));
    if (!b) return nullptr;
    b[0] = k_overflow_tag;
    note_overflow(heap, size);
    return b + 1;
}

static void __fastcall free_hook(void *heap, void *edx, void *p)
{
    if (port_pool_free_notify) port_pool_free_notify((uint32_t)(uintptr_t)p);
    BYTE *base = *(BYTE **)((BYTE *)heap + 0x64), *end = base + *(DWORD *)((BYTE *)heap + 0x6C) * 4;
    if (p && ((BYTE *)p < base || (BYTE *)p >= end)) {
        DWORD *b = (DWORD *)p - 1;
        if (*b != k_overflow_tag) {
            port_log("smallheap: free of %08X outside heap %08X-%08X with no overflow tag - ignored\n",
                     (DWORD)(uintptr_t)p, (DWORD)(uintptr_t)base, (DWORD)(uintptr_t)end);
            return;
        }
        *b = 0;
        game_free(b);
        InterlockedDecrement(&g_overflow_blocks);
        return;
    }
    ((FreeFn)(void *)free_orig)(heap, edx, p);
}

static bool hook(DWORD at, void *to, const BYTE *expect, int n)
{
    if (memcmp((void *)at, expect, n)) return false;
    BYTE *p = (BYTE *)at;
    p[0] = 0xE9;
    *(int32_t *)(p + 1) = (int32_t)((DWORD)(uintptr_t)to - (at + 5));
    return true;
}

extern "C" void port_smallheap_init()
{
    static const BYTE alloc_head[11] = {0x81, 0xEC, 0x0C, 0x02, 0x00, 0x00, 0xA1, 0xF0, 0x91, 0x38, 0x00};
    static const BYTE free_head[6] = {0x8B, 0x54, 0x24, 0x04, 0x53, 0x55};
    if (memcmp((void *)k_alloc, alloc_head, sizeof alloc_head) || memcmp((void *)k_free, free_head, sizeof free_head)) {
        port_log("smallheap: unexpected Alloc/Free bytes - overflow not installed\n");
        return;
    }
    hook(k_free, (void *)free_hook, free_head, sizeof free_head);
    hook(k_alloc, (void *)alloc_hook, alloc_head, sizeof alloc_head);
    port_smallheap_installed = true;
    port_log("smallheap: overflow into the game heap installed\n");
}
