# GladiusPort

**A native Windows port of *Gladius* (LucasArts, 2003), built from the retail Xbox executable.**

GladiusPort converts the Xbox `default.xbe` into a Windows `.exe`. The game's original x86 code runs natively on
the PC, with no CPU emulation. Everything the Xbox provided around the game (its kernel, Direct3D 8, DirectSound
and the controller libraries) is reimplemented in `gladiusport.dll` on top of Direct3D 11, WASAPI, SDL2 and plain
files. Strictly speaking, that reimplementation is a form of high-level emulation of the Xbox's system libraries,
much like Wine does for Windows programs. The hardware itself (CPU, GPU, audio chip) is never emulated.

> **No game files are included.** You need your own copy of the retail Xbox disc (USA). The GladiusPort Builder
> converts it into a PC install on your machine.

-  **[How it works](docs/TECHNICAL.md)**: kernel replacement, the Direct3D 8 → 11 layer and the visual enhancements
-  **[Gladius Editor](docs/EDITOR.md)**: the built-in level editor

---

## Features

- **Native PC port** of the Xbox release: the original game code runs natively, unchanged.
- **Any resolution and widescreen** (4:3, 16:9, 21:9), resizable window and borderless fullscreen.
- **Modern renderer**:
  - HDR;
  - per-pixel lighting;
  - sun shadows;
  - ambient occlusion;
  - soft particles;
  - bloom and god rays;
  - effect lights;
  - material effects;
  - FXAA / TAA;
  - supersampling;
  - colour grading.

  All of it is optional, with Low / Medium / High presets.
