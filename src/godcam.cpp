// God Camera (Ctrl+G): the retail engine's developer free camera (flag 0x4B2814, camera pointer 0x4B2810), flown
// with controller 1. Disabled during online matches, where it would desync the machines.
#include "port.h"
#include "gladiusnet_api.h"

// Per frame: run the God Camera update (0x332D0) ourselves, since the retail screens never call it.
void port_godcam_frame()
{
    if (*(volatile LONG *)0x4B2814 && *(void **)0x4B2810 && !gn_net_running())
        ((void(__cdecl *)(int))0x332D0)(0);
}
// Switch the God Camera on / off.

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
