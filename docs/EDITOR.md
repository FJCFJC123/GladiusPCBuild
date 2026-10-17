# Gladius Editor

GladiusPort includes an optional editor that runs **inside the live game**. Walk the world map or start an arena
battle, open the editor, and change the place you're standing in. Every change shows up immediately, with no
export or rebuild step.

You can:
- reshape and retexture **level terrain**;
- **merge new 3D models** into a level's own geometry;
- edit the **floor collision** that decides where units can walk;
- paint the **battle grid** that controls movement, line of sight and starting positions in arenas;
- place and edit **props**.

Everything is saved as loose files in the install's `data\` folder. The original `gladius.bec` is never modified,
and a level can always be reset to the original.

[← Back to the README](../README.md)

---

## Contents

- [Getting started](#getting-started)
- [The editor window](#the-editor-window)
- [Controls](#controls)
- [Level geometry](#level-geometry)
- [Merging GLB models into a level](#merging-glb-models-into-a-level)
- [Mesh tools reference](#mesh-tools-reference)
- [Textures](#textures)
- [Floor collision](#floor-collision)
- [Battle grid](#battle-grid)
- [Props](#props)
- [Saving, storage and resetting](#saving-storage-and-resetting)
- [Command-line tools](#command-line-tools)
- [Requirements and limits](#requirements-and-limits)
- [Editing and online play](#editing-and-online-play)

---

## Getting started

1. In the GladiusPort Builder, tick **Level editor** (or pass `--editor` on the command line). The builder installs
   `gladius_editor.exe` next to `gladius.exe`: the same game, with the editor switched on.
2. Start `gladius_editor.exe`. The game window opens with the **Gladius Editor** window beside it.
3. Go somewhere to edit. On the Level page, **Go to** jumps straight to the **World map** or the **Main menu**,
   where you pick an arena battle.
4. Turn on the **God Camera** (Ctrl+G) to fly freely around the level with controller 1.

Random encounters on the world map are switched off while the editor runs, so you can work undisturbed. Every
control in the editor shows a tip when you hover over it, and the Level page has a built-in guide for the grid
layers and collision tools.

---

## The editor window

The editor window stays on top of the game; closing it only hides it, and **Ctrl+E** brings it back.

| Area | What it does |
|---|---|
| **Level page** (left) | Navigation, level geometry, the collision painter and the battle grid painter, with a guide box |
| **Character page** (left) | Reserved for character mesh editing (skinned gladiators); not available yet |
| **Prop page** (left) | The level's prop files and their props |
| **Mesh tools** (right) | Shaping, painting and texturing of whatever mesh is open: the level's terrain or a prop |

---

## Controls

| Key / input | Action |
|---|---|
| **Ctrl+E** | Show the editor window |
| **Ctrl+G** | God Camera on / off (the retail engine's developer free camera, flown with controller 1) |
| **Ctrl+R** | Reload the current level or battle with the edited files |
| **Ctrl+Z / Ctrl+Y** | Undo / redo |
| Left click / drag (game window) | Use the active tool: select, sculpt, paint, place or drag |
| Ctrl + click | The opposite action: deselect, erase a grid cell, lower instead of raise |
| Shift + click | Add to the selection or the picked sub-meshes |
| Mouse wheel | Brush size; turns what you're moving, placing or dragging (Ctrl+wheel 45°) |
| Shift + wheel | Scales what you're moving or placing |
| Numpad 4 / 6 | Turn left / right (5°; with Shift, 1°) |
| Numpad 8 / 2, 7 / 9 | Tilt about x / y |
| Numpad + / − | Scale up / down (5%; with Shift, 1%) |
| Numpad 1 / 3 | Lower / raise (0.25 units; with Shift, 0.05) |
| Numpad 5 | Reset turn, tilt, scale and height |

The numpad keys act on a GLB being placed and on picked sub-meshes. Numpad 1 / 3 also raise and lower a dragged
prop.

---

## Level geometry

**Edit this level's geometry** (on the Level page) opens the scene's level model in the mesh tools: the world
map's terrain, or the arena you are fighting in. The level model is the `.pao` file holding the visible
ground, walls, buildings and scenery of the area. It is edited exactly like a prop, with every tool in the
[mesh tools](#mesh-tools-reference) available.

- **Shape the terrain.** Grab, smooth, inflate or deflate ground and walls with a soft brush. Raise a hill, carve a
  path, pull a wall back.
- **Move whole pieces.** **Grab sub-mesh** picks a piece of the level (a building, a rock face, a wall section) and
  moves, turns or scales just that piece. The piece's seams are cut like paper, so it comes away cleanly from its
  neighbours.
- **Delete** unwanted faces, or **hide** parts while working to see what's behind them.
- **Recolour.** Paint vertex colours to darken, tint or fake shading, and paint straight onto the textures.
- **Merge new models** into the level. See the next section.
- **Culling stays correct.** Saving refits the level's culling boxes around the edited geometry. Without this,
  moved or added geometry would vanish at camera angles where the engine thinks nothing is there.

**Visual only.** The visible mesh and the collision are separate. After reshaping the ground, adjust the
[floor collision](#floor-collision) so units walk on the new surface, and the [battle grid](#battle-grid) so
movement in arenas matches.

**Reset this level to the original** throws away the edited level, moving its loose `.pao` and `.ptx` to
`editor_backups\`, and loads the original again.

---

## Merging GLB models into a level

**Merge GLB...** adds a 3D model in glTF binary format (`.glb`) to the open mesh. That mesh can be the level
itself or a prop. Any GLB works: a model from Blender, an asset store download or a photogrammetry scan. The
merged geometry becomes part of the level's own model file, with its own textures, and is drawn, lit and
shadowed like the rest of the level.

**Placing it**
1. Click **Merge GLB...** and choose the file. A **ghost** of the model (its wireframe) follows the cursor over the
   game window.
2. Set it up before placing. All of these can be changed while the ghost is live:
   - **Fit size:** scale the model so its largest side is this many units. 0 keeps the file's own size. One grid
     cell is 2 units.
   - **GLB up:** which axis is up in the file: standard glTF Y-up, Z-up exports and scans, or upside down.
   - **Level (scans):** straightens a tilted scan by estimating its floor and main wall.
   - **On ground, centred:** sits the model on the ground at the clicked spot, centred on it.
   - **Clear what it overlaps:** cuts away the existing faces under the model's real footprint, so it doesn't
     z-fight with the ground or walls it replaces.
3. Point at the spot and adjust:
   - the **mouse wheel** turns the model 5° per notch (Ctrl+wheel 45°) and **Shift+wheel** scales it;
   - the **numpad** turns, tilts, raises and lowers it (see [Controls](#controls));
   - numpad 5 resets the placement.
4. **Click** to place it. The level is saved with the model merged in and reloads with it in place. **Stop
   editing** cancels a placement.

**Reading the preview.** The preview shows what the merge will do before you commit:

| Colour | Meaning |
|---|---|
| Blue | The ghost of the new model |
| Red ghost edges | Parts of the new model that would end up below the existing surface |
| Yellow | The new model's bounding box |
| Orange | Existing pieces of the level the new model overlaps |
| Magenta | Existing faces that **Clear what it overlaps** would cut away |

**What the merge does to the file**
- **Appended, not replaced.** The model is added to the existing level file. Nothing the level already has is
  moved or rebuilt, so the original geometry, materials and culling stay intact.
- **Materials and textures.** Each GLB material becomes its own sub-mesh with the level's own render settings. Its
  texture is converted to the Xbox format (DXT, mipmapped) and appended to the level's texture file.
- **Vertex colours.** A GLB with vertex colours keeps them.
- **Undoing a merge.** A merge is written to the file straight away. To take it back, use **Reset this level** (or
  delete the prop's loose copy).

Prop meshes take merges the same way, for example to add a banner to a wall prop.

---

## Mesh tools reference

The panel on the right edits whatever mesh is open: the level's terrain (**Edit level** / **Edit this level's
geometry**) or one prop (**Edit this prop's mesh...**).

### Selection

| Control | Action |
|---|---|
| **Select** (tool) | Click or drag to select faces; Shift adds, Ctrl removes |
| **All / None** | Select everything / nothing |
| **Grow** | Add the faces touching the selection |
| **Select sub-mesh** | Select every face of the sub-meshes highlighted in the list |
| **Hide / show** | Hide the highlighted sub-meshes while working (or show them again) |
| **Visible faces only** | Brushes and selection only touch what you can see: faces turned away, and anything behind a nearer surface (the ground under a rock), are left alone |
| **Wireframe** | Draw the mesh's wireframe over it |

### Shaping

| Control | Action |
|---|---|
| **Move** (tool) | Drag the selection, along the view plane or one world axis (**Move along**) |
| **Grab** (tool) | Drag the vertices under the brush with a soft falloff |
| **Grab sub-mesh** (tool) | Pick whole sub-meshes under the cursor (Shift adds) and move them; the wheel turns, Shift+wheel scales, the numpad turns and tilts. Normals turn with the piece |
| **Smooth** (tool) | Even out bumps under the brush |
| **Inflate / Deflate** (tools) | Push the surface out / pull it in along its normals |
| **Move by** | Move the selection an exact x, y, z distance |
| **Turn (deg, z)** | Turn the selection about the vertical axis |
| **Scale %** | Scale the selection |
| **Brush radius / Strength** | Brush size in screen pixels; strength 0–1 |

### Faces, colour and normals

| Control | Action |
|---|---|
| **Delete faces** | Remove the selected faces |
| **Paint** (tool) | Paint vertex colours with the paint colour |
| **Paint colour / Colour... / Fill sel.** | Choose the colour; fill the whole selection with it |
| **Add vertex colours** | Give a model without vertex colours a colour channel, needed before Paint works |
| **Fix normals** | Recompute lighting normals after edits (hard edges stay hard) |

### History and saving

| Control | Action |
|---|---|
| **Undo / Redo** | One step per stroke or operation |
| **Revert all** | Drop every edit since the mesh was opened |
| **Save (this prop)** | Write the edited mesh as this prop's **own copy** and point its prop entry at it; other props using the model are unaffected |
| **Save over the shared model** | Change the model itself: every prop and level using it changes |
| **Stop editing** | Close the mesh (unsaved edits are dropped) |

A level's terrain is always saved over its own loose `.pao`.

---

## Textures

The **Textures** list shows every texture the open mesh draws with. Pick one to work on it.

- **Texture paint** (tool): paint straight onto the texture under the cursor, at the right scale for each surface.
  If the painted texture is also used by faces you didn't touch (a shared atlas), only the painted faces get
  their own copy of the texture on save. The rest keep the original.
- **Export as PNG...** saves the texture so you can edit it in an image editor.
- **Replace with image...** loads a PNG, TGA, BMP or JPG. It is resized to the texture's size, mipmapped and
  compressed to the texture's DXT format.
- **Restore original** puts the texture back.

Editable formats are DXT1 and DXT5, the formats nearly all level and prop textures use.

---

## Floor collision

Collision is the invisible floor units walk on, stored in the level's `.pms` file and separate from the visible
mesh. **Paint collision** on the Level page edits it live on the world map or in a battle. The game window then
shows the collision triangles near the cursor, with a white ring for the brush.

| Tool | Effect (Ctrl + click does the opposite) |
|---|---|
| **Raise / Lower** | Lift or sink the floor under the brush, strongest in the middle |
| **Flatten** | Pull the floor toward the height of the first spot clicked. Use it for level paving; for a ramp, flatten each end to its height |
| **Smooth** | Even out bumps and spikes (scanned levels often have them) |
| **Floor off** | Triangles stop counting as floor (drawn red): nothing can stand there. Removes stray collision or cuts holes |
| **Floor on** | Makes triangles floor again |
| **Surface category** | Paints the surface type (see below) |

**Surface categories** are four flags that can be combined. Arenas use 1 everywhere. On the world map:

| Value | Colour | Meaning |
|---|---|---|
| 1 | Green | Search spot: searchable in the morning for loot |
| 2 | Light blue | Road (lowest random-encounter chance) |
| 4 | Purple | Normal off-road ground (walkable, more encounters) |
| 6 | Yellow | Road + off-road: where certain NPCs spawn |
| 8 | Darker blue | Not confirmed (possibly more encounters) |
| 0 | — | Every category |

Other controls:
- **Radius:** the brush size in world units (2 units = one grid cell).
- **Undo stroke:** takes back up to 30 strokes.
- **Revert:** reloads the last saved collision.
- **Save collision:** writes the loose `.pms`. The edits are already live before saving.

---

## Battle grid

Arena battles are played on a 32 × 32 grid of 2-unit cells. **Paint grid** on the Level page (in a battle) paints
its layers cell by cell: click or drag to paint, Ctrl+click to erase, with a 1, 3 or 5 cell **Brush**. The cursor
and pathfinding use painted cells straight away. A cell can be in several layers.

| Layer | Effect |
|---|---|
| **NoMove** | Units can't stand on or walk through the cell. The battle cursor can still go there to aim |
| **NoMoveNoCursor** | Blocked for units, and the cursor skips it. Used for walls, water and everything outside the arena; the main blocking layer of the original grids |
| **NoLargeUnits** | Gladiators may stand here, large units (big beasts) may not: narrow gaps and doorways |
| **KnockbackOnly** | Nobody walks in, but a unit can be knocked into the cell by an attack (ledges, rubble) |
| **Gate** | The arena's gate cells (entrances) |
| **MapCenter** | The battle camera's centre point, normally one cell in the middle of the playable area |
| **LineOfSightPass** | On a blocked cell, ranged attacks and line of sight pass over it (low walls, fences, railings) |
| **NoTeleport** | Teleport skills can't land on the cell |
| **Start1–Start4** | Each team's placement cells (Start1 is your team in most battles). They're drawn red when the grid blocks them, because the battle can't start until they're cleared. Changes apply from the next battle |

Other controls:
- **Clear this layer** and **Clear all layers** empty layers so you can repaint from scratch. Start positions are
  kept.
- **Save grid** writes the arena's `.grd` and any changed start file.
- Props stamp their own *No_Stand* / *No_Cursor* cells on top of the grid.

---

## Props

Props are the separate objects placed in a level from `.prp` files: rocks, crates, banners, statues, braziers.
The **Prop page** edits them live.

- **Prop file:** pick one of the prop files the level loads, or create a **New prop file**. A battle's new prop
  file is attached to its encounter automatically.
- **Prop list:** every prop in the file. The selected prop gets a yellow box in the game, and props nearby a dim
  cyan one. Click a prop in the game window to select it.
- **Properties:**
  - **Name:** scripts find props by it.
  - **Model:** a `.pax` file.
  - **Position** and **Rotation**, in world units and degrees.
  - **Heights**.
  - **Lighting:** off means full bright.
  - **Shadow**.
  - **Script**.
  - **Attributes:** *No_Stand*, *No_Cursor*, *invisible*, *No_Modify_Heights*.

  **Apply** sends typed changes to the selected prop.
- **Placing:**
  - **New prop:** pick or type a model and **Add** it in front of the camera.
  - **Duplicate** and **Delete**.
  - **To camera:** moves the prop to the God Camera's spot, snapped to the ground.
- **Dragging:** drag a prop in the game window to move it along the floor; the wheel turns it 15° per notch, and
  numpad 1 / 3 raise and lower it. In battle, grid heights and pathfinding update as props move.
- **Model list:** lists the models the file uses, plus every loose `.pax` in `data\`, including models you
  converted from GLB (see below).
- **Save + Reload:** writes the `.prp` and reloads the level.
- **Edit this prop's mesh...:** opens the prop in the mesh tools.

---

## Saving, storage and resetting

- **Loose files.** Edits are saved as loose files under the install's `data\` folder, at the same paths the game
  uses (for example `data\levels\...`). A loose file wins over the copy inside `gladius.bec`, and new files the game
  doesn't have (new prop files, new models, private prop copies) are added the same way.
- **Live until saved.** Unsaved edits live only in game memory. **Ctrl+R** saves and reloads, so the game reads
  everything back from the files.
- **Reverting.**
  - **Reset this level to the original** moves the edited level files to `editor_backups\`.
  - To undo any other change permanently, delete its loose file.
- **Plain game data.** Edited files are ordinary game data, so `gladius.exe` (without the editor) plays them too.
  Share an edit by sharing the files.

---

## Command-line tools

The repository's `tools/` folder has converters for work outside the game:

| Tool | Use |
|---|---|
| `glb2pax.py` | GLB → a new static prop model (`.pax` + `.ptx`): bakes the whole scene into one model, one sub-mesh per material |
| `paxmerge.py` | Merges a GLB into an existing prop or level model; this is what **Merge GLB...** runs |
| `glb2pao.py` | Replaces an arena's entire visible level with a GLB scene, keeping its collision and grid |
| `paxsplit.py` | Gives painted faces their own texture copy; used by texture paint on shared textures |
| `grd.py`, `pms.py`, `ptx.py`, `xrnd.py` | Readers and writers for the grid, collision, texture and model formats |

---

## Requirements and limits

- **Python for GLB tools.** **Merge GLB...** and texture paint on shared textures run the Python tools above. They
  need Python 3 on `PATH`, and the `tools` folder either at `<install>\..\tools` or in the `GLADIUS_TOOLS`
  environment variable. Each run's output goes to `paxmerge.log` next to the exe. Every other editor feature
  works without Python.
- **Static models only.** Character meshes (skinned gladiators and creatures) can't be edited yet; the Character
  page is a placeholder.
- **DXT textures only.** Texture paint and replacement work on DXT1 and DXT5 textures only, and keep the
  texture's size.

---

## Editing and online play

The online version check includes every loose file in `data\` (paths and contents). Players can only play
together if they have the **same** edits, or none. To play online with an unmodified install, move your loose
edits out of `data\` first.
