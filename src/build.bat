@echo off
rem Builds gladiusport.dll (x86) into ..\build\
rem   build.bat release: ..\build\release\gladiusport.dll with /DPORT_RELEASE=1 (developer overrides off, see port.h)
rem   obj\gladiusport.lib = the import library gladiusnet.dll links against (gp_api.h)
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
set OUT=..\build
set DEFS=
if /i "%~1"=="release" (set OUT=..\build\release& set DEFS=/DPORT_RELEASE=1)
if not exist %OUT% mkdir %OUT%
cl /nologo /O2 /MT /LD /W3 /EHsc /GS- /D_CRT_SECURE_NO_WARNINGS /FIgp_api.h %DEFS% /Foobj\ port_main.cpp krnl_exports.cpp xapi_heap.cpp memstats.cpp container.cpp handles.cpp dlmalloc_cfg.c xapi_thread.cpp critsec.cpp krnl_time.cpp xapi_settings.cpp xapi_memory.cpp d3d_core.cpp d3d_resources.cpp d3d_stubs.cpp d3d_render.cpp xapi_file.cpp xapi_save.cpp xapi_section.cpp xapi_input.cpp input_sdl.cpp dsound_core.cpp dsound_stubs.cpp cri_lock.cpp xg_shader.cpp d3d_shader.cpp d3d_program.cpp d3d_prof.cpp game_resolution.cpp tex_replace.cpp d3d_ssao.cpp d3d_bloom.cpp d3d_taa.cpp d3d_shadow.cpp battle_trace.cpp battle_restart.cpp save_pages.cpp mods.cpp codes.cpp smallheap.cpp godcam.cpp editor.cpp mesh_edit.cpp bec_read.cpp grid_edit.cpp coll_edit.cpp editor_help.cpp reboot.cpp options_video.cpp hw_detect.cpp menu_quit.cpp text_buffers.cpp preview_cache.cpp shader_translate.cpp gn_core.cpp gn_hooks.cpp pkgdiag.cpp overlay.cpp ^
   /link /BASE:0x30000000 /DYNAMICBASE:NO /DEF:gladiusport.def /OUT:%OUT%\gladiusport.dll /IMPLIB:obj\gladiusport.lib /MAP:obj\gladiusport.map kernel32.lib user32.lib gdi32.lib ws2_32.lib winmm.lib ole32.lib windowscodecs.lib comdlg32.lib comctl32.lib || exit /b 1
echo built %OUT%\gladiusport.dll
