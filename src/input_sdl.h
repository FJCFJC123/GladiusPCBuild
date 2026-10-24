// SDL2 game controller backend (input_sdl.cpp). Pad state uses the Windows XInput gamepad layout.
#pragma once
#include <windows.h>

struct PadState {                    // == XINPUT_GAMEPAD
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};

bool sdl_input_start();              // false: SDL2.dll unavailable -> use XInput directly
int sdl_player_count();              // players 1..n currently have a controller (slots can have gaps)
bool sdl_read(int player, PadState *out);
void sdl_rumble(int player, WORD lo, WORD hi);
int sdl_prompt_style();              // button prompts of the controller used last: 0 Xbox, 1 PlayStation, 2 Nintendo
