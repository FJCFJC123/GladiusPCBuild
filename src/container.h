// Memory container: game memory at fixed addresses, identical on every PC.
//   main  0x20000000 192 MB   game heap (CRT malloc / XAPI heap) - main thread and everything else
//   io    0x2C000000  16 MB   game heap blocks allocated on the stream reader / cache writer threads
//   local 0x2D000000  32 MB   blocks allocated by machine-local code (sound / CRI / I-O): never synced
//   phys  0xA0000000 512 MB   "physical" (XPhysicalAlloc, XMemAlloc, D3D resource data); >= 0x80000000 because
//                             the game rebuilds pointers with Data | 0x80000000
//   lphys 0xC0000000  64 MB   the same for machine-local "physical" memory
// (sizes: the table in container.cpp is the only copy)
// Ranges are reserved in DllMain (before any game code); [mem] Container=1 turns the allocator on.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <utility>

enum { CTR_MAIN, CTR_IO, CTR_PHYS, CTR_LOCAL, CTR_LOCAL_PHYS, CTR_ARENAS };

bool container_on();
void container_init(const char *ini);           // port_start: commit + create the arenas if enabled
void container_set_io_thread();                 // this thread allocates from the io arena
void *ctr_alloc(size_t size, bool zero);        // heap block: main or io arena by thread
void *ctr_phys_alloc(size_t size, size_t align);
bool ctr_owns(const void *p);
size_t ctr_size(const void *p);
void ctr_free(void *p);
void *ctr_realloc(void *p, size_t size, bool zero, bool in_place_only);   // shrinks always stay in place
void container_report();                        // usage per arena (log)
// an arena's whole reserved range; false if the container is off
bool container_arena(int arena, uintptr_t *base, size_t *size);
// page probe: the range of an arena that has ever held blocks ([base, high-water mark)); false if off
bool container_range(int arena, uintptr_t *base, uintptr_t *end);
// [mem] AllocTags=1: visit every live block with its size and allocating game stack
static const int CTR_TAG_STACK = 12;
int container_local_blocks_n(uint32_t *pairs, int max);   // sorted (addr, size) pairs; returns the total
void container_tags(void (*cb)(uintptr_t addr, uint32_t size, const uint32_t *stack, void *ctx), void *ctx);
void *container_shared_page();                  // 0x20001000: shim state that is game state (synced)
void *container_fake_heap();                    // game-visible process heap handle (same on every PC)

// shim objects the game holds pointers to: main arena when the container is on, else the process heap
void *game_alloc(size_t size, bool zero = true);
void *game_alloc_local(size_t size);            // same, but machine-local (excluded from state syncs)
void game_free(void *p);
// operator new/delete for shim classes whose instances the game points at
#define GAME_ALLOCATED_LOCAL static void *operator new(size_t n) { return game_alloc_local(n); } static void operator delete(void *p) { game_free(p); }
#define GAME_ALLOCATED static void *operator new(size_t n) { return game_alloc(n, true); } static void operator delete(void *p) { game_free(p); }
