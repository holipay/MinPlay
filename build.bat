@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"
cl /O2 /W4 /utf-8 ^
    src\main.c src\core\player.c src\core\source_reader_callback.c ^
    src\media\media_source.c ^
    src\video_out\vo_d3d11.c src\audio_out\ao_wasapi.c src\util\osd.c ^
    src\sync\sync.c src\sync\hw_clock.c ^
    /Fe:MiniPlayer.exe ^
    /link user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib shlwapi.lib d3d11.lib dxgi.lib d3dcompiler.lib
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCESS ===
) else (
    echo.
    echo === BUILD FAILED ===
)
