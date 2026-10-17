// XAPI save-game support -> Windows. Saves are plain files under SaveDir\U (xapi_file.cpp maps U:/T:/Z:), so
// what remains is the console-only plumbing around them: disk geometry queries, content signatures and memory
// units (a PC has none - only the "hard disk" exists).
#include "port.h"
#include "container.h"
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

extern "C" {

// XGetDiskClusterSize / XGetDiskSectorSize(root): the Xbox hard disk's FATX geometry (16 KB clusters, 512-byte
// sectors) - save-size accounting ("blocks") is expressed in these units.
DWORD __stdcall xapi_XGetDiskClusterSize(const char *root) { return 16384; }
DWORD __stdcall xapi_XGetDiskSectorSize(const char *root) { return 512; }

// XCalculateSignatureBegin/Update/End: the console keys an HMAC-SHA1 with the per-box / title signature key.
// Saves only ever need to verify against signatures this same build wrote, so a plain SHA-1 is equivalent.
struct SigCtx {
    GAME_ALLOCATED_LOCAL                                     // the handle value is game-visible (memory container) - machine-local state
    BCRYPT_HASH_HANDLE hash;
};

HANDLE __stdcall xapi_XCalculateSignatureBegin(DWORD flags)
{
    static BCRYPT_ALG_HANDLE alg;
    if (!alg && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return INVALID_HANDLE_VALUE;
    SigCtx *c = new SigCtx();
    if (BCryptCreateHash(alg, &c->hash, nullptr, 0, nullptr, 0, 0) != 0) {
        delete c;
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)c;
}

DWORD __stdcall xapi_XCalculateSignatureUpdate(HANDLE h, const BYTE *data, DWORD len)
{
    if (h == INVALID_HANDLE_VALUE || !h)
        return ERROR_INVALID_HANDLE;
    BCryptHashData(((SigCtx *)h)->hash, (PUCHAR)data, len, 0);
    return ERROR_SUCCESS;
}

DWORD __stdcall xapi_XCalculateSignatureEnd(HANDLE h, BYTE *sig)
{
    if (h == INVALID_HANDLE_VALUE || !h)
        return ERROR_INVALID_HANDLE;
    SigCtx *c = (SigCtx *)h;
    BYTE out[20] = {};
    BCryptFinishHash(c->hash, out, sizeof out, 0);
    BCryptDestroyHash(c->hash);
    delete c;
    if (sig)
        memcpy(sig, out, sizeof out);                   // XCALCSIG_SIGNATURE = 20 bytes
    return ERROR_SUCCESS;
}

// Memory units: never present.
DWORD __stdcall xapi_XMUNameFromDriveLetter(char drive, wchar_t *name, UINT cch)
{
    if (name && cch)
        name[0] = 0;
    return ERROR_INVALID_DRIVE;
}
DWORD __stdcall xapi_XMountMUA(DWORD port, DWORD slot, char *drive) { return ERROR_DEVICE_NOT_CONNECTED; }
DWORD __stdcall xapi_XUnmountMU(DWORD port, DWORD slot) { return ERROR_SUCCESS; }

// Z: utility (cache) partition: always available as SaveDir\Z.
BOOL __stdcall xapi_XMountUtilityDrive(BOOL format) { return TRUE; }

}
