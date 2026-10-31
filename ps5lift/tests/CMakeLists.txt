find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PS5LIFT_FIXTURES basic vector float threads callbacks x87 setjmp cpu mmx modules strings sse41 avx)

# Adds a test per fixture, and one of the basic fixture through an unoptimized lift, that run tools/run_fixture.py for
# a triple with the arguments after it. A run of the threads fixture uses TSO only.
function(ps5lift_fixture_tests prefix triple)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture IN LISTS PS5LIFT_FIXTURES)
        set(memory_models)
        if(fixture STREQUAL "threads" AND NOT "--lift-only" IN_LIST ARGN)
            set(memory_models --memory tso)
        endif()
        add_test(NAME ${prefix}_${fixture}_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_fixture.py
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --triple ${triple}
                        --fixture ${fixture}
                        ${memory_models}
                        ${ARGN})
    endforeach()
    add_test(NAME ${prefix}_basic_unoptimized_${arch}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_fixture.py
                    --ps5lift $<TARGET_FILE:ps5lift>
                    --clang ${CMAKE_C_COMPILER}
                    --clangxx ${CMAKE_CXX_COMPILER}
                    --triple ${triple}
                    --fixture basic
                    --optimization=-O2
                    --unoptimized-lift
                    ${ARGN})
endfunction()

# Adds a test of each of AnyPS5's PS5-style fixtures, which the relinker relinks before ps5lift lifts them for a triple.
function(ps5lift_ps5_fixture_tests relinker triple)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture import tls)
        add_test(NAME ps5_${fixture}_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_ps5_fixture.py
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --relinker ${relinker}
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --triple ${triple}
                        --fixture ${fixture})
    endforeach()
endfunction()

