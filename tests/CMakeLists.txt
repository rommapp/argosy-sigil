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
sigil_add_test(unit_psvita)
sigil_add_test(unit_dir_scan)
sigil_add_test(unit_3ds)
sigil_add_test(unit_wiiu)
sigil_add_test(unit_zip)
sigil_add_test(unit_zar)
sigil_add_test(unit_switch_nca)
sigil_add_test(unit_switch_cnmt)
sigil_add_test(unit_gb)
sigil_add_test(unit_snes)
sigil_add_test(unit_n64)
target_include_directories(unit_n64 PRIVATE ${CMAKE_SOURCE_DIR}/src)
sigil_add_test(unit_n64_save)
target_include_directories(unit_n64_save PRIVATE ${CMAKE_SOURCE_DIR}/src)
sigil_add_test(unit_sync_gb_mbc2)
target_include_directories(unit_sync_gb_mbc2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
sigil_add_test(unit_rzip)
target_include_directories(unit_rzip PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_rzip PRIVATE sigil_zstd_compress sigil_zlib)
sigil_add_test(unit_save_unit)
sigil_add_test(unit_utf8_paths)

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
          integration_sync_companions integration_sync_profiles integration_sync_n64 integration_sync_gb)
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
target_link_libraries(integration_sync_volumes PRIVATE sigil_zlib)
target_link_libraries(unit_zar PRIVATE sigil_zstd_compress)
target_link_libraries(unit_switch_cnmt PRIVATE sigil_aes)

sigil_add_test(unit_save_names)
target_include_directories(unit_save_names PRIVATE ${CMAKE_SOURCE_DIR}/src)
sigil_add_test(unit_sha1)
target_include_directories(unit_sha1 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(unit_save_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The JNI binding's name escaping is plain C, tested here without a JDK.
sigil_add_test(unit_jni_names)
target_include_directories(unit_jni_names PRIVATE ${CMAKE_SOURCE_DIR}/bindings/android/src/main/cpp)
sigil_add_test(unit_jni_utf16)
target_include_directories(unit_jni_utf16 PRIVATE ${CMAKE_SOURCE_DIR}/bindings/android/src/main/cpp)

# The Go and Python bindings link the static libraries this build makes, so
# their tests run here when the toolchain is there and the build is plain and
# static: a sanitized archive needs its runtime in the host process.
if(NOT SIGIL_BUILD_SHARED AND NOT CMAKE_C_FLAGS MATCHES "-fsanitize")
    # Multi-config generators (Visual Studio) put the archives in a folder per
    # configuration.
    set(sigil_lib_dir "$<TARGET_LINKER_FILE_DIR:sigil>")

    find_program(SIGIL_GO go)
    # cgo links with gcc, which can't take MSVC's .lib archives; on Windows the
    # Go binding needs sigil built with MinGW.
    if(SIGIL_GO AND NOT MSVC)
        # -a: cgo doesn't track include/sigil.h, which sits outside the package,
        # so a cached build keeps the old struct layout after the header changes.
        add_test(NAME binding_go COMMAND ${SIGIL_GO} test -count=1 -a ./...
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/bindings/go)
        set_tests_properties(binding_go PROPERTIES ENVIRONMENT "CGO_LDFLAGS=-L${sigil_lib_dir}")
    elseif(MSVC)
        message(STATUS "Go binding tests off: cgo can't link an MSVC build")
    else()
        message(STATUS "Go binding tests off: no go")
    endif()

    if(WIN32)
        set(venv_python "${CMAKE_SOURCE_DIR}/bindings/python/.venv/Scripts/python.exe")
    else()
        set(venv_python "${CMAKE_SOURCE_DIR}/bindings/python/.venv/bin/python")
    endif()
    set(SIGIL_BINDING_PYTHON "${venv_python}" CACHE FILEPATH
        "Python with cffi and pytest that runs the binding tests")
    set(python_ready 1)
    # CPython on Windows builds extensions with MSVC, which can't link MinGW's archives.
    if(WIN32 AND NOT MSVC)
        message(STATUS "Python binding tests off: CPython on Windows links MSVC builds only")
    elseif(EXISTS "${SIGIL_BINDING_PYTHON}")
        execute_process(COMMAND ${SIGIL_BINDING_PYTHON} -c "import cffi, pytest"
                        RESULT_VARIABLE python_ready OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(python_ready EQUAL 0)
        set(python_dir ${CMAKE_SOURCE_DIR}/bindings/python)
        add_test(NAME binding_python_build COMMAND ${SIGIL_BINDING_PYTHON} build_sigil.py WORKING_DIRECTORY ${python_dir})
        set_tests_properties(binding_python_build PROPERTIES ENVIRONMENT "SIGIL_LIB_DIR=${sigil_lib_dir}"
                             FIXTURES_SETUP sigil_python_extension)
        add_test(NAME binding_python COMMAND ${SIGIL_BINDING_PYTHON} -m pytest -q test_sigil.py WORKING_DIRECTORY ${python_dir})
        set_tests_properties(binding_python PROPERTIES FIXTURES_REQUIRED sigil_python_extension)
        add_test(NAME binding_contract COMMAND ${SIGIL_BINDING_PYTHON} -m pytest -q test_contract.py test_docs.py
                 WORKING_DIRECTORY ${python_dir})
    elseif(MSVC OR NOT WIN32)
        message(STATUS "Python binding tests off: set SIGIL_BINDING_PYTHON to a Python with cffi and pytest")
    endif()
endif()

# save_id case is emitted by the extractors AND by the filename fallback, so
# these carry assertions against both paths.
foreach(t unit_wii_wbfs unit_zip unit_3ds unit_wiiu)
    target_compile_definitions(${t} PRIVATE SIGIL_TEST_FILENAME_FALLBACK=1)
endforeach()
