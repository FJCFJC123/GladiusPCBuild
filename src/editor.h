// Level editor internals shared by editor.cpp (props window, picking, overlay) and mesh_edit.cpp (live mesh editing).
#pragma once
#include <stdint.h>
#include <string>
#include <windows.h>

// camera space of the scene camera: x right, y up, depth forward (the engine looks down -z)
struct CamFrame { float c[3], rt[3], up[3], bk[3], n, hw, hh; };
bool ed_cam_frame(CamFrame &f);
void ed_to_cam(const CamFrame &f, const float w[3], float v[3]);
void ed_cam_line(const CamFrame &f, float a[3], float b[3], float thick, uint32_t argb);   // overlay 640x480
bool ed_prop_alive(const uint8_t *prop);                // game thread: still in a loaded prop file
uint8_t *ed_live_prop(const std::string &file, int index);   // game thread
bool ed_set_prop_model(const std::string &file, int live, const std::string &model);   // editor thread
bool ed_ground_at(float x, float y, float z_from, float &out);   // game thread: floor height (world map / battle)
std::string ed_current_enc();                           // the encounter last loaded ("data/encounters/...")

// grid_edit.cpp (battle grid painter, Level page)
int grid_create_ui(HWND page, int y);                   // editor thread -> y below the section
bool grid_command(int id, int code);                    // editor thread: true = its control
bool grid_active();                                     // painting in a battle: the game window's clicks are its
void grid_frame();                                      // game thread
void grid_draw(const CamFrame &f);                      // game thread, overlay open
void grid_press(float nx, float ny, bool ctrl);
void grid_move(float nx, float ny, bool down);
void grid_release();

// editor_help.cpp (hover tips, Level page guide)
void help_apply_tips(HWND frame);                       // editor thread, after every page is built
int help_create_guide(HWND page, int y, int bottom);
bool help_command(int id, int code);
void ed_level_guide(int kind, const char *focus);       // 0 level, 1 grid, 2 collision; focus = entry to highlight

// coll_edit.cpp (collision painter, Level page)
int coll_create_ui(HWND page, int y);
bool coll_command(int id, int code);
bool coll_active();                                     // painting in a scene: the game window's clicks are its
void coll_frame();                                      // game thread
void coll_draw(const CamFrame &f);                      // game thread, overlay open
void coll_press(float nx, float ny, bool ctrl);
void coll_move(float nx, float ny, bool down);
void coll_release();

bool ed_save_file(const std::string &key, const std::string &data);   // "data/..." under the game folder

// mesh_edit.cpp
void mesh_init();
void mesh_create_window(HINSTANCE inst, HWND parent, int x, int y, int w, int h);   // editor thread: the mesh panel
void mesh_edit_level();                                 // editor thread: open the scene's level geometry
void mesh_reset_level();                                // editor thread: the level back to the original (loose copy moved away)
bool mesh_window_message(MSG *m);                       // editor thread message loop: true = handled
void mesh_edit_prop(const std::string &file, int live, const std::string &model, const std::string &name);
bool mesh_active();                                     // a mesh session owns the mouse in the game window
void mesh_frame();                                      // game thread, every frame
void mesh_draw(const CamFrame &f);                      // game thread, between ov_begin / ov_end
void mesh_press(float nx, float ny, bool shift, bool ctrl);
void mesh_move(float nx, float ny, bool down);
void mesh_release();
void mesh_wheel(int notches, bool shift, bool ctrl);
bool mesh_key(int vk);                                  // game window: Ctrl+Z / Ctrl+Y, numpad (true = used)
