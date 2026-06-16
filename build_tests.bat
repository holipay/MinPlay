@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "%~dp0"
cl /Od /Zi /W4 /utf-8 /EHsc /Fe:MinPlayTests.exe ^
    tests\test_counters.cpp tests\test_main.cpp tests\sync_test.cpp tests\hls_test.cpp ^
    src\sync\sync_context.cpp src\network\hls_stream.cpp ^
    /link ole32.lib oleaut32.lib winhttp.lib shlwapi.lib ^
          mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib uuid.lib
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === TEST BUILD SUCCESS ===
    .\MinPlayTests.exe
) else (
    echo.
    echo === TEST BUILD FAILED ===
)
