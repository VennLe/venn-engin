@echo off
rem ============================================================================
rem  msvc-build.cmd - build driver for the MSVC toolchain.
rem
rem  WHY THIS FILE EXISTS
rem    Makefile recipes must work under BOTH cmd.exe and sh (GNU Make on Windows
rem    picks whichever shell it finds, and with PortableGit on PATH that is sh).
rem    MSVC builds need cmd's `call ... && ...` semantics, so that cmd-only logic
rem    lives here and the Makefile just runs `cmd /c msvc-build.cmd <args>`.
rem
rem  USAGE
rem    msvc-build.cmd configure            configure the project
rem    msvc-build.cmd build [target]       build all, or one target
rem
rem  Inputs (read from the environment, all optional):
rem    VENN_BUILD_DIR     build dir       (default: build)
rem    VENN_BUILD_TYPE    Debug/Release   (default: Debug)
rem    VENN_GENERATOR     CMake generator (default: Ninja)
rem    VENN_JOBS          parallel jobs   (default: NUMBER_OF_PROCESSORS)
rem    VENN_CMAKE_ARGS    extra -D args
rem
rem  NOTE
rem    Keep this file pure ASCII. cmd.exe reads .cmd/.bat with the OEM code
rem    page (936/GBK here); UTF-8 non-ASCII bytes become garbage and get
rem    executed as commands, which produces a storm of
rem    "'xxx' is not recognized as an internal or external command".
rem ============================================================================
setlocal enabledelayedexpansion

rem The proxy vars must be cleared BEFORE vcvars/msbuild reads them: a
rem case-insensitive duplicate key makes MSBuild throw.
set "https_proxy="
set "http_proxy="
set "all_proxy="
set "no_proxy="

rem Always run from the repo root (this script's own directory).
pushd "%~dp0"

call "%~dp0vcvars.cmd"
if errorlevel 1 (
    echo [msvc-build] ERROR: vcvars.cmd failed
    popd
    exit /b 1
)

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=build"

set "BUILD_DIR=%VENN_BUILD_DIR%"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build"
set "BUILD_TYPE=%VENN_BUILD_TYPE%"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
set "GEN=%VENN_GENERATOR%"
if "%GEN%"=="" set "GEN=Ninja"
set "JOBS=%VENN_JOBS%"
if "%JOBS%"=="" set "JOBS=%NUMBER_OF_PROCESSORS%"

if /i "%ACTION%"=="configure" goto :do_configure
if /i "%ACTION%"=="build"     goto :do_build

echo [msvc-build] ERROR: unknown action "%ACTION%" - use: configure ^| build
popd
exit /b 1

:do_configure
echo [msvc-build] configure  dir=%BUILD_DIR% gen=%GEN% type=%BUILD_TYPE%
cmake -S . -B "%BUILD_DIR%" -G "%GEN%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" %VENN_CMAKE_ARGS%
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" cmake -E touch "%BUILD_DIR%/.configured-%BUILD_TYPE%"
popd
exit /b %RC%

:do_build
set "TARGET=%~2"
echo [msvc-build] build  dir=%BUILD_DIR% type=%BUILD_TYPE% target=%TARGET%
if "%TARGET%"=="" (
    cmake --build "%BUILD_DIR%" --parallel %JOBS%
) else (
    cmake --build "%BUILD_DIR%" --target "%TARGET%" --parallel %JOBS%
)
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
