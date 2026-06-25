cmake_minimum_required(VERSION 3.15)
project(MinPlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MinPlay
    src/main.cpp
    src/core/player.cpp
    src/core/playlist.cpp
    src/core/source_reader_callback.cpp
    src/media/media_source.cpp
    src/network/hls_stream.cpp
    src/demux/ts_packet_parser.cpp
    src/demux/pes_assembler.cpp
    src/demux/program_manager.cpp
    src/demux/ts_demuxer.cpp
    src/demux/ts_byte_stream.cpp
    src/demux/hls_media_source.cpp
    src/demux/hls_media_stream.cpp
    src/video_out/d3d11_video_output.cpp
    src/audio_out/wasapi_audio_output.cpp
    src/util/osd.cpp
    src/util/yuv_convert.cpp
    src/sync/sync_context.cpp
    src/tts/text_reader.cpp
    src/tts/tts_engine.cpp
)

target_link_libraries(MinPlay PRIVATE
    user32
    gdi32
    ole32
    oleaut32
    shell32
    mf
    mfplat
    mfreadwrite
    mfuuid
    uuid
    shlwapi
    d3d11
    dxgi
    d3dcompiler
    winhttp
    winmm
    sapi
)

target_include_directories(MinPlay PRIVATE src)

if(MSVC)
    target_compile_options(MinPlay PRIVATE /W4 /utf-8 /EHsc)
    set_target_properties(MinPlay PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# Tests
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()

    add_executable(MinPlayTests
        tests/test_counters.cpp
        tests/test_main.cpp
        tests/sync_test.cpp
        tests/hls_test.cpp
        tests/player_test.cpp
        tests/audio_test.cpp
        src/sync/sync_context.cpp
        src/network/hls_stream.cpp
        src/util/yuv_convert.cpp
        src/audio_out/wasapi_audio_output.cpp
    )

    target_include_directories(MinPlayTests PRIVATE src tests)
    target_link_libraries(MinPlayTests PRIVATE
        ole32
        oleaut32
        winhttp
        shlwapi
        mf
        mfplat
        mfreadwrite
        mfuuid
        uuid
        avrt
    )

    add_test(NAME MinPlayTests COMMAND MinPlayTests)
endif()
