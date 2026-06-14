cmake_minimum_required(VERSION 3.15)
project(MinPlay C)

set(CMAKE_C_STANDARD 17)

add_executable(MinPlay WIN32
    src/main.c
    src/core/player.c
    src/core/source_reader_callback.c
    src/media/media_source.c
    src/video_out/vo_gdi.c
    src/audio_out/ao_wasapi.c
    src/util/osd.c
)

target_link_libraries(MinPlay PRIVATE
    user32
    gdi32
    ole32
    oleaut32
    winmm
    mf
    mfplat
    mfreadwrite
    mfuuid
    uuid
    shlwapi
)

target_include_directories(MinPlay PRIVATE src)

if(MSVC)
    target_compile_options(MinPlay PRIVATE /W4 /utf-8)
    set_target_properties(MinPlay PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE"
    )
endif()
