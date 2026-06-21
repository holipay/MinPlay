@echo off
setlocal enabledelayedexpansion

rem Usage: build.bat [release]
rem   (no arg)  -- debug build  (/Od /Zi)
rem   release   -- release build (/O2)

set OPTFLAGS=/Od /Zi /DCURRENT_LOG_LEVEL=1
if /i "%1"=="release" set OPTFLAGS=/O2 /DCURRENT_LOG_LEVEL=2

rem Try vswhere first (VS 2017+), fall back to default path
set VS_PATH=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set VS_PATH=%%i
if defined VS_PATH (
    call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
) else (
    rem Fallback: VS 2026 Community default path
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
)
if %ERRORLEVEL% NEQ 0 (
    echo Failed to find VS vcvarsall. Install VS or set VS_PATH manually.
    exit /b 1
)
cd /d "%~dp0"
echo Build config: %OPTFLAGS%
cl %OPTFLAGS% /W4 /utf-8 /EHsc ^
    src\main.cpp src\core\player.cpp src\core\playlist.cpp src\core\source_reader_callback.cpp ^
    src\media\media_source.cpp ^
    src\network\hls_stream.cpp ^
    src\demux\ts_packet_parser.cpp src\demux\pes_assembler.cpp src\demux\program_manager.cpp src\demux\ts_demuxer.cpp src\demux\ts_byte_stream.cpp src\demux\hls_media_source.cpp src\demux\hls_media_stream.cpp ^
    src\video_out\d3d11_video_output.cpp src\audio_out\wasapi_audio_output.cpp src\util\osd.cpp src\util\yuv_convert.cpp ^
    src\sync\sync_context.cpp ^
    /Fe:MinPlay.exe ^
    /link user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib shlwapi.lib d3d11.lib dxgi.lib d3dcompiler.lib winhttp.lib
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCESS ===
) else (
    echo.
    echo === BUILD FAILED ===
)
