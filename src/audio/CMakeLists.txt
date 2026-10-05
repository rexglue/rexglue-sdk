# rex-audio - Audio subsystem library
# Audio processing and XMA decoding for Xbox 360 emulation

add_library(rex-audio OBJECT
    
    audio_driver.cpp
    audio_system.cpp
    downmix.cpp
    xma_context.cpp
    xma_decoder.cpp
    xma_register_file.cpp
    # NOP backend (always available)
    nop/nop_audio_system.cpp
    # SDL backend
    sdl/sdl_audio_system.cpp
    sdl/sdl_audio_driver.cpp
)
add_library(rex::audio ALIAS rex-audio)

rex_target_pch(rex-audio
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/platform/thread.h>
    <rex/ppc/context.h>
    <rex/runtime.h>
    <rex/runtime/xthread.h>
)

target_include_directories(rex-audio PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rex-audio PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(rex-audio
    PUBLIC rex-core SDL3::SDL3
    PRIVATE libavcodec libavutil
)