include(ExternalProject)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # On another host the tests lift every fixture for both Linux targets without running the result, run the fixtures
    # and AnyPS5's PS5-style ones natively on macOS for arm64, with a relinker-only build of AnyPS5, and leave the
    # tests that run AnyPS5's libraries to a Linux host. The guests build
    # against an x86-64 Linux sysroot, since threads, callbacks and modules link its libraries: usr/x86_64-linux-gnu
    # and usr/lib/gcc-cross/x86_64-linux-gnu as Ubuntu's g++-x86-64-linux-gnu installs them, copied below one directory.
    set(PS5LIFT_X86_64_SYSROOT "" CACHE PATH "x86-64 Linux sysroot that the fixture guests build against")
    if(NOT IS_DIRECTORY "${PS5LIFT_X86_64_SYSROOT}/usr/x86_64-linux-gnu" OR NOT IS_DIRECTORY "${PS5LIFT_X86_64_SYSROOT}/usr/lib/gcc-cross/x86_64-linux-gnu")
        message(FATAL_ERROR "The fixture guests need an x86-64 Linux sysroot on ${CMAKE_SYSTEM_NAME}: set PS5LIFT_X86_64_SYSROOT to a directory "
                "with usr/x86_64-linux-gnu and usr/lib/gcc-cross/x86_64-linux-gnu, as Ubuntu's g++-x86-64-linux-gnu installs them, "
                "or configure with -DBUILD_TESTING=OFF.")
    endif()
    foreach(triple aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu)
        ps5lift_fixture_tests(lift ${triple} --guest-sysroot ${PS5LIFT_X86_64_SYSROOT} --lift-only)
    endforeach()
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        ps5lift_fixture_tests(fixture arm64-apple-macosx --guest-sysroot ${PS5LIFT_X86_64_SYSROOT})
        set(PS5LIFT_RELINKER_BUILD ${CMAKE_BINARY_DIR}/anyps5-relinker)
        ExternalProject_Add(anyps5_relinker
                SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/anyps5
                BINARY_DIR ${PS5LIFT_RELINKER_BUILD}
                CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON
                           -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target relinker
                INSTALL_COMMAND ""
                BUILD_ALWAYS ON
                BUILD_BYPRODUCTS ${PS5LIFT_RELINKER_BUILD}/core/relinker/relinker
                EXCLUDE_FROM_ALL OFF)
        ps5lift_ps5_fixture_tests(${PS5LIFT_RELINKER_BUILD}/core/relinker/relinker arm64-apple-macosx)
    endif()
    return()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(PS5LIFT_HOST_ARCH aarch64)
    set(PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
else()
    set(PS5LIFT_HOST_ARCH x86_64)
    set(PS5LIFT_TEST_TRIPLES x86_64-unknown-linux-gnu)
    find_program(PS5LIFT_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
    if(PS5LIFT_QEMU_AARCH64)
        list(APPEND PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
    endif()
endif()

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    ps5lift_fixture_tests(fixture ${triple})
endforeach()

# AnyPS5's lifted libraries link against the runtime built for this host, as the tests' programs do.
set(PS5LIFT_HOST_TRIPLE ${PS5LIFT_HOST_ARCH}-unknown-linux-gnu)
set(PS5LIFT_HOST_RUNTIME ${CMAKE_BINARY_DIR}/runtime-${PS5LIFT_HOST_TRIPLE}/libps5lift_runtime.so)
add_custom_target(ps5lift_host_runtime
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_runtime.py --triple ${PS5LIFT_HOST_TRIPLE}
                --output ${CMAKE_BINARY_DIR}/runtime-${PS5LIFT_HOST_TRIPLE} --clang ${CMAKE_C_COMPILER} --clangxx ${CMAKE_CXX_COMPILER}
        BYPRODUCTS ${PS5LIFT_HOST_RUNTIME}
        VERBATIM)

set(PS5LIFT_ANYPS5_BUILD ${CMAKE_BINARY_DIR}/anyps5)
ExternalProject_Add(anyps5
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/..
        BINARY_DIR ${PS5LIFT_ANYPS5_BUILD}
        DEPENDS ps5lift_host_runtime
        CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_GUEST_LIFTED=ON -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                   -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                   -DANYPS5_LIFTED_RUNTIME=${PS5LIFT_HOST_RUNTIME}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libs relinker
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS ${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker
        EXCLUDE_FROM_ALL OFF)

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    ps5lift_ps5_fixture_tests(${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker ${triple})
endforeach()

execute_process(COMMAND ${Python3_EXECUTABLE} -c "import clang.cindex; clang.cindex.Index.create()"
        RESULT_VARIABLE PS5LIFT_LIBCLANG_PYTHON ERROR_VARIABLE PS5LIFT_LIBCLANG_PYTHON_ERROR OUTPUT_QUIET)
if(NOT PS5LIFT_LIBCLANG_PYTHON EQUAL 0)
    message(FATAL_ERROR "The libc, exceptions, kernel and fiber tests generate bridges with the libclang Python bindings, "
            "which ${Python3_EXECUTABLE} cannot load:\n${PS5LIFT_LIBCLANG_PYTHON_ERROR}"
            "Install them (python3-clang-18 on Ubuntu) or configure with -DBUILD_TESTING=OFF.")
endif()
foreach(fixture libc exceptions kernel fiber)
    add_test(NAME ${fixture}_${PS5LIFT_HOST_ARCH}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_libc_fixture.py
                    --ps5lift $<TARGET_FILE:ps5lift>
                    --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                    --clang ${CMAKE_C_COMPILER}
                    --clangxx ${CMAKE_CXX_COMPILER}
                    --fixture ${fixture})
endforeach()

add_test(NAME record_bridges
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_record_bridges.py --clang ${CMAKE_C_COMPILER})

add_test(NAME libc_upstream_${PS5LIFT_HOST_ARCH}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_libc_tests.py
                --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                --clang ${CMAKE_C_COMPILER}
                --clangxx ${CMAKE_CXX_COMPILER}
                --build ${CMAKE_BINARY_DIR})
