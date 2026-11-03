@echo off
rem Exact engine flags for lua_static, minus /O2 (which clashes with /RTC1).
setlocal
cd /d "%~dp0"
call "%~dp0..\vcvars.cmd" >nul || exit /b 1
set SRC=..\third_party\lua
set INC=/I"%SRC%"
set ENGFLAGS=/DWIN32 /D_WINDOWS /Zi /Ob0 /Od /RTC1 /MDd /utf-8 /W3 /permissive- /Zc:__cplusplus /wd4819 /W0 /GS-

echo =============== [E-1] exact engine flags (no /O2) ===============
cl /nologo %INC% %ENGFLAGS% /Fe:probe_eng1.exe luaprobe.c "%SRC%\*.c" >eng1.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL eng1 & type eng1.log ) else ( probe_eng1.exe )
echo EXIT1=%ERRORLEVEL%
endlocal
