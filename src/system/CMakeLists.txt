set(REXSYSTEM_SOURCES
    runtime.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    shared_library.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp

    # Bink cinematic overlay (uses host libavformat/libavcodec when found).
    bink_overlay.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

# Optional system FFmpeg integration for the Bink overlay. When the host
# provides libavformat/libavcodec/libswscale, decode .bik in parallel and
# composite over Kameo's swap so cinematics render even when the recomp's
# CPU-decoded Bink frames don't reach the GPU. Linux only; falls back to a
# no-op overlay otherwise.
#
# Honors REX_FFMPEG_DIR (a CMake-config or env var) for hosts that have
# the headers + .so under a non-standard prefix (used on the m4-fed build
# host where ffmpeg-free-devel can't be installed via dnf — we sync the
# headers + libs from a target running pkg-config'd FFmpeg).
if(NOT WIN32 AND NOT APPLE)
    set(_REX_FFMPEG_DIR "${REX_FFMPEG_DIR}")
    if(NOT _REX_FFMPEG_DIR AND DEFINED ENV{REX_FFMPEG_DIR})
        set(_REX_FFMPEG_DIR "$ENV{REX_FFMPEG_DIR}")
    endif()
    set(_REX_FFMPEG_FOUND OFF)
    set(_REX_FFMPEG_INCLUDE_DIRS "")
    set(_REX_FFMPEG_LIBRARIES "")
    if(_REX_FFMPEG_DIR AND EXISTS "${_REX_FFMPEG_DIR}/ffmpeg/libavformat/avformat.h")
        # Prefix layout: ${_REX_FFMPEG_DIR}/ffmpeg/<libname>/header.h, libs in lib/.
        set(_REX_FFMPEG_FOUND ON)
        set(_REX_FFMPEG_INCLUDE_DIRS "${_REX_FFMPEG_DIR}/ffmpeg")
        foreach(_lib avformat avcodec avutil swscale)
            find_library(_REX_FFMPEG_${_lib}_LIB NAMES ${_lib}
                HINTS "${_REX_FFMPEG_DIR}/lib" NO_DEFAULT_PATH)
            if(_REX_FFMPEG_${_lib}_LIB)
                list(APPEND _REX_FFMPEG_LIBRARIES "${_REX_FFMPEG_${_lib}_LIB}")
            else()
                set(_REX_FFMPEG_FOUND OFF)
            endif()
        endforeach()
    endif()
    if(NOT _REX_FFMPEG_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(REX_FFMPEG_BINK QUIET libavformat libavcodec libavutil libswscale)
            if(REX_FFMPEG_BINK_FOUND)
                set(_REX_FFMPEG_FOUND ON)
                set(_REX_FFMPEG_INCLUDE_DIRS ${REX_FFMPEG_BINK_INCLUDE_DIRS})
                set(_REX_FFMPEG_LIBRARIES ${REX_FFMPEG_BINK_LIBRARIES})
            endif()
        endif()
    endif()
    if(_REX_FFMPEG_FOUND)
        target_compile_definitions(rexruntime PUBLIC REX_HAS_FFMPEG=1)
        # Headers only — runtime symbols are dlopen'd at first use, into a
        # private namespace, so the dynamic libavcodec doesn't shadow the
        # static libavcodec we link for XMA. (Fedora's libavcodec ships
        # without XMA, so a normal dynamic link breaks game audio.)
        target_include_directories(rexruntime PRIVATE ${_REX_FFMPEG_INCLUDE_DIRS})
        target_link_libraries(rexruntime PRIVATE dl)
        message(STATUS "rexruntime: Bink overlay enabled (dlopen'd at runtime), "
                       "headers=${_REX_FFMPEG_INCLUDE_DIRS}")
    else()
        message(STATUS "rexruntime: FFmpeg not found, Bink overlay is a no-op "
                       "(set REX_FFMPEG_DIR to a tree with ffmpeg/* headers + lib/lib*.so to enable)")
    endif()
endif()

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        SDL3::SDL3
)

if(REXGLUE_ENABLE_TRACY)
    target_link_libraries(rexruntime PUBLIC TracyClient)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
