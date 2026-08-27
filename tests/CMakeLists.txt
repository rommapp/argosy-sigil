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
sigil_add_test(unit_xex)
sigil_add_test(unit_wii_wbfs)
sigil_add_test(unit_dreamcast)
sigil_add_test(unit_xbox)
sigil_add_test(unit_ps3_iso)
sigil_add_test(unit_3ds)
sigil_add_test(unit_wiiu)
sigil_add_test(unit_zip)
sigil_add_test(unit_zar)
sigil_add_test(unit_switch_nca)
sigil_add_test(unit_switch_cnmt)

# Fixtures have to produce what the extractors read, so these link the
# compression and crypto helpers directly rather than relying on reaching them
# through sigil. That indirection only ever worked for a static build; a shared
# libsigil keeps those dependencies private and the tests fail to link.
target_link_libraries(unit_zip PRIVATE sigil_zlib)
target_link_libraries(unit_zar PRIVATE sigil_zstd_compress)
target_link_libraries(unit_switch_cnmt PRIVATE sigil_aes)

# save_id case is emitted by the extractors AND by the filename fallback, so
# these carry assertions against both paths.
foreach(t unit_wii_wbfs unit_zip unit_3ds unit_wiiu)
    target_compile_definitions(${t} PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
endforeach()
