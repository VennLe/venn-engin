@echo off
rem Link the probe against the SAME lua_static.lib that Game.exe uses.
setlocal
cd /d "%~dp0"
call "%~dp0..\vcvars.cmd" >nul || exit /b 1
echo =============== [link-against-engine-lib] ===============
cl /nologo /utf-8 /MDd /Od /RTC1 /I"..\third_party\lua" /Fe:probe_lib.exe luaprobe.c "..\build\lua_static.lib" >link1.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL & type link1.log ) else ( probe_lib.exe )
echo EXIT=%ERRORLEVEL%
endlocal
