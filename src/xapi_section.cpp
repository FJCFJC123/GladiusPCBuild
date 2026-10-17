// XBE sections: on Xbox some load on demand; in the converted exe all are always mapped. A section handle
// is the XBE section header copied into .port by xbe2pe.py (+4 virtual address, +8 virtual size).
#include "port.h"

extern "C" {

void *__stdcall xapi_XGetSectionHandleA(const char *name)
{
    const BYTE *h = (const BYTE *)g_port_info->sections;
    for (uint32_t i = 0; i < g_port_info->section_count; i++, h += 56)
        if (!_stricmp(*(const char **)(h + 0x14), name))
            return (void *)h;
    port_log("XGetSectionHandleA(%s): no such section\n", name);
    SetLastError(ERROR_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

void *__stdcall xapi_XLoadSectionByHandle(const BYTE *h) { return *(void **)(h + 4); }
BOOL __stdcall xapi_XFreeSectionByHandle(const BYTE *h) { return TRUE; }

}
