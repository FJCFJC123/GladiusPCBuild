// Virtual handles for game-visible Windows objects (handles.cpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

static const uintptr_t VH_BASE = 0x40000000;
enum { VH_FREE, VH_EVENT, VH_SEMAPHORE, VH_MUTEX, VH_THREAD, VH_FILE };

bool vh_is(HANDLE h);
// wrap a real handle the game is about to receive (returns `real` unchanged if it is null / invalid)
HANDLE vh_new(uint32_t kind, HANDLE real, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, const char *path = nullptr);
HANDLE vh_real(HANDLE h);                       // virtual -> real; anything else unchanged
uint32_t vh_kind(HANDLE h);
BOOL vh_close(HANDLE h);                        // CloseHandle for virtual and real handles
DWORD vh_wait(HANDLE h, DWORD ms, BOOL alertable);   // rebind-aware waits (wake when the table is rebuilt)
DWORD vh_wait_multi(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable);
LONG vh_epoch();                                // table rebuild counter
void vh_mark_epoch();                           // this thread is leaving game code: waits after a rebuild return at once
void vh_publish();                              // before a state sync: object states into the shared table
void vh_reconcile();                            // after a state sync: real objects for the host's table

void port_note_overlapped_handle(HANDLE real);  // xapi_file.cpp: reopened FILE_FLAG_OVERLAPPED file
bool port_map_path(const char *xbox, char *out); // xapi_file.cpp: Xbox path -> this machine's PC path
// xapi_file.cpp: the game's real open of an Xbox path (mapping, z:\\gladius.bec -> disc file, overlapped set)
extern "C" HANDLE port_open_file(const char *name, DWORD access, DWORD share, DWORD disp, DWORD flags, char *mapped);