- **HD texture packs**: load a folder of upscaled textures; F4 switches between original and HD live.
- **Controllers**: Xbox, PlayStation, Switch Pro and more through SDL2, plus keyboard support.
- **Quality of life**: in-game Video options, 27 save slots, Quit Game in the main menu, all versus arenas.
- **Online versus** over the internet or LAN: encrypted, relay-hosted, with match codes. It's provided by the
  closed-source `gladiusnet.dll`, under its own [license](#license).
- **Level editor** (optional): reshape levels, merge in GLB models, and edit collision, the battle grid and props, live
  inside the running game. See [below](#level-editor).

---

## Installing

1. Download `GladiusPortBuilder.exe` from [Releases](../../releases), or build it yourself (below).
2. Run it and choose your **Gladius (USA) Xbox ISO** (or extracted disc folder), an install folder and,
   optionally, an HD texture pack folder.
3. Start `gladius.exe` in the install folder.

The game files, saves, settings and logs all live in that one folder. For online play, choose **VersuS** on the
main menu, then **Host** or **Join**. Both players need the same GladiusPort version. Online play comes from
`gladiusnet.dll`, which the builder installs together with its license, `gladiusnet_LICENSE.txt`.

---

## Level editor

Tick **Level editor** in the builder to also install `gladius_editor.exe`: the game with an editor window
alongside it. Walk the world map or start an arena battle, edit the place you're in, and see the result
immediately.

- **Level geometry:** sculpt, smooth, cut, move and recolour the level's own terrain and buildings, and paint its
  textures. Culling is refitted automatically on save.
- **GLB merging:** merge any glTF model (a Blender asset, a store download or a photogrammetry scan) into the
  level's mesh. A live ghost preview shows overlaps and buried edges, with optional cut-away of the faces the new
  model replaces.
- **Floor collision:** raise, lower, flatten, smooth or switch off the walkable floor, and paint surface types
  such as roads, search spots and NPC spawn areas.
- **Battle grid:** paint movement, line-of-sight, teleport and start-position layers cell by cell in an arena.
- **Props:** place, drag, rotate, duplicate and delete level objects, and edit their meshes.
- **Safe storage:** edits save as loose files in `data\`, never touching `gladius.bec`, and a level can be reset to
  the original at any time.

The full guide is in **[docs/EDITOR.md](docs/EDITOR.md)**.

---

## Controls

| Key | Xbox button | | Key | Action |
|---|---|---|---|---|
| Arrow keys | D-pad | | F4 | HD textures on / off |
| Z / X / A / S | A / B / X / Y | | F5 | Shadows on / off |
| Q / W | White / Black | | F6 | Ambient occlusion on / off |
| Shift / Ctrl | Left / Right trigger | | F7 | Soft particles on / off |
| Enter | Start | | F8 | Per-vertex / per-pixel lighting |
| Backspace | Back | | F9 | Anti-aliasing: off → FXAA → TAA |
| Esc | Quit | | F11 | Borderless fullscreen |

On keyboards whose F-keys are media keys, the number row 4–9 works as F4–F9. Settings live in
`gladiusport.ini` (game, video, input) and `gladiusnet.ini` (online play) next to `gladius.exe`. Most video
settings can also be changed in game under **Options → Video**.

---

## Building from source

Requirements:
- Visual Studio 2022 with the C++ desktop workload (x86 toolset);
- Python 3 with `pyinstaller`.

```bat
src\build.bat release       :: gladiusport.dll  -> build\release\
builder\build_exe.bat       :: GladiusPortBuilder.exe -> builder\dist\
```

`src\build.bat` without `release` builds a development DLL with extra debugging switches.

`netplay/gladiusnet.dll` (online versus) ships prebuilt and closed source; `build_exe.bat` packs it into the builder
with `netplay/gladiusnet_LICENSE.txt`. It isn't covered by the MIT License: it may only be used with GladiusPort and
redistributed unmodified, together with that license (see [License](#license)). Without it, the game runs offline.

| Path | Contents |
|---|---|
| `src/` | `gladiusport.dll`: kernel/XAPI replacements, Direct3D 11 renderer, audio, input, game additions |
| `tools/` | XBE → PE converter, replacement tables, model and level tools |
| `builder/` | GladiusPort Builder (Python / tkinter) |
| `netplay/` | `gladiusnet.dll`, prebuilt and closed source, and its license `gladiusnet_LICENSE.txt` |
| `third_party/` | dlmalloc, SDL2 |

---

## Credits

GladiusPort stands on tools and research by others. Thank you to all of them.

- **[Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)** and
  **[XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)**: Xbox kernel, XAPI and Direct3D
  reference; the library signatures that located the functions to replace.
- **[xemu](https://xemu.app/)**: reference for original Xbox behaviour.
- **[pyxbe](https://github.com/mborgerson/pyxbe)** by mborgerson: XBE format reference.
- **[dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html)** by Doug Lea and **[SDL2](https://www.libsdl.org/)**.
- **[JIMB16](https://github.com/JIMB16/Gladius)** and **[xexuxjy](https://github.com/xexuxjy/Gladius)**: Gladius
  BEC archive tools and model, texture and level format research.
- The **Gladius modding community**: PAX/PTX/PAO converters documenting the asset formats.
- **FlynnyFoo** and **Espiral**: the Xbox 720p patch, which mapped the engine's hard-coded 640×480.
- **FlynnyFoo**: the Xbox HD texture pack (inspired by **Swift's Dolphin Texture Remaster**) the texture loader
  was built around.
- **Protius**, LucasArts' early-development PC build of Gladius: the map of the engine.

***Gladius*** was developed and published by **LucasArts** (2003).

---

## License

GladiusPort's source code is released under the [MIT License](LICENSE). The license covers this repository's
code only:
- `netplay/gladiusnet.dll` is proprietary and closed source, under its own restrictive license
  ([`netplay/gladiusnet_LICENSE.txt`](netplay/gladiusnet_LICENSE.txt)). It may be used only with GladiusPort
  and redistributed only unmodified as part of GladiusPort. Reverse engineering it, modifying it or using it
  separately is not allowed.
- Third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
  [`LICENSES/`](LICENSES/).
- *Gladius* itself is not covered by any license here.

GladiusPort is an unofficial fan project, not affiliated with or endorsed by LucasArts, Lucasfilm, Disney or
Microsoft. This repository contains **no game code or game data**. The port works only with a retail copy you own.
Don't distribute converted executables, extracted game files or disc images.
