// God Camera (Ctrl+G): the retail engine still has the developers' free camera. InitGodCamera (0x6D590) builds
// it and registers console var "GodCamera" (flag 0x4B2814; camera 0x3ABF70, override pointer 0x4B2810) with the
// console key binding "key ctrl-g,godcamera=1-godcamera" - but there is no console keyboard on the PC port, so
// nothing could switch it on. While the flag is set, the world-map camera's update (camera 0x39D580, vtable
// 0x33F000, update 0x33F10 -> 0x332D0) flies the God Camera from controller 1: left stick move, right stick
// rotate, two buttons up / down; its debug-keyboard path (W A S D, R F, I K J L) needs the Xbox debug keyboard.
// Netplay: not in a session - the camera takes controller 1 away from the game, which would desync the machines.
#include "port.h"
#include "gladiusnet_api.h"

// Per frame, after the pads are polled (gn_hooks.cpp frame hook). The retail world-map screen never runs the
// camera update that contains the God Camera code (verified 2026-10-10: stick at -1.0, camera did not move), so
// call that code (0x332D0, cdecl, one unused argument) ourselves. It works on the God Camera directly (esi =
// [0x4B2810]) and rebuilds its view (0x30F00 / 0x317D0), so it flies in any screen.
void port_godcam_frame()
{
    if (*(volatile LONG *)0x4B2814 && *(void **)0x4B2810 && !gn_net_running())
        ((void(__cdecl *)(int))0x332D0)(0);
}

void port_godcam_toggle()
{
    volatile LONG *flag = (volatile LONG *)0x4B2814;
    if (gn_net_running()) {
        port_log("godcam: not available during an online match\n");
        return;
    }
    if (!*(void **)0x4B2810) {                          // InitGodCamera has not run yet
        port_log("godcam: God Camera not created yet\n");
        return;
    }
    LONG on = *flag ? 0 : 1;
    InterlockedExchange(flag, on);
    port_log("godcam: God Camera %s (Ctrl+G)\n", on ? "ON - controller 1 flies it (world map)" : "off");
}
