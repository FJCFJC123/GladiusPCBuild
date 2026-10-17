// Xbox DirectSound -> PC audio. R0: a "silent mixer" - objects behave correctly (buffer memory, play state,
// cursors advancing with real time, stream packets completing) so the game's timing logic runs; real
// output comes in the audio phase.
#pragma once
#include "port.h"

void dsound_stub_call(DWORD addr, const char *name, const char *fmt, ...);
