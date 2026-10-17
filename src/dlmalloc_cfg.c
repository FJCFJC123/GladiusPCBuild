/* dlmalloc (third_party/dlmalloc/malloc.c, Doug Lea, MIT) built for the memory container (container.cpp):
   mspaces only, each living in one fixed reserved range. No randomisation; with the same call sequence the same
   addresses come out. HAVE_MMAP is forced on for WIN32 inside malloc.c - container.cpp switches it off at run time
   (mmap threshold = max, footprint limit = capacity) so a block can never land outside its range. */
#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define FOOTERS 0
#define MALLOC_FAILURE_ACTION
#define DLMALLOC_EXPORT extern
#pragma warning(disable : 4267 4244 4146 4018 4334)
/* The mspace header (copied between machines by the netplay state sync) holds dlmalloc's "magic", which it
   checks on every call and normally derives from GetTickCount() - it must be the same on every PC. windows.h is
   included first so the macro below only affects malloc.c's single use. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GetTickCount() ((DWORD)0x47504E31)
#include "../third_party/dlmalloc/malloc.c"
