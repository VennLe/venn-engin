@echo off
rem ============================================================================
rem  run_probe.cmd - isolate the `lua_resume` access violation, no engine build.
rem
rem  Variants answer one question: which switch group does the crash follow?
rem    rel   /MD  /O2                     normal release, no runtime checks
rem    dbg1  /MDd /Od /RTC1               reproduces CMake Debug defaults
rem    dbg2  /MDd /O2                     RTC1 removed, optimiser on
rem    dbg3  /MDd /Od /RTC1 /O2           expect D8016, confirms the clash
rem
rem  NOTE: keep this file pure ASCII. cmd.exe reads .cmd with the OEM code page
rem  (936/GBK here); UTF-8 Chinese bytes get executed as commands.
rem ============================================================================
setlocal
cd /d "%~dp0"
call "%~dp0..\vcvars.cmd" >nul || exit /b 1

set SRC=..\third_party\lua
set INC=/I"%SRC%"

echo.
echo =============== [rel] /MD /O2 ===============
cl /nologo %INC% /utf-8 /MD /O2 /W0 /Fe:probe_rel.exe luaprobe.c "%SRC%\*.c" >rel.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL rel & type rel.log ) else ( probe_rel.exe )

echo.
echo =============== [dbg1] /MDd /Od /RTC1 ===============
cl /nologo %INC% /utf-8 /MDd /Od /RTC1 /W0 /Fe:probe_dbg1.exe luaprobe.c "%SRC%\*.c" >dbg1.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL dbg1 & type dbg1.log ) else ( probe_dbg1.exe )

echo.
echo =============== [dbg2] /MDd /O2  (RTC1 removed) ===============
cl /nologo %INC% /utf-8 /MDd /O2 /W0 /Fe:probe_dbg2.exe luaprobe.c "%SRC%\*.c" >dbg2.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL dbg2 & type dbg2.log ) else ( probe_dbg2.exe )

echo.
echo =============== [dbg3] /MDd /Od /RTC1 /O2  (expect D8016) ===============
cl /nologo %INC% /utf-8 /MDd /Od /RTC1 /O2 /W0 /Fe:probe_dbg3.exe luaprobe.c "%SRC%\*.c" >dbg3.log 2>&1
if errorlevel 1 ( echo BUILD-FAIL dbg3 EXPECTED & findstr /C:"D8016" dbg3.log ) else ( echo BUILD-OK dbg3 & probe_dbg3.exe )

endlocal
