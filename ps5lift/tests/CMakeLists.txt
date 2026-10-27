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
    foreach(fixture basic vector float)
    string(REGEX REPLACE "-.*" "" arch ${triple})
    add_test(NAME fixture_${fixture}_${arch}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/run_fixture.py
                    --ps5lift $<TARGET_FILE:ps5lift>
                    --clang ${CMAKE_C_COMPILER}
                    --clangxx ${CMAKE_CXX_COMPILER}
                    --triple ${triple}
                    --fixture ${fixture})
    endforeach()
endforeach()
