set(REXKERNEL_SOURCES
    kernel_init.cpp
    exports.cpp

    # XAM subsystem - provides __imp__Xam* symbols
    xam/xam_module.cpp
    xam/xam_avatar.cpp
    xam/xam_content.cpp
    xam/xam_content_aggregate.cpp
    xam/xam_content_device.cpp
    xam/xam_debug.cpp
    xam/xam_enum.cpp
    xam/xam_info.cpp
    xam/xam_input.cpp
    xam/xam_locale.cpp
    xam/xam_msg.cpp
    xam/xam_net.cpp
    xam/xam_notify.cpp
    xam/xam_nui.cpp
    xam/xam_party.cpp
    xam/xam_task.cpp
    xam/xam_ui.cpp
    xam/xam_user.cpp
    xam/xam_video.cpp
    xam/xam_voice.cpp
    xam/xam_stubs.cpp
    xam/xam_file.cpp
    xam/xam_memory.cpp
    xam/xam_string.cpp
    xam/apps/xam_app.cpp
    xam/apps/xgi_app.cpp
    xam/apps/xlivebase_app.cpp
    xam/apps/xmp_app.cpp

    # XBDM subsystem - provides __imp__Xbdm* symbols
    xbdm/xbdm_module.cpp
    xbdm/xbdm_stubs.cpp

    # xboxkrnl exports - provides __imp__ symbols for generated code
    xboxkrnl/xboxkrnl_crypt.cpp
    xboxkrnl/xboxkrnl_debug.cpp
    xboxkrnl/xboxkrnl_error.cpp
    xboxkrnl/xboxkrnl_hal.cpp
    xboxkrnl/xboxkrnl_hid.cpp
    xboxkrnl/xboxkrnl_io.cpp
    xboxkrnl/xboxkrnl_io_info.cpp
    xboxkrnl/xboxkrnl_memory.cpp
    xboxkrnl/xboxkrnl_stubs.cpp
    xboxkrnl/xboxkrnl_module.cpp
    xboxkrnl/xboxkrnl_modules.cpp
    xboxkrnl/xboxkrnl_ob.cpp
    xboxkrnl/xboxkrnl_rtl.cpp
    xboxkrnl/xboxkrnl_strings.cpp
    xboxkrnl/xboxkrnl_threading.cpp
    xboxkrnl/xboxkrnl_usbcam.cpp
    xboxkrnl/xboxkrnl_video.cpp
    xboxkrnl/xboxkrnl_xconfig.cpp
    xboxkrnl/xboxkrnl_audio.cpp
    xboxkrnl/xboxkrnl_audio_xma.cpp

    shim/heap.cpp
    shim/file.cpp
    shim/memory.cpp
    shim/string.cpp
    shim/threading.cpp
)

list(TRANSFORM REXKERNEL_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(rex-runtime PRIVATE ${REXKERNEL_SOURCES})

set(REXKERNEL_THIRDPARTY_SOURCES
    ${PROJECT_SOURCE_DIR}/thirdparty/crypto/des/des.cpp
    ${PROJECT_SOURCE_DIR}/thirdparty/crypto/sha256.cpp
)
target_sources(rex-runtime PRIVATE ${REXKERNEL_THIRDPARTY_SOURCES})
set_source_files_properties(${REXKERNEL_THIRDPARTY_SOURCES} TARGET_DIRECTORY rex-runtime
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -w)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(REXKERNEL_DES_INCLUDE /FIcstring)
else()
    set(REXKERNEL_DES_INCLUDE -include cstring)
endif()
set_source_files_properties(${PROJECT_SOURCE_DIR}/thirdparty/crypto/des/des.cpp
    TARGET_DIRECTORY rex-runtime PROPERTIES COMPILE_OPTIONS "-w;${REXKERNEL_DES_INCLUDE}")

target_link_libraries(rex-runtime PRIVATE o1heap libavcodec libavutil)
