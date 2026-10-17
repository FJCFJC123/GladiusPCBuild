// [debug] MemStats: allocation counters per allocator kind and thread role (memstats.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { MS_HEAP, MS_PRIVHEAP, MS_PHYS, MS_VIRTUAL, MS_KINDS };
enum { MS_OTHER, MS_MAIN, MS_CRI, MS_IO, MS_ROLES };

void memstats_init(const char *ini);
bool memstats_enabled();
void memstats_set_role(int role);                // this thread's role (default MS_OTHER)
int memstats_role_for_thread(uint32_t start);    // role of a game thread by entry point
void memstats_alloc(int kind, const void *p, size_t size);
void memstats_free(int kind, size_t size);
void memstats_tick();                            // watchdog: report every 60 s

// [debug] HeapCheck=1: live set of game process-heap blocks + ring of recent frees with the freeing game stack;
// the engine's tracked free (0x176A30) refuses blocks that are already free and logs both stacks.
bool heapcheck_enabled();
void heapcheck_alloc(const void *p);
void heapcheck_free(const void *p);
void heapcheck_install();

// helpers: game return addresses on a stack (image .text, preceded by a call), formatted " XXXXXX ..."
void memstats_game_stack(uint32_t *out, int n, const void *from);
void memstats_fmt_stack(char *o, int n, const uint32_t *s, int count);

// [debug] PageHeap=1 (xapi_heap.cpp): game blocks on their own pages, freed pages decommitted
void pageheap_init(const char *ini);
void pageheap_describe(uintptr_t addr);          // crash log: which game block a fault address hits
