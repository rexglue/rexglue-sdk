#==========================================================
# rexglue_configure_target() - Configure a consumer target
# with platform-specific settings and SDK source files.
#
# Usage:
#   rexglue_configure_target(<target>)
#
# Adds:
#   - Platform entry point source (windowed_app_main_*.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - Platform-specific link/compile settings
#==========================================================
function(rexglue_configure_target target_name)
    # Platform entry point
    if(WIN32)
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_win.cpp)
    elseif(APPLE)
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_macos.mm)
        set_source_files_properties(
            ${REXGLUE_SHARE_DIR}/windowed_app_main_macos.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )
    else()
        target_sources(${target_name} PRIVATE
            ${REXGLUE_SHARE_DIR}/windowed_app_main_posix.cpp)
    endif()

    # ReXApp base class
    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    # Linux platform settings
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        target_include_directories(${target_name} PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(${target_name} PRIVATE ${GTK3_LIBRARIES})

        # Whole-archive linking for kernel hooks
        target_link_options(${target_name} PRIVATE
            -Wl,--whole-archive
            $<TARGET_FILE:rex::kernel>
            -Wl,--no-whole-archive
        )
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    elseif(APPLE)
        target_link_libraries(${target_name} PRIVATE
            "-framework Cocoa"
            "-framework QuartzCore"
        )
    endif()

    if(NOT MSVC
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"
       AND NOT (APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"))
        target_compile_options(${target_name} PRIVATE -msse4.1)
    endif()
endfunction()
