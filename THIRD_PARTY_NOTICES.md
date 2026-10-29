# Third-party notices

GladiusPort includes or uses the third-party software below. The full license texts are in the
[`LICENSES/`](LICENSES/) folder. Keep that folder with any distribution of GladiusPort or of
`GladiusPortBuilder.exe`.

## In gladiusport.dll and the game install

| Component | Version | License | Text |
|---|---|---|---|
| [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) | commit `20eced5` (derived tables only) | MIT, Copyright (c) 2023 The XbSymbolDatabase authors | [LICENSES/XbSymbolDatabase.txt](LICENSES/XbSymbolDatabase.txt) |
| [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut | 1.91.9b-docking (`third_party/imgui`, in `devtools.dll`) | MIT, Copyright (c) 2014-2025 Omar Cornut | [LICENSES/DearImGui.txt](LICENSES/DearImGui.txt) |
| [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) by Doug Lea | 2.8.6 (`third_party/dlmalloc/malloc.c`) | MIT-style, Copyright 2023 Doug Lea | [LICENSES/dlmalloc.txt](LICENSES/dlmalloc.txt) |
| [SDL2](https://www.libsdl.org/) | runtime, upstream commit `c98c4fb` (`SDL2.dll`) | zlib, Copyright (C) 1997-2024 Sam Lantinga | [LICENSES/SDL2.txt](LICENSES/SDL2.txt) |

**XbSymbolDatabase** identifies the Xbox SDK library functions in the retail `default.xbe`. The replacement
tables (`tools/d3d_table.py`, `tools/replacements.py`) are derived from its output and are covered by its notice.

## Bundled into GladiusPortBuilder.exe

The builder is a Python program packaged with PyInstaller, so the exe carries the Python runtime and
Tcl/Tk (for its window).

| Component | Version | License | Text |
|---|---|---|---|
| [Python](https://www.python.org/) | 3.13 | Python Software Foundation License (includes notices for components bundled with Python) | [LICENSES/Python.txt](LICENSES/Python.txt) |
| [Tcl](https://www.tcl-lang.org/) | 8.6.15 | Tcl/Tk license (BSD-style) | [LICENSES/Tcl.txt](LICENSES/Tcl.txt) |
| [Tk](https://www.tcl-lang.org/) | 8.6 | Tcl/Tk license (BSD-style) | [LICENSES/Tk.txt](LICENSES/Tk.txt) |
| [PyInstaller](https://pyinstaller.org/) | 6.22.3 | GPL 2.0 or later with the Bootloader Exception (the packaged exe may be distributed under any terms) | [LICENSES/PyInstaller.txt](LICENSES/PyInstaller.txt) |

## Used as references only (no code included)

These projects informed the research but none of their code is part of GladiusPort, so their licenses
do not apply to it:

- [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) (GPL 2.0)
- [xemu](https://xemu.app/) (GPL 2.0)
- [pyxbe](https://github.com/mborgerson/pyxbe) (MIT)
- Protius (`protius.exe`), the Gladius community tools and converters, and the 720p XBE patch, used to
  understand the game and its formats.

## Windows components

Direct3D 11, DXGI, WASAPI, XInput and CNG (`bcrypt.dll`) are parts of Windows that GladiusPort calls at
run time; none of them is redistributed.
