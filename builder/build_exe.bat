@echo off
rem Packages GladiusPort Builder into a single windowed exe (builder\dist\GladiusPortBuilder.exe).
rem Needs: pip install pyinstaller. Build gladiusport.dll first: src\build.bat release
rem (default: ships the RELEASE dll, developer overrides off). "build_exe.bat dev" ships ..\build\gladiusport.dll.
rem Online versus: ..\netplay\gladiusnet.dll (prebuilt release, committed); "dev" prefers ..\build\gladiusnet.dll.
rem The converter scripts ship as plain files in tools\ and are imported from the bundle at runtime.
rem Developer tools (modders / devs): devtools.dll from the same build as gladiusport.dll (release: no crash reports
rem or dumps) and the script debugger's Dog decompiler (src\devtools\dog) - the builder offers them as its
rem "Developer tools" option (off by default).
setlocal
cd /d "%~dp0"
set DLL=..\build\release\gladiusport.dll
if /i "%~1"=="dev" set DLL=..\build\gladiusport.dll
if not exist %DLL% (echo missing %DLL% - build it first: src\build.bat release & exit /b 1)
if /i not "%~1"=="dev" findstr /c:"GPBUILD=release" %DLL% >nul || (echo %DLL% is not a release build & exit /b 1)
if exist payload rmdir /s /q payload
mkdir payload\tools
for %%f in (xbe.py xbe2pe.py xbeicon.py xiso.py replacements.py d3d_table.py kernel_names.py vcdiff.py) do copy /y ..\tools\%%f payload\tools\ >nul || exit /b 1
copy /y %DLL% payload\ >nul || exit /b 1
for %%d in (%DLL%) do copy /y "%%~dpdoldhud.dll" payload\ >nul || exit /b 1
set NETDLL=..\netplay\gladiusnet.dll
if /i "%~1"=="dev" if exist ..\build\gladiusnet.dll set NETDLL=..\build\gladiusnet.dll
if not exist %NETDLL% (echo missing %NETDLL% - online versus would be unavailable & exit /b 1)
copy /y %NETDLL% payload\ >nul || exit /b 1
copy /y ..\netplay\gladiusnet_LICENSE.txt payload\ >nul || exit /b 1
copy /y ..\build\SDL2.dll payload\ >nul || exit /b 1
xcopy /y /q /i ..\LICENSES payload\LICENSES >nul || exit /b 1
copy /y ..\THIRD_PARTY_NOTICES.md payload\ >nul || exit /b 1
copy /y ..\LICENSE payload\ >nul || exit /b 1
for %%d in (%DLL%) do copy /y "%%~dpddevtools.dll" payload\ >nul || exit /b 1
mkdir payload\devtools\dog
copy /y ..\src\devtools\dog\*.py payload\devtools\dog\ >nul || exit /b 1
python -m PyInstaller --noconfirm --onefile --windowed --name GladiusPortBuilder ^
  --add-data "payload;." ^
  --hidden-import re --hidden-import struct --hidden-import hashlib ^
  gladiusport_builder.py || exit /b 1
echo built dist\GladiusPortBuilder.exe (%DLL%, %NETDLL%)
