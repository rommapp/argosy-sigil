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
sigil_add_test(integration_3ds)
sigil_add_test(integration_wiiu)
sigil_add_test(integration_switch)
sigil_add_test(unit_sfo)
sigil_add_test(unit_cnf)
sigil_add_test(unit_xex)
sigil_add_test(unit_wii_wbfs)
sigil_add_test(unit_wii_wad)
sigil_add_test(unit_gamecube)
sigil_add_test(unit_filename)
sigil_add_test(unit_dreamcast)
sigil_add_test(unit_xbox)
sigil_add_test(unit_ps3_iso)
sigil_add_test(unit_3ds)
sigil_add_test(unit_wiiu)
sigil_add_test(unit_zip)
sigil_add_test(unit_zar)
sigil_add_test(unit_switch_nca)
sigil_add_test(unit_switch_cnmt)
sigil_add_test(unit_gb)
sigil_add_test(unit_snes)
sigil_add_test(unit_save_unit)

# Checks the save samples against their manifests. It hashes the files with
# sigil's md5 compiled in directly, so it doesn't link the library it will
# later be used to test.
add_executable(integration_save_corpus integration_save_corpus.c ${CMAKE_SOURCE_DIR}/src/md5.c)
target_include_directories(integration_save_corpus PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(integration_save_corpus PRIVATE
    SIGIL_SAVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/saves")
add_test(NAME integration_save_corpus COMMAND integration_save_corpus)
set_tests_properties(integration_save_corpus PROPERTIES SKIP_RETURN_CODE 77)

# Card tests read the save samples and reach the card internals, which the
# static library tests are built against expose.
foreach(t integration_card_ps1 integration_card_ps2 integration_card_gamecube
          integration_card_dreamcast integration_card_saturn integration_card_segacd
          integration_clock_gb integration_sync_ps1 integration_sync_ps2
          integration_sync_volumes integration_sync_dreamcast integration_sync_gamecube
          integration_sync_companions)
    sigil_add_test(${t})
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${t} PRIVATE
        SIGIL_SAVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/saves")
endforeach()

# Fixtures have to produce what the extractors read, so these link the
# compression and crypto helpers directly rather than relying on reaching them
# through sigil. That indirection only ever worked for a static build; a shared
# libsigil keeps those dependencies private and the tests fail to link.
target_link_libraries(unit_zip PRIVATE sigil_zlib)
target_link_libraries(unit_zar PRIVATE sigil_zstd_compress)
target_link_libraries(unit_switch_cnmt PRIVATE sigil_aes)

sigil_add_test(unit_save_names)
target_include_directories(unit_save_names PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The JNI binding's name escaping is plain C, tested here without a JDK.
sigil_add_test(unit_jni_names)
target_include_directories(unit_jni_names PRIVATE ${CMAKE_SOURCE_DIR}/bindings/android/src/main/cpp)

# save_id case is emitted by the extractors AND by the filename fallback, so
# these carry assertions against both paths.
foreach(t unit_wii_wbfs unit_zip unit_3ds unit_wiiu)
    target_compile_definitions(${t} PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
endforeach()
