# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.16)
project(sigil C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# glibc gates fseeko/ftello/off_t under strict C99; implicit declarations
# truncate 64-bit offsets (CHD reads past 2 GiB fail). Must cover libchdr too.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _FILE_OFFSET_BITS=64)
endif()

# ---- Feature flags -----------------------------------------------------------
option(SIGIL_WITH_CHD       "Build CHD support (libchdr + zlib/lzma/zstd)" ON)
option(SIGIL_WITH_CSO       "Build PSP .cso/.ciso v1 IO (zlib raw deflate)" ON)
option(SIGIL_WITH_SWITCH    "Build Switch NSP/XCI/NCA + AES-XTS"           ON)
option(SIGIL_WITH_WIIU      "Build Wii U WUA reader"                       ON)
option(SIGIL_WITH_3DS       "Build 3DS NCSD + .z3ds/.zcci zstd"            ON)
option(SIGIL_WITH_FILENAME  "Build filename fallback scanners"             ON)
option(SIGIL_BUILD_CLI      "Build sigil(1) CLI"                           ON)
option(SIGIL_BUILD_TESTS    "Build tests"                                  ON)
option(SIGIL_BUILD_SHARED   "Build libsigil as a shared library"           OFF)

set(SIGIL_NEEDS_ZLIB OFF)
if(SIGIL_WITH_CHD OR SIGIL_WITH_CSO)
    set(SIGIL_NEEDS_ZLIB ON)
endif()

# zstd is shared between libchdr's needs (CHD codecs) and sigil's own
# decompression for Wii U WUA + 3DS .z3ds/.zcci.
set(SIGIL_NEEDS_ZSTD OFF)
if(SIGIL_WITH_CHD OR SIGIL_WITH_WIIU OR SIGIL_WITH_3DS)
    set(SIGIL_NEEDS_ZSTD ON)
endif()

# ---- Vendored deps -----------------------------------------------------------
set(LIBCHDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr)

if(SIGIL_NEEDS_ZSTD)
    set(ZSTD_DIR ${LIBCHDR_DIR}/deps/zstd-1.5.6/lib)
    file(GLOB ZSTD_COMMON_SOURCES ${ZSTD_DIR}/common/*.c)
    file(GLOB ZSTD_DECOMPRESS_SOURCES ${ZSTD_DIR}/decompress/*.c)
    add_library(sigil_zstd STATIC ${ZSTD_COMMON_SOURCES} ${ZSTD_DECOMPRESS_SOURCES})
    target_include_directories(sigil_zstd PUBLIC ${ZSTD_DIR} ${ZSTD_DIR}/common)
endif()

if(SIGIL_NEEDS_ZLIB)
    # zlib (core only) -- shared between libchdr's CHD codecs and io_cso's raw deflate.
    set(ZLIB_DIR ${LIBCHDR_DIR}/deps/zlib-1.3.1)
    add_library(sigil_zlib STATIC
        ${ZLIB_DIR}/adler32.c
        ${ZLIB_DIR}/compress.c
        ${ZLIB_DIR}/crc32.c
        ${ZLIB_DIR}/deflate.c
        ${ZLIB_DIR}/infback.c
        ${ZLIB_DIR}/inffast.c
        ${ZLIB_DIR}/inflate.c
        ${ZLIB_DIR}/inftrees.c
        ${ZLIB_DIR}/trees.c
        ${ZLIB_DIR}/uncompr.c
        ${ZLIB_DIR}/zutil.c)
    target_include_directories(sigil_zlib PUBLIC ${ZLIB_DIR})
endif()

if(SIGIL_WITH_CHD)
    # lzma
    set(LZMA_DIR ${LIBCHDR_DIR}/deps/lzma-24.05)
    add_library(sigil_lzma STATIC
        ${LZMA_DIR}/src/Alloc.c
        ${LZMA_DIR}/src/Bra86.c
        ${LZMA_DIR}/src/BraIA64.c
        ${LZMA_DIR}/src/CpuArch.c
        ${LZMA_DIR}/src/Delta.c
        ${LZMA_DIR}/src/LzFind.c
        ${LZMA_DIR}/src/Lzma86Dec.c
        ${LZMA_DIR}/src/LzmaDec.c
        ${LZMA_DIR}/src/Sort.c)
    target_include_directories(sigil_lzma PUBLIC ${LZMA_DIR}/include)
    target_compile_definitions(sigil_lzma PRIVATE _7ZIP_ST)

    # libchdr
    add_library(sigil_chdr STATIC
        ${LIBCHDR_DIR}/src/libchdr_bitstream.c
        ${LIBCHDR_DIR}/src/libchdr_cdrom.c
        ${LIBCHDR_DIR}/src/libchdr_chd.c
        ${LIBCHDR_DIR}/src/libchdr_flac.c
        ${LIBCHDR_DIR}/src/libchdr_huffman.c)
    target_include_directories(sigil_chdr PUBLIC ${LIBCHDR_DIR}/include)
    target_link_libraries(sigil_chdr PRIVATE sigil_zlib sigil_lzma sigil_zstd)
    target_compile_definitions(sigil_chdr PRIVATE FLAC__NO_DLL FLAC__HAS_OGG=0)
endif()

if(SIGIL_WITH_SWITCH)
    set(TINY_AES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tiny-AES-c)
    add_library(sigil_aes STATIC ${TINY_AES_DIR}/aes.c)
    target_include_directories(sigil_aes PUBLIC ${TINY_AES_DIR})
    target_compile_definitions(sigil_aes PUBLIC AES128=1 CBC=0 CTR=1 ECB=1)
endif()

# ---- libsigil ----------------------------------------------------------------
set(SIGIL_SOURCES
    src/sigil.c
    src/iso9660.c
    src/cnf_parser.c
    src/sfo_parser.c
    src/io_file.c
    src/psp.c
    src/psx.c
    src/ps2.c
    src/ps3.c
    src/psvita.c
    src/wii.c
    src/xbox360.c
    src/dreamcast.c
)

if(SIGIL_WITH_CHD)
    list(APPEND SIGIL_SOURCES src/io_chd.c src/io_raw_cd.c)
endif()
if(SIGIL_WITH_CSO)
    list(APPEND SIGIL_SOURCES src/io_cso.c)
endif()
if(SIGIL_WITH_SWITCH)
    list(APPEND SIGIL_SOURCES
        src/aes_xts.c
        src/switch_keys.c
        src/switch_nsp.c
        src/switch_xci.c
        src/switch_nca.c
        src/switch_cnmt.c)
endif()
if(SIGIL_WITH_WIIU)
    list(APPEND SIGIL_SOURCES src/wiiu_wua.c)
endif()
if(SIGIL_WITH_3DS)
    list(APPEND SIGIL_SOURCES src/threeds.c)
endif()
if(SIGIL_WITH_FILENAME)
    list(APPEND SIGIL_SOURCES src/filename.c)
endif()

if(SIGIL_BUILD_SHARED)
    add_library(sigil SHARED ${SIGIL_SOURCES})
    # SIGIL_EXPORTS marks the build of the library itself (dllexport /
    # default visibility); SIGIL_SHARED tells consumers to dllimport.
    target_compile_definitions(sigil
        PRIVATE SIGIL_EXPORTS
        INTERFACE SIGIL_SHARED)
    set_target_properties(sigil PROPERTIES VERSION 0.1.0 SOVERSION 0)
else()
    add_library(sigil STATIC ${SIGIL_SOURCES})
endif()
target_include_directories(sigil PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(SIGIL_WITH_CHD)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_CHD=1)
    target_link_libraries(sigil PRIVATE sigil_chdr)
endif()
if(SIGIL_WITH_CSO)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_CSO=1)
    target_link_libraries(sigil PRIVATE sigil_zlib)
endif()
if(SIGIL_WITH_SWITCH)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_SWITCH=1)
    target_link_libraries(sigil PRIVATE sigil_aes)
endif()
if(SIGIL_WITH_WIIU)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_WIIU=1)
    # WUA title-ID extraction reads only the uncompressed file tree + name
    # table; the zstd-compressed blocks hold file *contents* and aren't
    # needed for our purpose.
endif()
if(SIGIL_WITH_3DS)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_3DS=1)
    target_link_libraries(sigil PRIVATE sigil_zstd)
endif()
if(SIGIL_WITH_FILENAME)
    target_compile_definitions(sigil PRIVATE SIGIL_WITH_FILENAME=1)
endif()

# ---- CLI ---------------------------------------------------------------------
if(SIGIL_BUILD_CLI)
    add_executable(sigil_cli cli/sigil.c)
    target_link_libraries(sigil_cli PRIVATE sigil)
    set_target_properties(sigil_cli PROPERTIES OUTPUT_NAME sigil)
endif()

# ---- Tests -------------------------------------------------------------------
if(SIGIL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
