// Editor help (gladius_editor.exe): hover tips for every control of the editor window, and the Level page's guide box -
// a reference that flips between the battle grid layers and the collision tools (it follows the painter you turn on,
// and highlights the layer / tool selected).
//
// Tips are matched after the windows are built: buttons / check boxes by their caption (prefix), edit boxes, lists and
// drop-downs by the label beside (or above) them - so the pages need no changes of their own.
#include "port.h"
#include <string>
#include <vector>
#include <commctrl.h>
#include "editor.h"

struct Tip { const char *key, *text; };
static const Tip k_tips[] = {
    // Props page
    {"New prop file", "Create an empty prop file (.prp) for this level and add it to the encounter / scene."},
    {"Prop file", "The prop files this level loads. Pick one to list (and edit) its props."},
    {"Name", "The prop's name in the .prp (scripts find props by it)."},
    {"Model (.pax)", "The mesh this prop shows (data/... path of a .pax)."},
    {"Position", "World x, y, z. Drag the prop in the game window instead to place it by eye; numpad 1 / 3 raise and lower it."},
    {"Rotation", "Rotation in degrees about x, y, z (z = turn on the ground). Mouse wheel over a dragged prop turns it."},
    {"Heights", "The prop's height values from the .prp (as the original file)."},
    {"Lighting", "Lit by the scene's lights (off = full bright)."},
    {"Shadow", "Casts a shadow."},
    {"Script", "Script attached to the prop (run by the level's scripting)."},
    {"Attributes", "Extra prop attributes as written in the .prp (e.g. No_Stand / No_Cursor stamp the battle grid)."},
    {"New prop", "Type or pick a .pax model, then Add places a new prop in front of the camera."},
    {"Add", "Add a new prop with the chosen model in front of the camera."},
    {"Apply", "Apply the typed fields to the selected prop (live)."},
    {"Duplicate", "Copy the selected prop (placed beside it)."},
    {"Delete", "Remove the selected prop from the file."},
    {"To camera", "Move the selected prop to the spot in front of the camera."},
    {"Save + Reload", "Write the prop file and reload the level so the game reads it back."},
    {"Edit this prop's mesh", "Open the selected prop's mesh in the mesh tools on the right."},
    // Level page
    {"World map", "Go to the world map (random encounters are off in the editor)."},
    {"Main menu (arenas)", "Back to the main menu, to pick an arena battle."},
    {"Edit this level's geometry", "Open the scene's level model (.pao) in the mesh tools: the world map or the arena you are in."},
    {"Reset this level", "Throw away the edited level: its loose .pao / .ptx move to editor_backups and the original loads again."},
    {"Paint grid", "Battle grid painter: in a battle, left click / drag paints cells into the chosen layer, Ctrl+click erases. "
                   "The guide below lists what each layer does."},
    {"Layer", "The grid layer to paint (see the guide below). Start1-4 are the teams' placement cells."},
    {"Brush", "Brush size in cells (1, 3 or 5 square)."},
    {"Save grid", "Write the arena's grid (.grd) and any changed start file - battles on this arena use them."},
    {"Clear this layer", "Empty the selected layer everywhere (live; Save grid keeps it)."},
    {"Clear all layers", "Empty every blocking layer: a blank grid to paint from. Start positions are kept."},
    {"Paint collision", "Collision painter: on the world map or in a battle, left click / drag edits the floor collision "
                        "(.pms) with the chosen tool, Ctrl+click does the opposite. Edits are live at once."},
    {"Tool", "What a click does to the collision (see the guide below)."},
    {"Radius", "Brush radius in world units (a grid cell is 2 units)."},
    {"Category", "Surface category painted with the Surface category tool: 1 search spot, 2 road, 4 off-road, 8 unknown, 6 NPC spawn - see the guide."},
    {"Undo stroke", "Take back the last collision stroke (up to 30)."},
    {"Revert", "Reload the collision from its file - drops every edit since the last Save collision."},
    {"Save collision", "Write the scene's loose .pms (it is already live in the game)."},
    {"Grid guide", "Show the battle grid layer reference."},
    {"Collision guide", "Show the collision tool reference."},
    // Mesh tools
    {"Select sub-mesh", "Select every face of the sub-meshes highlighted in the list."},
    {"Hide / show", "Hide (or show again) the highlighted sub-meshes while editing."},
    {"Edit level", "Open the scene's level geometry instead of a prop."},
    {"Stop editing", "Close the mesh (unsaved edits are dropped)."},
    {"Select", "Tool: click / drag to select faces (Ctrl removes)."},
    {"Move", "Tool: drag the selection (along the axis below)."},
    {"Grab", "Tool: drag vertices under the brush with a soft falloff."},
    {"Smooth", "Tool: even out bumps under the brush."},
    {"Inflate", "Tool: push the surface out along its normals."},
    {"Deflate", "Tool: pull the surface in along its normals."},
    {"Paint", "Tool: paint vertex colours with the paint colour."},
    {"Texture paint", "Tool: paint the texture under the cursor (point and paint; shared texels are split off on Save)."},
    {"Grab sub-mesh", "Tool: drag whole sub-meshes (or the groups from the selection); seams are cut like paper."},
    {"Move along", "Axis for Move / Grab: the view plane or one world axis."},
    {"Brush radius", "Brush radius in screen pixels (640 x 480)."},
    {"Strength", "How strongly a brush stroke acts (0-1)."},
    {"Visible faces only", "Brushes and Select only touch what you can see: faces turned away from the camera and anything hidden behind a nearer surface (the ground under a rock) are left alone."},
    {"Fix normals", "Recompute vertex normals after edits (lighting follows the new shape)."},
    {"Wireframe", "Draw the edited mesh's wireframe over it."},
    {"Paint colour", "Colour used by Paint and Fill sel."},
    {"Colour...", "Pick the paint colour."},
    {"Fill sel.", "Paint the whole selection with the paint colour."},
    {"Add vertex colours", "Give a model without vertex colours a colour channel (needed before Paint works)."},
    {"All", "Select every face."},
    {"None", "Clear the selection."},
    {"Grow", "Add the faces touching the selection."},
    {"Delete faces", "Remove the selected faces (written on Save)."},
    {"Move by", "Exact move of the selection in world units (x, y, z) - then press Move."},
    {"Turn (deg, z)", "Turn the selection about the vertical by this many degrees - then press Turn."},
    {"Turn", "Turn the selection by the degrees beside it."},
    {"Scale %", "Scale the selection by the percentage beside it."},
    {"Textures:", "The model's textures. Pick one to export, replace or restore it."},
    {"Export as PNG", "Save the selected texture as a PNG."},
    {"Replace with image", "Swap the selected texture for an image file (resized / compressed to match)."},
    {"Restore original", "Put the selected texture back to the original."},
    {"Merge GLB", "Add a GLB model into this mesh / level: click in the game window to place its ghost, numpad turns / "
                  "tilts / raises / scales it, Enter merges."},
    {"fit size", "Size the GLB's largest side to this many units (0 = keep its own size)."},
    {"on ground, centred", "Sit the merged model on the ground at the clicked spot."},
    {"GLB up", "Which axis is up in the GLB file."},
    {"level (scans)", "Straighten a tilted scan (estimates its floor / walls)."},
    {"clear what it overlaps", "Cut away existing faces under the merged model's real footprint so it shows cleanly."},
    {"Undo", "Undo the last mesh edit."},
    {"Redo", "Redo."},
    {"Revert all", "Drop every edit since the mesh was opened."},
    {"Save (this prop)", "Write the edited mesh as this prop's own copy (the .prp is pointed at it)."},
    {"Save over the shared model", "Write the edit into the shared model itself - every prop / level using it changes."},
};

