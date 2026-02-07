# Third-party vendored dependencies for ReXGlue
# Libraries available via vcpkg are no longer defined here

#=============================================================================
# Verify git submodules are initialized (only vendored ones)
#=============================================================================
set(REQUIRED_SUBMODULES
    abseil-cpp
    libmspack
    FFmpeg
    glslang
    tomlplusplus
    simde
    xxHash
)

foreach(submodule ${REQUIRED_SUBMODULES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${submodule}/.git")
        message(FATAL_ERROR
            "Git submodule '${submodule}' is not initialized.\n"
            "Please run: git submodule update --init --recursive\n"
            "from the repository root directory."
        )
    endif()
endforeach()

#=============================================================================
# Header-only libraries (vendored, not in vcpkg)
#=============================================================================

# disruptorplus - lock-free queue (header-only)
add_library(disruptorplus INTERFACE)
target_include_directories(disruptorplus INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/disruptorplus/include>
    $<INSTALL_INTERFACE:include/disruptorplus>)

# renderdoc - RenderDoc API header (header-only)
add_library(renderdoc INTERFACE)
target_include_directories(renderdoc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/renderdoc>
    $<INSTALL_INTERFACE:include/renderdoc>)

# tomlplusplus - TOML config file parser (header-only)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# simde - SIMD Everywhere (header-only, git submodule)
add_library(simde INTERFACE)
target_include_directories(simde INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simde>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simde/simde>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

#=============================================================================
# Compiled libraries (vendored sources)
#=============================================================================

# tiny-AES-c - AES encryption (used for XEX decryption)
add_library(tiny-aes STATIC
    tiny-aes-c/aes.c
)
target_include_directories(tiny-aes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tiny-aes-c)
target_compile_definitions(tiny-aes PUBLIC AES128=1 CBC=1)

# aes_128 - AES-128 encryption/decryption (used by kernel crypt routines)
# From: https://github.com/openluopworld/aes_128
add_library(aes128 STATIC
    aes_128/aes.c
)
target_include_directories(aes128 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Suppress warnings in third-party C code
target_compile_options(aes128 PRIVATE -w)

# libmspack - LZX decompression (only lzxd.c needed for XEX decompression)
# Uses cabextract version which has all needed headers in one place
set(MSPACK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libmspack/cabextract/mspack")
add_library(mspack STATIC
    ${MSPACK_DIR}/lzxd.c
)
target_include_directories(mspack PUBLIC
    $<BUILD_INTERFACE:${MSPACK_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Suppress warnings in third-party C code
target_compile_options(mspack PRIVATE -w)

# xxHash - extremely fast non-cryptographic hash (git submodule)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
add_subdirectory(xxHash/cmake_unofficial)
target_compile_options(xxhash PRIVATE -w)

# disasm - PowerPC disassembler (existing local library)
add_subdirectory(disasm)
# Suppress warnings in disasm
target_compile_options(disasm PRIVATE -w)

#=============================================================================
# glslang/SPIRV - SPIR-V code generation for shader translation
#=============================================================================

# glslang is used only for its SPIR-V building capabilities
# We disable everything we don't need to minimize build time
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
# Enable RTTI for spv::Builder inheritance in SpirvBuilder
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)

# Add glslang subdirectory
# Set policy minimum for older glslang versions with outdated cmake_minimum_required
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory(glslang)
# Create alias for older glslang versions that don't provide it
if(NOT TARGET glslang::SPIRV)
    add_library(glslang::SPIRV ALIAS SPIRV)
endif()

#=============================================================================
# abseil-cpp - Google's C++ common libraries
#=============================================================================
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(abseil-cpp)

#=============================================================================
# FFmpeg - Audio/video codec library (for XMA decoding)
# Only builds libavutil and libavcodec needed for WMA Pro decoding
#=============================================================================

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg")

# Common defines for both libraries
set(FFMPEG_COMMON_DEFINES
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES  # For M_PI/etc
)

# Platform-specific include paths for atomics
if(WIN32)
    set(FFMPEG_ATOMICS_INCLUDE "${FFMPEG_DIR}/compat/atomics/win32")
else()
    set(FFMPEG_ATOMICS_INCLUDE "${FFMPEG_DIR}/compat/atomics/gcc")
endif()

# libavutil - FFmpeg utility library (must build first)
add_library(libavutil STATIC
    ${FFMPEG_DIR}/libavutil/adler32.c
    ${FFMPEG_DIR}/libavutil/aes.c
    ${FFMPEG_DIR}/libavutil/aes_ctr.c
    ${FFMPEG_DIR}/libavutil/audio_fifo.c
    ${FFMPEG_DIR}/libavutil/avstring.c
    ${FFMPEG_DIR}/libavutil/avsscanf.c
    ${FFMPEG_DIR}/libavutil/base64.c
    ${FFMPEG_DIR}/libavutil/blowfish.c
    ${FFMPEG_DIR}/libavutil/bprint.c
    ${FFMPEG_DIR}/libavutil/buffer.c
    ${FFMPEG_DIR}/libavutil/cast5.c
    ${FFMPEG_DIR}/libavutil/camellia.c
    ${FFMPEG_DIR}/libavutil/channel_layout.c
    ${FFMPEG_DIR}/libavutil/color_utils.c
    ${FFMPEG_DIR}/libavutil/cpu.c
    ${FFMPEG_DIR}/libavutil/crc.c
    ${FFMPEG_DIR}/libavutil/des.c
    ${FFMPEG_DIR}/libavutil/dict.c
    ${FFMPEG_DIR}/libavutil/display.c
    ${FFMPEG_DIR}/libavutil/dovi_meta.c
    ${FFMPEG_DIR}/libavutil/downmix_info.c
    ${FFMPEG_DIR}/libavutil/encryption_info.c
    ${FFMPEG_DIR}/libavutil/error.c
    ${FFMPEG_DIR}/libavutil/eval.c
    ${FFMPEG_DIR}/libavutil/fifo.c
    ${FFMPEG_DIR}/libavutil/file.c
    ${FFMPEG_DIR}/libavutil/file_open.c
    ${FFMPEG_DIR}/libavutil/float_dsp.c
    ${FFMPEG_DIR}/libavutil/fixed_dsp.c
    ${FFMPEG_DIR}/libavutil/frame.c
    ${FFMPEG_DIR}/libavutil/hash.c
    ${FFMPEG_DIR}/libavutil/hdr_dynamic_metadata.c
    ${FFMPEG_DIR}/libavutil/hmac.c
    ${FFMPEG_DIR}/libavutil/hwcontext.c
    ${FFMPEG_DIR}/libavutil/imgutils.c
    ${FFMPEG_DIR}/libavutil/integer.c
    ${FFMPEG_DIR}/libavutil/intmath.c
    ${FFMPEG_DIR}/libavutil/lfg.c
    ${FFMPEG_DIR}/libavutil/lls.c
    ${FFMPEG_DIR}/libavutil/log.c
    ${FFMPEG_DIR}/libavutil/log2_tab.c
    ${FFMPEG_DIR}/libavutil/mathematics.c
    ${FFMPEG_DIR}/libavutil/mastering_display_metadata.c
    ${FFMPEG_DIR}/libavutil/md5.c
    ${FFMPEG_DIR}/libavutil/mem.c
    ${FFMPEG_DIR}/libavutil/murmur3.c
    ${FFMPEG_DIR}/libavutil/opt.c
    ${FFMPEG_DIR}/libavutil/parseutils.c
    ${FFMPEG_DIR}/libavutil/pixdesc.c
    ${FFMPEG_DIR}/libavutil/pixelutils.c
    ${FFMPEG_DIR}/libavutil/random_seed.c
    ${FFMPEG_DIR}/libavutil/rational.c
    ${FFMPEG_DIR}/libavutil/reverse.c
    ${FFMPEG_DIR}/libavutil/rc4.c
    ${FFMPEG_DIR}/libavutil/ripemd.c
    ${FFMPEG_DIR}/libavutil/samplefmt.c
    ${FFMPEG_DIR}/libavutil/sha.c
    ${FFMPEG_DIR}/libavutil/sha512.c
    ${FFMPEG_DIR}/libavutil/slicethread.c
    ${FFMPEG_DIR}/libavutil/spherical.c
    ${FFMPEG_DIR}/libavutil/stereo3d.c
    ${FFMPEG_DIR}/libavutil/threadmessage.c
    ${FFMPEG_DIR}/libavutil/time.c
    ${FFMPEG_DIR}/libavutil/timecode.c
    ${FFMPEG_DIR}/libavutil/tree.c
    ${FFMPEG_DIR}/libavutil/twofish.c
    ${FFMPEG_DIR}/libavutil/utils.c
    ${FFMPEG_DIR}/libavutil/xga_font_data.c
    ${FFMPEG_DIR}/libavutil/xtea.c
    ${FFMPEG_DIR}/libavutil/tea.c
    ${FFMPEG_DIR}/libavutil/tx.c
    ${FFMPEG_DIR}/libavutil/tx_float.c
    ${FFMPEG_DIR}/libavutil/tx_double.c
    ${FFMPEG_DIR}/libavutil/tx_int32.c
    ${FFMPEG_DIR}/libavutil/video_enc_params.c
    ${FFMPEG_DIR}/libavutil/film_grain_params.c
)

# Add x86 optimizations on Windows/Linux
if(WIN32 OR (UNIX AND NOT APPLE))
    target_sources(libavutil PRIVATE
        ${FFMPEG_DIR}/libavutil/x86/cpu.c
        ${FFMPEG_DIR}/libavutil/x86/fixed_dsp_init.c
        ${FFMPEG_DIR}/libavutil/x86/float_dsp_init.c
        ${FFMPEG_DIR}/libavutil/x86/imgutils_init.c
        ${FFMPEG_DIR}/libavutil/x86/lls_init.c
    )
endif()

target_include_directories(libavutil
    PUBLIC
        $<BUILD_INTERFACE:${FFMPEG_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${FFMPEG_ATOMICS_INCLUDE}
)
target_compile_definitions(libavutil PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavutil PRIVATE -w)  # Suppress warnings in third-party code
if(WIN32)
    target_link_libraries(libavutil PRIVATE bcrypt)
endif()

# libavcodec - FFmpeg codec library (depends on libavutil)
add_library(libavcodec STATIC
    ${FFMPEG_DIR}/libavcodec/ac3_parser.c
    ${FFMPEG_DIR}/libavcodec/adts_parser.c
    ${FFMPEG_DIR}/libavcodec/allcodecs.c
    ${FFMPEG_DIR}/libavcodec/avcodec.c
    ${FFMPEG_DIR}/libavcodec/avdct.c
    ${FFMPEG_DIR}/libavcodec/avpacket.c
    ${FFMPEG_DIR}/libavcodec/avpicture.c
    ${FFMPEG_DIR}/libavcodec/bitstream.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filter.c
    ${FFMPEG_DIR}/libavcodec/bitstream_filters.c
    ${FFMPEG_DIR}/libavcodec/bsf.c
    ${FFMPEG_DIR}/libavcodec/codec_desc.c
    ${FFMPEG_DIR}/libavcodec/codec_par.c
    ${FFMPEG_DIR}/libavcodec/d3d11va.c
    ${FFMPEG_DIR}/libavcodec/decode.c
    ${FFMPEG_DIR}/libavcodec/dirac.c
    ${FFMPEG_DIR}/libavcodec/dv_profile.c
    ${FFMPEG_DIR}/libavcodec/encode.c
    ${FFMPEG_DIR}/libavcodec/imgconvert.c
    ${FFMPEG_DIR}/libavcodec/jni.c
    ${FFMPEG_DIR}/libavcodec/mathtables.c
    ${FFMPEG_DIR}/libavcodec/mediacodec.c
    ${FFMPEG_DIR}/libavcodec/mpeg12framerate.c
    ${FFMPEG_DIR}/libavcodec/options.c
    ${FFMPEG_DIR}/libavcodec/parser.c
    ${FFMPEG_DIR}/libavcodec/parsers.c
    ${FFMPEG_DIR}/libavcodec/profiles.c
    ${FFMPEG_DIR}/libavcodec/qsv_api.c
    ${FFMPEG_DIR}/libavcodec/raw.c
    ${FFMPEG_DIR}/libavcodec/utils.c
    ${FFMPEG_DIR}/libavcodec/vorbis_parser.c
    ${FFMPEG_DIR}/libavcodec/xiph.c
    ${FFMPEG_DIR}/libavcodec/faandct.c
    ${FFMPEG_DIR}/libavcodec/faanidct.c
    ${FFMPEG_DIR}/libavcodec/fdctdsp.c
    ${FFMPEG_DIR}/libavcodec/jfdctfst.c
    ${FFMPEG_DIR}/libavcodec/jfdctint.c
    ${FFMPEG_DIR}/libavcodec/idctdsp.c
    ${FFMPEG_DIR}/libavcodec/simple_idct.c
    ${FFMPEG_DIR}/libavcodec/jrevdct.c
    ${FFMPEG_DIR}/libavcodec/mdct_float.c
    ${FFMPEG_DIR}/libavcodec/mdct_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/sinewin.c
    ${FFMPEG_DIR}/libavcodec/wma_freqs.c
    ${FFMPEG_DIR}/libavcodec/wmaprodec.c
    ${FFMPEG_DIR}/libavcodec/wma.c
    ${FFMPEG_DIR}/libavcodec/wma_common.c
    ${FFMPEG_DIR}/libavcodec/null_bsf.c
    ${FFMPEG_DIR}/libavcodec/pthread.c
    ${FFMPEG_DIR}/libavcodec/pthread_slice.c
    ${FFMPEG_DIR}/libavcodec/pthread_frame.c
    ${FFMPEG_DIR}/libavcodec/avfft.c
    ${FFMPEG_DIR}/libavcodec/fft_float.c
    ${FFMPEG_DIR}/libavcodec/fft_fixed_32.c
    ${FFMPEG_DIR}/libavcodec/fft_init_table.c
)

# Windows-specific source
if(WIN32)
    target_sources(libavcodec PRIVATE
        ${FFMPEG_DIR}/libavcodec/file_open.c
    )
endif()

# Add x86 optimizations on Windows/Linux
if(WIN32 OR (UNIX AND NOT APPLE))
    target_sources(libavcodec PRIVATE
        ${FFMPEG_DIR}/libavcodec/x86/constants.c
        ${FFMPEG_DIR}/libavcodec/x86/fdctdsp_init.c
        ${FFMPEG_DIR}/libavcodec/x86/fft_init.c
        ${FFMPEG_DIR}/libavcodec/x86/idctdsp_init.c
        ${FFMPEG_DIR}/libavcodec/x86/fdct.c
    )
endif()

target_include_directories(libavcodec
    PUBLIC
        $<BUILD_INTERFACE:${FFMPEG_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${FFMPEG_ATOMICS_INCLUDE}
)
target_compile_definitions(libavcodec PRIVATE ${FFMPEG_COMMON_DEFINES})
target_compile_options(libavcodec PRIVATE -w)  # Suppress warnings in third-party code
target_link_libraries(libavcodec PUBLIC libavutil)
if(WIN32)
    target_link_libraries(libavcodec PRIVATE bcrypt)
endif()

# IDE folder organization
set_target_properties(libavutil libavcodec PROPERTIES FOLDER "FFmpeg")
