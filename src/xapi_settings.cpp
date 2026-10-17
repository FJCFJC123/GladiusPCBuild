// Xbox dashboard settings (EEPROM via ExQueryNonVolatileSetting on the console) -> fixed PC defaults.
// Replaced at the XAPI level: XQueryValue, XGetAVPack, XGetVideoFlags, XGetAudioFlags,
// XGetParentalControlSetting.
#include "port.h"

enum {
    XC_LANGUAGE = 7, XC_VIDEO_FLAGS = 8, XC_AUDIO_FLAGS = 9, XC_PARENTAL_CONTROL_GAMES = 0xA,
    XC_FACTORY_AV_REGION = 0x103, XC_FACTORY_GAME_REGION = 0x104,
};
static const DWORD XC_LANGUAGE_ENGLISH = 1;
static const DWORD XC_AV_PACK_STANDARD = 6;
static const DWORD AV_REGION_NTSC_M = 0x00400100;
static const DWORD GAME_REGION_NA = 1;

static bool setting_value(DWORD index, DWORD *value)
{
    switch (index) {
    case XC_LANGUAGE: *value = XC_LANGUAGE_ENGLISH; return true;
    case XC_VIDEO_FLAGS: *value = 0; return true;
    case XC_AUDIO_FLAGS: *value = 0; return true;              // stereo
    case XC_PARENTAL_CONTROL_GAMES: *value = 0; return true;
    case XC_FACTORY_AV_REGION: *value = AV_REGION_NTSC_M; return true;
    case XC_FACTORY_GAME_REGION: *value = GAME_REGION_NA; return true;
    }
    return false;
}

extern "C" {

// DWORD XQueryValue(ULONG index, ULONG *type, void *value, ULONG size, ULONG *result_len)
DWORD __stdcall xapi_XQueryValue(DWORD index, DWORD *type, void *value, DWORD size, DWORD *result_len)
{
    DWORD v;
    if (!setting_value(index, &v)) {
        port_log("XQueryValue: unknown setting 0x%X\n", index);
        return ERROR_INVALID_PARAMETER;
    }
    if (type)
        *type = REG_DWORD;
    if (result_len)
        *result_len = 4;
    if (size < 4)
        return ERROR_INSUFFICIENT_BUFFER;
    *(DWORD *)value = v;
    return ERROR_SUCCESS;
}

DWORD __stdcall xapi_XGetAVPack() { return XC_AV_PACK_STANDARD; }
DWORD __stdcall xapi_XGetVideoFlags() { return 0; }
DWORD __stdcall xapi_XGetAudioFlags() { return 0; }
DWORD __stdcall xapi_XGetParentalControlSetting() { return 0; }

}
