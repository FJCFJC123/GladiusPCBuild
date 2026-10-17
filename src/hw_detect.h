// Hardware detection for the Video settings presets (hw_detect.cpp)
#pragma once
#include <windows.h>

enum { HW_LOW, HW_MEDIUM, HW_HIGH };
struct HwInfo {
    char gpu[128];
    UINT vendor, vram_mb;                               // strongest hardware adapter, dedicated video memory
    int cores, threads;                                 // physical cores, logical processors
    int screen_w, screen_h;                             // primary monitor
    int tier;                                           // HW_LOW / HW_MEDIUM / HW_HIGH
};
const HwInfo &port_hw_info();                           // detected once, logged as "hardware:"
