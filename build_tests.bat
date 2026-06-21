@echo off
setlocal enabledelayedexpansion
set VS_PATH=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set VS_PATH=%%i
if defined VS_PATH (
    call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
) else (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
)
if %ERRORLEVEL% NEQ 0 (
    echo Failed to find VS vcvarsall.
    exit /b 1
)
cd /d "%~dp0"
cl /Od /Zi /FS /W4 /utf-8 /EHsc /FdMinPlayTests.pdb /Fe:MinPlayTests.exe ^
    tests\test_counters.cpp tests\test_main.cpp tests\sync_test.cpp tests\hls_test.cpp ^
    tests\player_test.cpp tests\audio_test.cpp ^
    src\sync\sync_context.cpp src\network\hls_stream.cpp ^
    src\util\yuv_convert.cpp src\audio_out\wasapi_audio_output.cpp src\util\log.cpp ^
    /link ole32.lib oleaut32.lib winhttp.lib shlwapi.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib ^
          avrt.lib
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === TEST BUILD SUCCESS ===
    .\MinPlayTests.exe
) else (
    echo.
    echo === TEST BUILD FAILED ===
)
