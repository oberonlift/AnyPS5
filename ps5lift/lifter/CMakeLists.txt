add_executable(ps5lift
        src/main.cpp
        src/GuestImage.cpp
        src/Discovery.cpp
        src/Lifter.cpp
        src/Codegen.cpp
        src/Compile.cpp
        src/Program.cpp)
target_include_directories(ps5lift PRIVATE include ${LLVM_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/runtime/include)
target_compile_definitions(ps5lift PRIVATE ${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(PS5LIFT_LLVM_LIBS core support passes target mc irreader ipo object
        aarch64codegen aarch64asmparser aarch64desc aarch64info
        x86codegen x86asmparser x86desc x86info)
target_link_libraries(ps5lift PRIVATE anyps5_elf Zydis ${PS5LIFT_LLVM_LIBS})
