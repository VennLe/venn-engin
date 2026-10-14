@echo off
rem ============================================================================
rem  make.cmd - lets you type `make build` straight from this directory.
rem
rem  BACKGROUND
rem    On Windows the GNU Make binary is not always named "make.exe":
rem      * MSYS2 only ships mingw32-make.exe (no make.exe)
rem      * chocolatey / scoop normally install it as make.exe
rem      * on this machine (since 2026-10-14) the working copy lives at
rem        C:\ProgramData\chocolatey\bin\make.exe
rem    This shim finds a usable one (in priority order) and forwards args as-is.
rem
rem  USAGE
rem    cmd.exe     :  make build / make run / make clean / make package
rem    PowerShell  :  .\make.cmd build    (or  .\make build)
rem
rem  To use a bare `make` from any directory, add this directory to PATH.
rem
rem  NOTE
rem    Keep this file pure ASCII: cmd.exe reads .cmd/.bat with the OEM code
rem    page (936/GBK here) and UTF-8 bytes would be executed as commands.
rem ============================================================================
setlocal

rem ---- [1] explicit override wins ----
if defined VENN_MAKE if exist "%VENN_MAKE%" (
    "%VENN_MAKE%" %*
    exit /b %ERRORLEVEL%
)

rem ---- [2] try the usual install locations ----
set "MYVK_MAKE="
for %%P in (
    "C:\ProgramData\chocolatey\bin\make.exe"
    "C:\msys64\ucrt64\bin\mingw32-make.exe"
    "C:\msys64\usr\bin\make.exe"
    "C:\Program Files\Git\usr\bin\make.exe"
    "C:\Program Files\Git\mingw64\bin\make.exe"
    "%USERPROFILE%\scoop\shims\make.exe"
    "%LOCALAPPDATA%\Microsoft\WinGet\Links\make.exe"
) do (
    if not defined MYVK_MAKE if exist %%P set "MYVK_MAKE=%%~P"
)

rem ---- [3] fall back to bare names on PATH ----
if not defined MYVK_MAKE (
    for %%N in (make.exe mingw32-make.exe gmake.exe) do (
        if not defined MYVK_MAKE (
            for /f "delims=" %%W in ('where %%N 2^>nul') do (
                if not defined MYVK_MAKE set "MYVK_MAKE=%%W"
            )
        )
    )
)

if not defined MYVK_MAKE (
    echo [make.cmd] ERROR: no GNU Make found.
    echo [make.cmd]   Looked in the usual install dirs and on PATH.
    echo [make.cmd]   Fix: install one, e.g. `choco install make`
    echo [make.cmd]        or set VENN_MAKE to your make.exe path.
    exit /b 1
)

"%MYVK_MAKE%" %*

exit /b %ERRORLEVEL%
