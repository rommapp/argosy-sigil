# SPDX-License-Identifier: MPL-2.0
# Test harness — both synthetic unit tests and real-ROM integration tests.

# Integration tests are gated on SIGIL_ROM_DIR (and SIGIL_PROD_KEYS for Switch);
# they skip cleanly when the env vars aren't set so the public CI without
# ROMs can still pass.

# Helper: define a test that links against sigil and reports skip vs fail
# uniformly via exit code conventions.
function(sigil_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE sigil)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME ${name} COMMAND ${name})
    # Skip is signaled by exit code 77 (autotools convention).
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

sigil_add_test(integration_psp)
sigil_add_test(integration_psx)
sigil_add_test(integration_ps2)
sigil_add_test(integration_wii)
sigil_add_test(integration_gamecube)
sigil_add_test(integration_psvita)
sigil_add_test(integration_dreamcast)
sigil_add_test(unit_sfo)
sigil_add_test(unit_xex)
sigil_add_test(unit_wii_wbfs)
sigil_add_test(unit_dreamcast)
sigil_add_test(unit_xbox)
sigil_add_test(unit_ps3_iso)

# A test for a platform whose sources are dropped by a feature flag would
# either fail or fail to link, so each follows its own flag.
if(SIGIL_WITH_3DS)
    sigil_add_test(unit_3ds)
    sigil_add_test(integration_3ds)
endif()

# The zip fixture is deflated at test time, so it needs zlib's headers. The
# archive itself arrives transitively through sigil, so linking it again here
# would only put the same static library on the command line twice.
if(SIGIL_WITH_ZIP)
    sigil_add_test(unit_zip)
    target_include_directories(unit_zip PRIVATE
        $<TARGET_PROPERTY:sigil_zlib,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Same arrangement for the ZArchive fixture, which zstd-compresses its blocks
# at test time.
if(SIGIL_WITH_ZARCHIVE)
    sigil_add_test(unit_zar)
    target_link_libraries(unit_zar PRIVATE sigil_zstd_compress)
endif()

if(SIGIL_WITH_WIIU)
    sigil_add_test(unit_wiiu)
    sigil_add_test(integration_wiiu)
endif()

# The NCA and CNMT fixtures build encrypted sections, so they need the AES
# helpers directly: tiny-AES's ECB for CNMT, and sigil's XTS *encrypt* half,
# which exists only to let tests produce what the extractor reads.
if(SIGIL_WITH_SWITCH)
    sigil_add_test(unit_switch_nca)
    sigil_add_test(unit_switch_cnmt)
    sigil_add_test(integration_switch)
    target_link_libraries(unit_switch_cnmt PRIVATE sigil_aes)
endif()

# save_id case is emitted by the extractors AND by the filename fallback; the
# fallback assertions only compile when that scanner is in the build.
if(SIGIL_WITH_FILENAME)
    target_compile_definitions(unit_wii_wbfs PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    if(SIGIL_WITH_ZIP)
        target_compile_definitions(unit_zip PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    endif()
    if(SIGIL_WITH_3DS)
        target_compile_definitions(unit_3ds PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    endif()
    if(SIGIL_WITH_WIIU)
        target_compile_definitions(unit_wiiu PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    endif()
endif()
