@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"
cl /Od /Zi /W4 /utf-8 /EHsc ^
    src\main.cpp src\core\player.cpp src\core\source_reader_callback.cpp ^
    src\media\media_source.cpp ^
    src\video_out\d3d11_video_output.cpp src\audio_out\wasapi_audio_output.cpp src\util\osd.cpp ^
    src\sync\sync_context.cpp ^
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
