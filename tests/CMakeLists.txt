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
sigil_add_test(integration_switch)
sigil_add_test(integration_wiiu)
sigil_add_test(integration_3ds)
sigil_add_test(unit_sfo)
sigil_add_test(unit_xex)
sigil_add_test(unit_3ds)
sigil_add_test(unit_switch_nca)
sigil_add_test(unit_wii_wbfs)

if(SIGIL_WITH_WIIU)
    sigil_add_test(unit_wiiu)
endif()

# The CNMT fixture builds encrypted NCA sections, so it needs tiny-AES's ECB
# helpers directly (in addition to sigil's exported CTR/XTS wrappers).
if(SIGIL_WITH_SWITCH)
    sigil_add_test(unit_switch_cnmt)
    target_link_libraries(unit_switch_cnmt PRIVATE sigil_aes)
endif()

# save_id case is emitted by the extractors AND by the filename fallback; the
# fallback assertions only compile when that scanner is in the build.
if(SIGIL_WITH_FILENAME)
    target_compile_definitions(unit_wii_wbfs PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    if(SIGIL_WITH_WIIU)
        target_compile_definitions(unit_wiiu PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
    endif()
endif()
