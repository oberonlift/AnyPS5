find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(PS5LIFT_QEMU_TESTS "On an x86-64 host, also lift the fixtures for aarch64 and run them under qemu-aarch64" OFF)

set(PS5LIFT_FIXTURES basic vector float threads callbacks x87 setjmp cpu mmx modules strings sse41 avx)

function(ps5lift_fixture_tests triple)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture IN LISTS PS5LIFT_FIXTURES)
        set(memory_models)
        if(fixture STREQUAL "threads")
            set(memory_models --memory tso)
        endif()
        foreach(level O0 O1 O2 O3)
            add_test(NAME fixture_${fixture}_${level}_${arch}
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py fixture
                            --ps5lift $<TARGET_FILE:ps5lift>
                            --clang ${CMAKE_C_COMPILER}
                            --clangxx ${CMAKE_CXX_COMPILER}
                            --triple ${triple}
                            --fixture ${fixture}
                            --optimization=-${level}
                            ${memory_models}
                            ${ARGN})
        endforeach()
    endforeach()
    add_test(NAME fixture_basic_unoptimized_${arch}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py fixture
                    --ps5lift $<TARGET_FILE:ps5lift>
                    --clang ${CMAKE_C_COMPILER}
                    --clangxx ${CMAKE_CXX_COMPILER}
                    --triple ${triple}
                    --fixture basic
                    --optimization=-O2
                    --unoptimized-lift
                    ${ARGN})
    add_test(NAME runtime_relink_${arch}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py runtime-relink
                    --triple ${triple}
                    --output ${CMAKE_BINARY_DIR}/runtime-relink-${triple}
                    --clang ${CMAKE_C_COMPILER}
                    --clangxx ${CMAKE_CXX_COMPILER})
    if(arch STREQUAL "aarch64")
        add_test(NAME runtime_without_sve_aarch64
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py runtime-sve
                        --triple ${triple}
                        --cpu generic
                        --output ${CMAKE_BINARY_DIR}/runtime-${triple}-generic
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER})
    endif()
endfunction()

