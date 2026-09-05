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

# ---- Build options -----------------------------------------------------------
# Every platform and container reader is always compiled. Sigil is embedded by
# larger applications, which expect a format to work rather than to discover at
# integration time that a build flag dropped it. The options below choose what
# gets produced, not what sigil can read.
option(SIGIL_BUILD_CLI      "Build sigil(1) CLI"                           ON)
option(SIGIL_BUILD_TESTS    "Build tests"                                  ON)
option(SIGIL_BUILD_SHARED   "Build libsigil as a shared library"           OFF)

# ---- Vendored deps -----------------------------------------------------------
set(LIBCHDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr)

set(ZSTD_DIR ${LIBCHDR_DIR}/deps/zstd-1.5.6/lib)
file(GLOB ZSTD_COMMON_SOURCES ${ZSTD_DIR}/common/*.c)
file(GLOB ZSTD_DECOMPRESS_SOURCES ${ZSTD_DIR}/decompress/*.c)
add_library(sigil_zstd STATIC ${ZSTD_COMMON_SOURCES} ${ZSTD_DECOMPRESS_SOURCES})
target_include_directories(sigil_zstd PUBLIC ${ZSTD_DIR} ${ZSTD_DIR}/common)
# The sources are globbed as C, which leaves zstd's amd64 huffman assembly out of the build
# while the C still calls into it. Decompressing without it is the portable half of zstd and
# the only half sigil uses.
target_compile_definitions(sigil_zstd PRIVATE ZSTD_DISABLE_ASM=1)

# sigil only ever decompresses, so the compressor is built for tests alone,
# where fixtures have to be produced in the same format they will be read.
if(SIGIL_BUILD_TESTS)
    file(GLOB ZSTD_COMPRESS_SOURCES ${ZSTD_DIR}/compress/*.c)
    add_library(sigil_zstd_compress STATIC ${ZSTD_COMPRESS_SOURCES})
    target_include_directories(sigil_zstd_compress PUBLIC ${ZSTD_DIR} ${ZSTD_DIR}/common)
    target_link_libraries(sigil_zstd_compress PUBLIC sigil_zstd)
endif()

# zlib (core only) -- shared between libchdr's CHD codecs and the raw-deflate
# readers for PSP .cso and zip members.
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

set(TINY_AES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tiny-AES-c)
add_library(sigil_aes STATIC ${TINY_AES_DIR}/aes.c)
target_include_directories(sigil_aes PUBLIC ${TINY_AES_DIR})
target_compile_definitions(sigil_aes PUBLIC AES128=1 CBC=0 CTR=1 ECB=1)

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
    src/xbox.c
    src/xdvdfs.c
    src/threeds.c
    src/wiiu_wua.c
    src/filename.c
    src/zarchive.c
    src/aes_xts.c
    src/switch_keys.c
    src/switch_nsp.c
    src/switch_xci.c
    src/switch_nca.c
    src/switch_cnmt.c
    src/io_file.c
    src/io_chd.c
    src/io_cso.c
    src/io_zip.c
    src/io_zar.c
    src/io_raw_cd.c
)
list(REMOVE_DUPLICATES SIGIL_SOURCES)

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

target_link_libraries(sigil PRIVATE sigil_chdr sigil_zlib sigil_zstd sigil_aes)

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
