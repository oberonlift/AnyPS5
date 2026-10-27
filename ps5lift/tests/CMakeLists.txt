find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
else()
    set(PS5LIFT_TEST_TRIPLES x86_64-unknown-linux-gnu)
    find_program(PS5LIFT_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
    if(PS5LIFT_QEMU_AARCH64)
        list(APPEND PS5LIFT_TEST_TRIPLES aarch64-unknown-linux-gnu)
    endif()
endif()

foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    foreach(fixture basic vector float threads)
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
find_program(PS5LIFT_RELINKER_C_COMPILER NAMES gcc)
find_program(PS5LIFT_RELINKER_CXX_COMPILER NAMES g++)
if(PS5LIFT_RELINKER_C_COMPILER AND PS5LIFT_RELINKER_CXX_COMPILER)
    ExternalProject_Add(anyps5_relinker
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/anyps5
            BINARY_DIR ${CMAKE_BINARY_DIR}/anyps5-relinker
            CMAKE_ARGS -G Ninja -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=OFF
                       -DCMAKE_C_COMPILER=${PS5LIFT_RELINKER_C_COMPILER} -DCMAKE_CXX_COMPILER=${PS5LIFT_RELINKER_CXX_COMPILER}
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target relinker
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/anyps5-relinker/core/relinker/relinker
            EXCLUDE_FROM_ALL OFF)
    foreach(triple IN LISTS PS5LIFT_TEST_TRIPLES)
        string(REGEX REPLACE "-.*" "" arch ${triple})
        foreach(fixture import tls)
            add_test(NAME ps5_${fixture}_${arch}
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_ps5_fixture.py
                            --ps5lift $<TARGET_FILE:ps5lift>
                            --relinker ${CMAKE_BINARY_DIR}/anyps5-relinker/core/relinker/relinker
                            --clangxx ${CMAKE_CXX_COMPILER}
                            --triple ${triple}
                            --fixture ${fixture})
        endforeach()
    endforeach()
else()
    message(STATUS "gcc and g++ were not found; the AnyPS5 relinker and the PS5 fixture tests are skipped")
endif()
