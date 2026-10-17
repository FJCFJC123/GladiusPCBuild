# GladiusPort

**A native Windows port of *Gladius* (LucasArts, 2003), built from the retail Xbox executable.**

GladiusPort converts the retail Xbox `default.xbe` into a Windows `.exe` and runs it on top of
`gladiusport.dll`, a hand-written replacement for the Xbox system libraries. The original game code and
the original game data run unchanged. Nothing is emulated: there is no Xbox kernel, no CPU emulation and
no GPU emulation. The game's own x86 code runs directly on the PC, and every call it used to make into
the Xbox kernel, XAPI, Direct3D 8, DirectSound or the controller libraries now lands in Windows code
instead (Direct3D 11, WASAPI, XInput/SDL2, plain files).

On top of a faithful port, the project adds a modern renderer, a Video options page, HD texture
support, widescreen and any resolution, keyboard and modern controller support, more save slots, and
**online versus play** over the internet.

> **No game files are included.** You need your own copy of the retail Xbox disc (USA). The
> GladiusPort Builder reads it and creates the PC install on your machine.

---

## Contents

- [How it works](#how-it-works)
- [Features](#features)
  - [Platform port](#platform-port)
  - [Graphics](#graphics)
  - [Display and resolution](#display-and-resolution)
  - [Input](#input)
  - [Audio and movies](#audio-and-movies)
  - [Saves and game options](#saves-and-game-options)
  - [Online versus (netplay)](#online-versus-netplay)
  - [GladiusPort Builder](#gladiusport-builder)
- [Installing and playing](#installing-and-playing)
- [Controls](#controls)
- [Settings files](#settings-files)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Credits and references](#credits-and-references)
- [License](#license)
- [Legal](#legal)

---

## How it works

```
retail default.xbe ──► tools/xbe2pe.py ──► gladius.exe (PE, original code at original addresses)
                                              │
                                              │  every former Xbox kernel / XAPI / D3D8 / DSOUND / XInput call
                                              ▼
                                       gladiusport.dll ──► Windows: Direct3D 11, WASAPI, XInput, SDL2, files
```

1. **XBE → PE conversion (`tools/xbe2pe.py`).** The Xbox executable is rebuilt as a Windows PE image with
   base address `0x10000`, so every XBE section keeps its original virtual address and no code needs
   relocating. The XBE kernel thunk table becomes the import table of `gladiusport.dll` (one `krnl_*`
   export per former kernel import), the XBE TLS block becomes a PE TLS directory, and a small `.port`
   section holds the startup stub and a few compatibility patches.
2. **Function replacement (`tools/replacements.py`).** Xbox library functions statically linked into the
   game (XAPI, Direct3D 8, DirectSound, XGRAPHICS, XPP) are found by signature and redirected to
   `gladiusport.dll`. The game's own logic (the code LucasArts wrote) is not touched; it never called the
   Xbox APIs directly.
3. **The replacement layer (`src/`).** Each Xbox subsystem has a Windows implementation with the same
   calling conventions and data layouts the game expects:
   - **XAPI**: files, heaps, threads, synchronization, time, sections, dashboard settings, saves.
   - **Direct3D 8 → Direct3D 11**: the engine's push-buffer commands, render states, textures and its
     runtime-generated vertex and pixel shaders, translated to HLSL.
   - **DirectSound → software mixer → WASAPI**.
   - **XInput / XPP → Windows XInput, SDL2 and the keyboard**.
4. **Startup.** `gladiusport.dll` runs the game's runtime initialisation (CRT init tables, C++
   constructors, FPU setup) in the original order, then calls the game's `main`.

---

## Features

### Platform port

- **Native execution**: the retail Xbox code runs directly on the PC; no emulator.
- **Xbox kernel removed**: the converted exe has no Xbox kernel; every former kernel import is a
  Windows implementation or a reported trap.
- **XAPI on Windows**: file I/O with Xbox drive letters mapped to PC folders (`D:` game files,
  `U:`/`T:`/`Z:` saves and cache), heaps, threads, critical sections on the Xbox 28-byte layout, timers,
  XBE sections, dashboard settings.
- **Memory container**: the game's heap and "physical" memory live in fixed, reserved address ranges
  managed by a deterministic allocator (dlmalloc mspaces), so the game's memory is laid out identically
  on every PC. This is what makes netplay state sync possible.
- **Virtual handles**: the game never sees a real Windows handle, so handle values are identical
  across machines too.
- **Soft reboot**: leaving the campaign world map or quitting to the menu (where the Xbox relaunches
  `default.xbe`) restarts seamlessly in place.
- **Diagnostics**: `port.log` with startup trace, crash logging with full-memory dumps, a watchdog that
  reports stuck threads, an optional sampling profiler and memory statistics.

### Graphics

A Direct3D 11 renderer underneath the original engine. It keeps the original look by default and adds
optional upgrades, all switchable in the in-game **Video** options page or live with hotkeys.

- **Shader translation**: the engine generates Xbox vertex/pixel shader *source text* at runtime;
  GladiusPort translates it to HLSL (shader model 4) instead of emulating NV2A microcode and register
  combiners.
- **HDR rendering** (16-bit float scene) with tone mapping.
- **Per-pixel lighting** with specular, rim light, sun relight and bump detail.
- **Sun shadows**: real shadow maps for everything that casts, beyond the game's original blob shadows.
- **Screen-space ambient occlusion (SSAO)**.
- **Soft particles**: particles fade where they meet geometry instead of cutting into it.
- **Bloom**: only overbright light (spell cores, lava, sun glints) blooms, thanks to the HDR scene.
- **God rays** from the sun.
- **Effect lights**: spell and effect particles light up the surroundings.
- **Material effects** by texture name (water, metal, emissive, foliage).
- **Anti-aliasing**: FXAA or TAA (temporal, with sub-pixel jitter).
- **Render scale** 1×–4× (supersampling) or Auto.
- **Colour grading**: saturation, contrast, warmth, vignette.
- **HD texture replacement**: load a folder of upscaled PNG textures by original texture name
  (`[textures] Pack`); F4 switches between original and HD textures live.
- **Graphics presets** (Low / Medium / High) with automatic hardware detection (GPU, CPU cores,
  monitor) choosing a recommended tier.

### Display and resolution

- **Any resolution**: the engine's hard-coded 640×480 is replaced throughout (`Resolution = auto`
  follows the monitor).
- **Widescreen** UI aspect 4:3, 16:9 or 21:9, with **Fill** (the whole frame) or **Centered**
  (pillarboxed) screen fit; movies and 2D follow the same layout.
- **Resizable window and borderless fullscreen** (F11).

### Input

- **Modern controllers through SDL2**: Xbox/XInput, PlayStation 3/4/5, Switch Pro and anything in
  SDL's mapping database share one layout, with hot-plugging.
- **Keyboard for player 1** (automatic when no controller is connected, or always on / off).
- **DS4Windows-aware**: skips the virtual mirror device so a PlayStation pad isn't seen twice.
- Ignore list for unwanted controllers.

### Audio and movies

- **DirectSound replaced by a software mixer feeding WASAPI**: positional voices, streams and effects
  mixed on the PC.
- **CRI Sofdec/ADX movies and music** run on the PC with the original middleware code, scaled to the
  chosen resolution.

### Saves and game options

- **Saves as plain files** in the install folder (`save\U\...`), in the original Xbox save format.
- **More save slots**: the save/load screen pages through up to **27 saves** (LB/RB, Q/W or
  PageUp/PageDown), instead of the original 3.
- **Video options page** added next to the game's own options.
- **Quit Game** entry in the main menu (Esc also quits).
- **Versus unlocked** from the start and **all 15 versus arenas** available (optional).
- **Developer menus** from the original game data, re-enabled (optional): the boot-time splash menu and
  the battle Debug Menu.

### Online versus (netplay)

Play versus matches against other people over the internet or LAN.

- **Lockstep netplay with input delay**: every machine runs the full game; only controller input is
  exchanged, sized automatically to the slowest connection.
- **Turn hand-off**: whoever's turn it is plays without input delay; the other players watch with a small
  jitter buffer.
- **State sync**: at session start and at each turn hand-off the host's game memory is copied to the
  other players, page by page, so every machine continues from identical state. Desyncs are detected
  and repaired automatically.
- **Relay server and match list**: host and join from an in-game lobby; matches are listed on a relay
  server, which also forwards traffic so nobody's IP address is revealed (direct connections are opt-in).
- **Match codes**: a host can require a 6-digit code; wrong codes are refused and repeated failures lock
  the address out.
- **Encrypted sessions**: each join performs an ECDH key exchange, then all traffic is encrypted and
  authenticated (AES-GCM) with replay protection.
- **Save exchange**: each player's school (save) is sent to the others, so everyone fights with their own
  gladiators.
- **Version check**: only identical builds can play together.
- **AI Simulator**: versus matches between random league schools played by the AI, configurable by
  level range and school list.
- **Abuse-hardened relay**: per-IP rate limits, quarantine, match and seat limits, rate-limited match lists
  and logs.

The online versus code (lockstep, lobby, relay client and server, encryption, save exchange, state sync) is
not part of this repository. It ships prebuilt as `netplay/gladiusnet.dll`, which the builder installs next
to `gladius.exe`; without it the game runs offline.

### GladiusPort Builder

A single Windows program (`GladiusPortBuilder.exe`) that turns your Xbox disc image into a PC install.

- Reads an Xbox **ISO** or an extracted disc folder.
- Verifies the disc is the supported retail USA release (MD5 of `default.xbe`).
- Converts `default.xbe` to `gladius.exe` and installs `gladiusport.dll`, `gladiusnet.dll` (online versus),
  SDL2 and the settings files.
- Detects your hardware and writes a recommended graphics preset (or keeps your existing settings).
- Player name and relay server for online play.
- Optional level editor (`gladius_editor.exe`): tick **Level editor** in the options (CLI `--editor`).
- GUI or command line (`--cli`), plus a quick **update** mode that only refreshes the exe and DLLs.

---

## Installing and playing

1. Get `GladiusPortBuilder.exe` (or build it, see below).
2. Run it, choose your **Gladius (USA) Xbox ISO** (or extracted disc folder) and an install folder.
3. Start `gladius.exe` in the install folder.

Everything (game files, saves, settings, logs) lives in that one folder.

**Online play:** choose **VersuS** on the main menu, then **Host** or **Join**. Both players need the
same GladiusPort build.

---

## Controls

**Keyboard (player 1)**

| Key | Xbox button |
|---|---|
| Arrow keys | D-pad |
| Z / X / A / S | A / B / X / Y |
| Q / W | White / Black (also save-screen pages) |
| Shift / Ctrl | Left / Right trigger |
| Enter | Start |
| Backspace | Back |
| Esc | Quit the game |

**Hotkeys**

| Key | Action |
|---|---|
| F4 | HD textures on / off |
| F5 | Shadows on / off |
| F6 | Ambient occlusion on / off |
| F7 | Soft particles on / off |
| F8 | Per-vertex / per-pixel lighting |
| F9 | Anti-aliasing: off → FXAA → TAA |
| F11 | Borderless fullscreen |

On keyboards whose F-keys are media keys, the number row 4–9 works as F4–F9.

---

## Settings files

Both live next to `gladius.exe`. Most graphics settings can also be changed in game
(**Options → Video**).

- **`gladiusport.ini`**: `[video]` resolution, UI mode/aspect, render scale, anti-aliasing,
  lighting, shadows, SSAO, bloom, god rays, HDR, colour grading; `[textures]` HD pack folder;
  `[input]` keyboard mode and ignored controllers; `[game]` versus/arena unlocks, developer menus, save
  pages; `[cpu]` single/multi-core; `[paths]` game and save folders; `[net]` netplay on/off.
- **`gladiusnet.ini`**: player name, relay server, LAN browsing, direct connections, match code,
  AI Simulator settings.

---

## Building from source

**Requirements**

- Visual Studio 2022 with the C++ desktop workload (x86 toolset).
- Python 3 (for the converter tools) and `pip install pyinstaller` (for the builder exe).

**Steps**

```bat
src\build.bat               :: development DLL  -> build\gladiusport.dll
src\build.bat release       :: release DLL      -> build\release\gladiusport.dll
builder\build_exe.bat       :: GladiusPortBuilder.exe with the release DLL + netplay\gladiusnet.dll -> builder\dist\
builder\build_exe.bat dev   :: same, with the development DLL
```

**Release vs development builds.** A release build (`PORT_RELEASE`) compiles out developer-only
overrides: DEP opt-out, the `-gn*` command-line switches, input record/replay, scripted button presses and
hardware memory watches. The builder refuses to package a DLL that is not a release build unless asked for
`dev`.

**Netplay DLL.** `netplay/gladiusnet.dll` is a prebuilt release binary. It talks to `gladiusport.dll`
through the interface in `src/gladiusnet_api.h` and the exports in `src/gp_api.h`; a `gladiusport.dll`
whose interface changed (`GLADIUSNET_ABI`) needs a matching `gladiusnet.dll`, otherwise online versus is
switched off at startup and the rest of the game is unaffected.

---

## Repository layout

| Path | Contents |
|---|---|
| `src/` | `gladiusport.dll`: XAPI, Direct3D 11 renderer, audio, input, saves, options, memory container |
| `netplay/` | `gladiusnet.dll`: online versus, prebuilt (source not included) |
| `tools/` | XBE → PE converter (`xbe2pe.py`), XBE/ISO readers, replacement tables, code generators, model / level / character converters |
| `builder/` | GladiusPort Builder (Python/tkinter, packaged with PyInstaller) |
| `third_party/` | dlmalloc, SDL2 |
| `LICENSES/` | License texts of all third-party components (see `THIRD_PARTY_NOTICES.md`) |

---

## Credits and references

GladiusPort is original work, but it stands on a lot of public knowledge and on tools and research
by others. Thank you to all of them.

### Code and data used by the project

- **[XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)** (Cxbx-Reloaded project, MIT
  License): identifies the Xbox SDK library functions statically linked into `default.xbe` using OOVPA
  signatures (originally invented by Caustik, maintained by RadWolfie, jarupxx, PatrickvL and
  contributors). The D3D/XAPI replacement tables are built from its output.
- **[dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html)** by Doug Lea (permissive MIT-style
  license): the allocator behind the deterministic memory container.
- **[SDL2](https://www.libsdl.org/)** (zlib License): controller support.

### Research and reference projects

- **[Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)**: reference for Xbox kernel, XAPI,
  Direct3D 8 and DirectSound behaviour, and the XBE format.
- **[xemu](https://xemu.app/)**: reference emulator for checking original Xbox behaviour (timing, NV2A
  rendering, audio) against the port.
- **[pyxbe](https://github.com/mborgerson/pyxbe)** by mborgerson: Python XBE library, reference for
  reading the XBE format.
- **Protius** (`protius.exe`, an early-development PC build of Gladius by LucasArts): the map of the
  engine. Its function names and structures guided the reverse engineering of the Xbox engine's
  graphics, material, mesh, effects and game layers.
- **Gladius BEC tools and format research** by **[JIMB16](https://github.com/JIMB16/Gladius)** and
  **[xexuxjy](https://github.com/xexuxjy/Gladius)**: the BEC archive tool (unpacking `gladius.bec`, file
  name hashes) and their work on the Gladius model, texture and level formats.
- **Gladius modding community converters**: the PAX/PTX/PAO model, texture and level converters, which
  document the Xbox asset formats.
- **720p patch for the Xbox release** (`default720P.XBE` / `default720PFIX.XBE`) by **FlynnyFoo** and
  **Espiral**: showed every place the engine hard-codes 640×480.
- **Xbox HD texture pack** by **FlynnyFoo**, inspired by **Swift's Dolphin Texture Remaster**: the
  upscaled Xbox textures GladiusPort's replacement loader was built around (not included).
- Public documentation of the **XBE**, **XDVDFS** and **PE** file formats.

### The game

***Gladius*** was developed and published by **LucasArts** (2003). All game code, art, audio and data
belong to their respective owners.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the third-party components and their licenses;
the full license texts are in [`LICENSES/`](LICENSES/).

---

## License

GladiusPort's source code is released under the [MIT License](LICENSE). The license covers this
repository's code only:

- `netplay/gladiusnet.dll` is shipped as a binary only (free to use and redistribute unmodified with
  GladiusPort; source not public).
- Third-party components keep their own licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
- *Gladius* itself is not part of this project and is not covered by any license here.

## Legal

GladiusPort is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by
LucasArts, Lucasfilm, Disney or Microsoft. *Gladius* is a trademark of its respective owner.

This repository contains **no game code or game data**. The port only works with a retail copy of the
game that you own: the builder converts your own disc image on your own machine. Do not distribute
converted executables, extracted game files or disc images.