static HWND g_tip;

static const char *tip_for(const std::string &key)
{
    if (key.empty()) return nullptr;
    const Tip *best = nullptr;
    size_t bl = 0;
    for (auto &t : k_tips) {                            // the longest key the text starts with
        size_t n = strlen(t.key);
        if (n > bl && key.compare(0, n, t.key) == 0) best = &t, bl = n;
    }
    return best ? best->text : nullptr;
}

static std::string text_of(HWND h)
{
    char b[256];
    GetWindowTextA(h, b, sizeof b);
    return b;
}

static std::string label_for(HWND c)                    // the STATIC left of (or else just above) a control
{
    HWND par = GetParent(c);
    RECT rc;
    GetWindowRect(c, &rc);
    HWND best = nullptr;
    LONG bd = 1 << 30;
    bool left = false;
    for (HWND s = GetWindow(par, GW_CHILD); s; s = GetWindow(s, GW_HWNDNEXT)) {
        char cls[32];
        GetClassNameA(s, cls, sizeof cls);
        if (_stricmp(cls, "Static") || text_of(s).empty()) continue;
        RECT r;
        GetWindowRect(s, &r);
        if (r.top < rc.bottom && r.bottom > rc.top && r.right <= rc.left + 2) {   // same row, to the left
            LONG d = rc.left - r.right;
            if (!left || d < bd) best = s, bd = d, left = true;
        } else if (!left && r.bottom <= rc.top + 2 && rc.top - r.bottom < 24 && r.left < rc.right && r.right > rc.left) {
            LONG d = rc.top - r.bottom;
            if (d < bd) best = s, bd = d;
        }
    }
    return best ? text_of(best) : "";
}

