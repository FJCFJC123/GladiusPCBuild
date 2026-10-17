# Gladius Editor

GladiusPort includes an optional level editor that runs **inside the live game**. You walk or fight through a
level, edit it, and see the result immediately: props, model geometry and textures, floor collision and the
battle grid. Edits are saved as loose files in the install's `data\` folder, so the original `gladius.bec` is
never modified, and any edit can be undone by deleting or resetting the file.

[← Back to the README](../README.md)

---

## Contents

- [Getting started](#getting-started)
- [Controls](#controls)
- [Prop page](#prop-page)
- [Mesh tools](#mesh-tools)
- [Level page](#level-page)
- [Importing models](#importing-models)
- [How edits are stored](#how-edits-are-stored)
- [Editing and online play](#editing-and-online-play)

---

## Getting started

1. In the GladiusPort Builder, tick **Level editor** (or pass `--editor` on the command line). The builder installs
   `gladius_editor.exe` next to `gladius.exe`.
2. Start `gladius_editor.exe`. The game starts as usual, with the **Gladius Editor** window alongside it.
3. Go to the place you want to edit: the world map, a town or an arena battle. The **Go to** buttons on the Level
   page jump to the world map or the arena menu.

The editor window stays on top of the game. Closing it only hides it, and **Ctrl+E** brings it back. Random
encounters on the world map are switched off while the editor runs. Every control has a hover tip, and the Level
page has a built-in guide to the grid layers and collision tools.

---

## Controls

| Key / input | Action |
|---|---|
| **Ctrl+E** | Show the editor window |
| **Ctrl+G** | God Camera on / off: the retail engine's developer free camera, flown with controller 1 |
| **Ctrl+R** | Save and reload the current level or battle with the edited files |
| **Ctrl+Z / Ctrl+Y** | Undo / redo |
| Left click (game window) | Select the prop under the cursor, or use the active tool |
| Drag | Move the selected prop along the floor, or paint or move with the active tool |
| Mouse wheel | Turn the selected prop 15° per notch; with a mesh tool, change the brush size |

---

## Prop page

Props are the static objects placed in a level: walls, rocks, banners, crates and so on. Each level and each
battle loads its props from `.prp` files. The Prop page edits them live.

- **Prop file:** choose which of the level's prop files to edit, or create a **New prop file**. A battle's new
  prop file is attached to its encounter automatically.
- **Prop list:** every prop in the file. Selected props get a yellow wire box in the game, and nearby props a dim
  cyan one.
- **Properties:**
  - **Model** (`.pax`);
  - **Position** and **Rotation**;
  - **Heights**;
  - **Lighting** and **Shadow**;
  - **Script** name;
  - **Attributes:** *No_Stand*, *No_Cursor*, *invisible*, *No_Modify_Heights*.
- **Placing props:**
  - **New prop:** pick a model and add it;
  - **Duplicate** and **Delete**;
  - **To camera:** drop the prop where the God Camera is, snapped to the ground.
- **Dragging:** a dragged prop follows the cursor, snapped onto the floor. In battle, the grid heights and
  pathfinding update as props move.
- **Save + Reload:** writes the `.prp` and reloads the level.
- **Edit this prop's mesh...:** opens the prop's model in the mesh tools.

---

## Mesh tools

The panel on the right edits a model's geometry and textures directly in the running game. It opens from the
Prop page for one prop, or from the Level page for the level's own terrain model (`.pao`).

**Selecting**
- **Select** and **Select sub-mesh**, with **Grow** and **Fill selection**.
- **Visible faces only**.
- **Hide / show** parts.
- **Wireframe** overlay.

**Shaping**
- **Grab** and **Grab sub-mesh**.
- **Move**, **Move along**, **Move by**, **Turn** and **Scale %**.
- **Smooth**, **Inflate** and **Deflate**, with a brush radius and strength.

**Faces**
- **Delete faces**.
- **Fix normals:** recomputes lighting normals; hard edges stay hard.

**Colour**
- **Paint colour:** vertex colours. Models without vertex colours get them when saved.

**Textures**
- **Texture paint:** paint straight onto the model's textures.
- **Export as PNG...** and **Replace with image...**
- **Restore original**.

  If you paint a texture that other parts of the model share, the painted faces get their own copy, so the
  rest keep the original.

**History**
- **Undo**, **Redo** and **Revert all**, with one step per brush stroke.

**Saving**
- **Save (this prop):** writes a private copy of the model for this prop and points its `.prp` at it, so other
  props using the same model are unaffected.
- **Save over the shared model:** changes every prop and level that uses it.

For level terrain, saving refits the model's culling boxes around the edited geometry, so moved pieces don't
disappear at the wrong camera angles.

---

## Level page

**Navigation and terrain**
- **Go to:** the world map or the main menu (arenas).
- **Edit this level's geometry:** opens the level's terrain model (the world map or the current arena) in the
  mesh tools.
- **Reset this level to the original:** removes the edited copy. It is moved to `editor_backups\`, not deleted.

**Collision painter.** Floor collision (`.pms`) decides where characters can walk and where the camera and
cursor rest. You can edit it live:
- **Raise**, **lower**, **flatten** and **smooth** the floor.
- Switch triangles **off and on**.
- Paint **surface categories**, which on the world map mark roads, off-road ground, search spots and NPC spawn
  areas.

**Battle grid painter.** In a battle, the arena's grid layers can be painted cell by cell, and the cursor and
pathfinding use a painted cell straight away:

| Layer | Effect |
|---|---|
| *NoMove* | Units can't move onto the cell |
| *NoMoveNoCursor* | Can't move there, and the cursor can't reach it either |
| *KnockbackOnly* | Units only end up there by being knocked back |
| *Gate* | A gate cell |
| *NoLargeUnits* | Large units can't stand there |
| *MapCenter* | The arena's centre |
| *LineOfSightPass* | Ranged attacks see through the cell |
| *NoTeleport* | Teleports can't land there |

Each team's **start positions** can be painted too; they apply from the next battle. The grid is saved as the
level's `.grd`.

---

## Importing models

- **New props from GLB:** `tools/glb2pax.py` converts a glTF / GLB model into a Gladius `.pax`. Put the result in
  `data\`, and it appears in the Prop page's model list.
- **Merge GLB...:** in the mesh tools, merges a GLB into the model being edited. Options:
  - **Fit size**, and **On ground, centred**;
  - the model's **up axis**: standard glTF Y-up, Z-up exports and scans, or upside down;
  - **Level** for scanned models;
  - **Clear what it overlaps:** cuts away existing faces under the new piece's footprint.

---

## How edits are stored

- **Loose files in `data\`.** Every edit is saved as a loose file under the install's `data\` folder, mirroring
  the game's own paths (for example `data\levels\...`). A loose file wins over the copy inside `gladius.bec`, and
  files that `gladius.bec` doesn't have can be added the same way.
- **Undoing a change.** To undo a change permanently, delete its loose file, or use **Reset this level to the
  original**.
- **Playing your edits.** The edited files are ordinary game data, so `gladius.exe` (without the editor) plays
  them too.
- **Unsaved edits.** Until you save, edits live only in game memory.

---

## Editing and online play

The online version check includes every loose file in `data\` (paths and contents). Players can only play
together if they have the **same** edits, or none. To play online with an unmodified install, move your loose
edits out of `data\` first.
