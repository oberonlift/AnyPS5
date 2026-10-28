find_package(Python3 REQUIRED COMPONENTS Interpreter)

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
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture basic vector float threads callbacks x87 setjmp cpu mmx modules)
        set(memory_models)
        if(fixture STREQUAL "threads")
            set(memory_models --memory tso)
        endif()
        add_test(NAME fixture_${fixture}_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_fixture.py
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --clang ${CMAKE_C_COMPILER}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --triple ${triple}
                        --fixture ${fixture}
                        ${memory_models})
    endforeach()
endforeach()

include(ExternalProject)
set(PS5LIFT_ANYPS5_BUILD ${CMAKE_BINARY_DIR}/anyps5)
ExternalProject_Add(anyps5
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/anyps5
        BINARY_DIR ${PS5LIFT_ANYPS5_BUILD}
        CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_GUEST_LIFTED=ON -DBUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                   -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libs relinker
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS ${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker
        EXCLUDE_FROM_ALL OFF)

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture import tls)
        add_test(NAME ps5_${fixture}_${arch}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_ps5_fixture.py
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --relinker ${PS5LIFT_ANYPS5_BUILD}/core/relinker/relinker
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --triple ${triple}
                        --fixture ${fixture})
    endforeach()
endforeach()

execute_process(COMMAND ${Python3_EXECUTABLE} -c "import clang.cindex" RESULT_VARIABLE PS5LIFT_HAVE_LIBCLANG_PYTHON OUTPUT_QUIET ERROR_QUIET)
if(PS5LIFT_HAVE_LIBCLANG_PYTHON EQUAL 0)
    foreach(fixture libc exceptions kernel fiber)
        add_test(NAME ${fixture}_${PS5LIFT_HOST_ARCH}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_libc_fixture.py
                        --ps5lift $<TARGET_FILE:ps5lift>
                        --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                        --clangxx ${CMAKE_CXX_COMPILER}
                        --fixture ${fixture})
    endforeach()
else()
    message(STATUS "The libclang Python bindings were not found; the libc bridge tests are skipped")
endif()

add_test(NAME libc_upstream_${PS5LIFT_HOST_ARCH}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_libc_tests.py
                --anyps5-build ${PS5LIFT_ANYPS5_BUILD}
                --clangxx ${CMAKE_CXX_COMPILER}
                --build ${CMAKE_BINARY_DIR})