static BOOL CALLBACK tip_child(HWND c, LPARAM)
{
    char cls[32];
    GetClassNameA(c, cls, sizeof cls);
    std::string key;
    if (!_stricmp(cls, "Button")) {
        LONG st = GetWindowLongA(c, GWL_STYLE) & BS_TYPEMASK;
        if (st == BS_GROUPBOX) return TRUE;
        key = text_of(c);
    } else if (!_stricmp(cls, "Edit") || !_stricmp(cls, "ComboBox") || !_stricmp(cls, "ListBox")) {
        if (GetWindowLongA(c, GWL_STYLE) & ES_READONLY && !_stricmp(cls, "Edit")) return TRUE;
        key = label_for(c);
    } else {
        return TRUE;
    }
    const char *t = tip_for(key);
    if (!t) return TRUE;
    TTTOOLINFOA ti = {};
    ti.cbSize = TTTOOLINFOA_V2_SIZE;                    // no v6 manifest: the older struct size
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(c);
    ti.uId = (UINT_PTR)c;
    ti.lpszText = (LPSTR)t;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    return TRUE;
}

void help_apply_tips(HWND frame)                        // editor thread, once every page exists
{
    if (!g_tip) {
        g_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, frame, nullptr, nullptr, nullptr);
        SendMessageA(g_tip, TTM_SETMAXTIPWIDTH, 0, 340);
        SendMessageA(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        SendMessageA(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }
    EnumChildWindows(frame, tip_child, 0);
}

// ------------------------------------------------------------------------------------------------ Level page guide
static const char *const k_guide_level =
    "LEVEL PAGE\r\n"
    "Turn on Paint grid (in a battle) or Paint collision (world map / battle) - this box then explains its layers or "
    "tools. Or pick a guide with the buttons above.\r\n\r\n"
    "Edit this level's geometry opens the level model in the mesh tools on the right. Reset puts the original level "
    "back (your edited copy goes to editor_backups).\r\n";

static const char *const k_guide_grid =
    "BATTLE GRID - 32 x 32 cells of 2 units over the arena. Each layer is a flag on the cell; a cell can be in several. "
    "Left click / drag paints the chosen layer, Ctrl erases. Changes act in the battle at once; Save grid writes them.\r\n\r\n"
    "NoMove\r\n"
    "Units cannot stand on or walk through the cell. The battle cursor can still go there (to aim at it).\r\n\r\n"
    "NoMoveNoCursor\r\n"
    "Blocked for units AND the cursor skips it. Use it for walls, water, and everything outside the arena. This is the "
    "main blocking layer of the retail grids.\r\n\r\n"
    "NoLargeUnits\r\n"
    "Normal gladiators may stand here, large units (big beasts) may not - narrow gaps and doorways.\r\n\r\n"
    "KnockbackOnly\r\n"
    "Nobody walks in, but a unit can be knocked into the cell by an attack (ledges, rubble).\r\n\r\n"
    "Gate\r\n"
    "The arena's gate cells (entrances).\r\n\r\n"
    "MapCenter\r\n"
    "The battle camera's centre point - normally one cell in the middle of the playable area.\r\n\r\n"
    "LineOfSightPass\r\n"
    "On a blocked cell: ranged attacks and line of sight pass over it (low walls, fences, railings). Without it a "
    "blocked cell blocks the shot.\r\n\r\n"
    "NoTeleport\r\n"
    "Teleport skills cannot land on the cell.\r\n\r\n"
    "Start1 / Start2 / Start3 / Start4\r\n"
    "The teams' placement cells, from the encounter's start file (Start1 = your team in most battles). They show as "
    "squares with an X in the team colour - RED when the grid blocks them: units cannot be placed there and the battle "
    "cannot start. Start changes take effect from the next battle.\r\n\r\n"
    "Clear this layer / Clear all layers empty layers to repaint from scratch (start positions are kept). Props stamp "
    "their own No_Stand / No_Cursor cells on top of the grid.\r\n";

static const char *const k_guide_coll =
    "COLLISION (.pms) - the floor units walk on, separate from the visible mesh. The overlay shows its triangles near "
    "the cursor; the white ring is the brush. Left click / drag uses the tool, Ctrl+click the opposite tool. Edits are "
    "live at once; Save collision writes the file.\r\n\r\n"
    "Raise\r\n"
    "Lifts the floor under the brush, strongest in the middle (Ctrl = Lower).\r\n\r\n"
    "Lower\r\n"
    "Sinks the floor under the brush (Ctrl = Raise).\r\n\r\n"
    "Flatten\r\n"
    "Pulls the floor toward the height of the first spot you clicked - make level paving, ramps between two heights by "
    "flattening each end.\r\n\r\n"
    "Smooth\r\n"
    "Evens out bumps and spikes (scanned levels often have them).\r\n\r\n"
    "Floor off (not walkable)\r\n"
    "The triangles stop counting as floor (drawn red): units cannot stand there and nothing finds ground. Use it to "
    "remove stray collision floating above the ground or cut holes (Ctrl = Floor on).\r\n\r\n"
    "Floor on (walkable)\r\n"
    "Makes triangles floor again (Ctrl = Floor off).\r\n\r\n"
    "Surface category\r\n"
    "Paints the triangles' category - four flags that can be mixed (the overlay colour shows it, and the line under "
    "the cursor names it). Arenas use 1 everywhere. On the world map (as seen in game):\r\n"
    "  1  green - search spot: searchable in the morning for loot\r\n"
    "  2  light blue - road (lowest random-encounter chance)\r\n"
    "  4  purple - normal off-road ground (walkable, more encounters)\r\n"
    "  8  darker blue - not confirmed (maybe more encounters)\r\n"
    "  6  yellow - road + off-road: where certain NPCs spawn\r\n"
    "  0  every category (the engine treats it as all four)\r\n"
    "Each scene only counts floor of the categories in its collide mask.\r\n\r\n"
    "Radius is in world units (2 = one grid cell). Undo stroke takes back the last strokes; Revert reloads the last "
    "save. The battle grid is separate: after reshaping the floor, repaint the grid to match.\r\n";

enum { IDH_GRID = 800, IDH_COLL, IDH_BOX };
static HWND g_guide;
static int g_guide_kind = -1;

void ed_level_guide(int kind, const char *focus)       // editor thread: 0 level, 1 grid, 2 collision
{
    if (!g_guide) return;
    if (kind != g_guide_kind) {
        SetWindowTextA(g_guide, kind == 1 ? k_guide_grid : kind == 2 ? k_guide_coll : k_guide_level);
        g_guide_kind = kind;
    }
    if (!focus || !*focus) { SendMessageA(g_guide, EM_SETSEL, 0, 0); SendMessageA(g_guide, EM_SCROLLCARET, 0, 0); return; }
    const char *all = kind == 1 ? k_guide_grid : kind == 2 ? k_guide_coll : k_guide_level;
    std::string head = std::string("\r\n\r\n") + focus + "\r\n";   // the entry's own heading line
    const char *p = strstr(all, head.c_str());
    if (!p) return;
    int a = (int)(p - all) + 4, b = a + (int)strlen(focus);
    const char *e = strstr(all + b, "\r\n\r\n");
    if (e) b = (int)(e - all);
    SendMessageA(g_guide, EM_SETSEL, a, b);              // highlighted (ES_NOHIDESEL) and scrolled to
    SendMessageA(g_guide, EM_SCROLLCARET, 0, 0);
}

bool help_command(int id, int code)                     // editor thread
{
    if ((id == IDH_GRID || id == IDH_COLL) && code == BN_CLICKED) {
        ed_level_guide(id == IDH_GRID ? 1 : 2, nullptr);
        return true;
    }
    return false;
}

int help_create_guide(HWND page, int y, int bottom)     // editor thread: the guide box from y down to bottom
{
    auto mk = [&](const char *cls, const char *text, DWORD style, int x, int yy, int w, int h, int id, DWORD ex = 0) {
        HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, yy, w, h, page, (HMENU)(INT_PTR)id,
                                 nullptr, nullptr);
        SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return c;
    };
    mk("BUTTON", "Guide", BS_GROUPBOX, 6, y, 398, bottom - y, -1);
    mk("BUTTON", "Grid guide", BS_PUSHBUTTON, 200, y + 14, 96, 22, IDH_GRID);
    mk("BUTTON", "Collision guide", BS_PUSHBUTTON, 300, y + 14, 96, 22, IDH_COLL);
    g_guide = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL | WS_VSCROLL, 14, y + 42, 382,
                 bottom - y - 50, IDH_BOX, WS_EX_CLIENTEDGE);
    ed_level_guide(0, nullptr);
    return bottom;
}