function(ps5lift_ps5_fixture_tests relinker triple)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture import tls)
        add_test(NAME ps5_${fixture}_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py ps5-fixture
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
    set(PS5LIFT_X86_64_SYSROOT "" CACHE PATH "x86-64 Linux sysroot that the fixture guests build against")
    if(NOT IS_DIRECTORY "${PS5LIFT_X86_64_SYSROOT}/usr/x86_64-linux-gnu" OR NOT IS_DIRECTORY "${PS5LIFT_X86_64_SYSROOT}/usr/lib/gcc-cross/x86_64-linux-gnu")
        message(FATAL_ERROR "The fixture guests need an x86-64 Linux sysroot on ${CMAKE_SYSTEM_NAME}: set PS5LIFT_X86_64_SYSROOT to a directory "
                "with usr/x86_64-linux-gnu and usr/lib/gcc-cross/x86_64-linux-gnu, as Ubuntu's g++-x86-64-linux-gnu installs them, "
                "or configure with -DBUILD_TESTING=OFF.")
    endif()
    foreach(triple aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu)
        string(REGEX REPLACE "-.*" "" arch ${triple})
        foreach(fixture IN LISTS PS5LIFT_FIXTURES)
            add_test(NAME lift_${fixture}_${arch}
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py fixture
                            --ps5lift $<TARGET_FILE:ps5lift>
                            --clang ${CMAKE_C_COMPILER}
                            --clangxx ${CMAKE_CXX_COMPILER}
                            --triple ${triple}
                            --fixture ${fixture}
                            --guest-sysroot ${PS5LIFT_X86_64_SYSROOT}
                            --lift-only)
        endforeach()
        add_test(NAME lift_basic_unoptimized_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py fixture
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --triple ${triple}
                        --fixture basic
                        --optimization=-O2
                        --unoptimized-lift
                        --guest-sysroot ${PS5LIFT_X86_64_SYSROOT}
                        --lift-only)
    endforeach()
    if(NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"))
        return()
    endif()
    set(PS5LIFT_HOST_ARCH arm64)
    set(PS5LIFT_HOST_TRIPLE arm64-apple-macosx)
    set(PS5LIFT_TEST_TRIPLES ${PS5LIFT_HOST_TRIPLE})
    set(PS5LIFT_GUEST_ARGUMENTS --guest-sysroot ${PS5LIFT_X86_64_SYSROOT})
    set(PS5LIFT_ANYPS5_CPU_FLAGS)
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(PS5LIFT_HOST_ARCH aarch64)
        set(PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
        set(PS5LIFT_ANYPS5_CPU_FLAGS -DCMAKE_C_FLAGS=-mcpu=neoverse-v2 -DCMAKE_CXX_FLAGS=-mcpu=neoverse-v2)
    else()
        set(PS5LIFT_HOST_ARCH x86_64)
        set(PS5LIFT_TEST_TRIPLES x86_64-unknown-linux-gnu)
        if(PS5LIFT_QEMU_TESTS)
            list(APPEND PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
        endif()
        set(PS5LIFT_ANYPS5_CPU_FLAGS)
    endif()
    set(PS5LIFT_HOST_TRIPLE ${PS5LIFT_HOST_ARCH}-unknown-linux-gnu)
    set(PS5LIFT_GUEST_ARGUMENTS)
endif()

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    ps5lift_fixture_tests(${triple} ${PS5LIFT_GUEST_ARGUMENTS})
endforeach()

set(PS5LIFT_HOST_RUNTIME ${CMAKE_BINARY_DIR}/runtime-${PS5LIFT_HOST_TRIPLE}/libps5lift_runtime${CMAKE_SHARED_LIBRARY_SUFFIX})
add_custom_target(ps5lift_host_runtime
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py runtime --triple ${PS5LIFT_HOST_TRIPLE}
                --output ${CMAKE_BINARY_DIR}/runtime-${PS5LIFT_HOST_TRIPLE} --clang ${CMAKE_C_COMPILER} --clangxx ${CMAKE_CXX_COMPILER}
        BYPRODUCTS ${PS5LIFT_HOST_RUNTIME}
        VERBATIM)

set(PS5LIFT_ANYPS5_BUILD ${CMAKE_BINARY_DIR}/anyps5)
set(PS5LIFT_ANYPS5_LIBRARIES ${PS5LIFT_ANYPS5_BUILD}/core/libs/libs/unpatched)
ExternalProject_Add(anyps5
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/..
        BINARY_DIR ${PS5LIFT_ANYPS5_BUILD}
        DEPENDS ps5lift_host_runtime
        CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_GUEST_LIFTED=ON -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                   -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                   -DANYPS5_LIFTED_RUNTIME=${PS5LIFT_HOST_RUNTIME} ${PS5LIFT_ANYPS5_CPU_FLAGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libs relinker
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS ${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker ${PS5LIFT_ANYPS5_LIBRARIES}/libc.prx ${PS5LIFT_ANYPS5_LIBRARIES}/libkernel.prx
                         ${PS5LIFT_ANYPS5_LIBRARIES}/libSceFiber.prx ${PS5LIFT_ANYPS5_LIBRARIES}/libkernel_sys.prx
        EXCLUDE_FROM_ALL OFF)

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    ps5lift_ps5_fixture_tests(${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker ${triple})
endforeach()

set(PS5LIFT_LIBCLANG_ARGUMENTS)
if(PS5LIFT_LIBCLANG)
    set(PS5LIFT_LIBCLANG_ARGUMENTS --libclang ${PS5LIFT_LIBCLANG})
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} -B -c "import sys; sys.path.insert(0, sys.argv.pop(1)); import ps5lift; ps5lift.bindings(*sys.argv[1:]).Index.create()"
                ${CMAKE_SOURCE_DIR}/tools ${PS5LIFT_LIBCLANG}
        RESULT_VARIABLE PS5LIFT_LIBCLANG_PYTHON ERROR_VARIABLE PS5LIFT_LIBCLANG_PYTHON_ERROR OUTPUT_QUIET)
if(NOT PS5LIFT_LIBCLANG_PYTHON EQUAL 0)
    message(FATAL_ERROR "The libc, exceptions, kernel, fiber and loader tests generate bridges with the libclang Python bindings, "
            "which ${Python3_EXECUTABLE} cannot load:\n${PS5LIFT_LIBCLANG_PYTHON_ERROR}"
            "Install them (python3-clang-18 on Ubuntu, Homebrew's llvm on macOS) or configure with -DBUILD_TESTING=OFF.")
endif()
set(PS5LIFT_BRIDGES_libc libc)
set(PS5LIFT_BRIDGES_exceptions libc)
set(PS5LIFT_BRIDGES_kernel libc libkernel libkernel_sys)
set(PS5LIFT_BRIDGES_fiber libc libkernel libSceFiber)
set(PS5LIFT_BRIDGES_loader libc libkernel)
set(PS5LIFT_TEST_BRIDGES)
foreach(fixture libc exceptions kernel fiber loader)
    set(bridges ${CMAKE_BINARY_DIR}/test-bridges/${fixture}.cpp)
    set(libraries)
    foreach(library IN LISTS PS5LIFT_BRIDGES_${fixture})
        list(APPEND libraries ${PS5LIFT_ANYPS5_LIBRARIES}/${library}.prx)
    endforeach()
    add_custom_command(OUTPUT ${bridges}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/test-bridges
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py bridges ${PS5LIFT_BRIDGES_${fixture}}
                    --anyps5-build ${PS5LIFT_ANYPS5_BUILD} -o ${bridges} ${PS5LIFT_LIBCLANG_ARGUMENTS}
            DEPENDS ${libraries} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py
            VERBATIM)
    list(APPEND PS5LIFT_TEST_BRIDGES ${bridges})
    foreach(level O0 O2)
        add_test(NAME ${fixture}_${level}_${PS5LIFT_HOST_ARCH}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py libc-fixture
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --fixture ${fixture}
                        --bridges ${bridges}
                        --optimization=-${level}
                        ${PS5LIFT_GUEST_ARGUMENTS})
    endforeach()
endforeach()
add_custom_target(ps5lift_test_bridges ALL DEPENDS ${PS5LIFT_TEST_BRIDGES})

add_test(NAME record_bridges
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py record-bridges --clang ${CMAKE_C_COMPILER} ${PS5LIFT_LIBCLANG_ARGUMENTS})

add_test(NAME libc_upstream_${PS5LIFT_HOST_ARCH}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ps5lift.py libc-tests
                --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                --clang ${CMAKE_C_COMPILER}
                --clangxx ${CMAKE_CXX_COMPILER}
                --build ${CMAKE_BINARY_DIR})
