@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"
cl /O2 /W4 /utf-8 ^
    src\main.c src\core\player.c src\media\media_source.c ^
    src\video_out\vo_gdi.c src\audio_out\ao_waveout.c src\util\osd.c ^
    /Fe:MiniPlayer.exe ^
    /link user32.lib gdi32.lib ole32.lib oleaut32.lib winmm.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib shlwapi.lib
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCESS ===
) else (
    echo.
    echo === BUILD FAILED ===
)
