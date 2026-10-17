// Xbox DirectSound replacement: software mixer -> WASAPI (dsound_core.cpp).
#pragma once
#include "port.h"

void dsound_stub_call(DWORD addr, const char *name, const char *fmt, ...);
