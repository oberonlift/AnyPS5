#ifndef PS5LIFT_PROGRAM_HPP
#define PS5LIFT_PROGRAM_HPP

#include <lifter/Codegen.hpp>
#include <lifter/Discovery.hpp>
#include <lifter/GuestImage.hpp>
#include <lifter/Lifter.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace Ps5Lift {

struct ProgramOptions {
    unsigned Jobs = 1;
    std::string Archive;
    std::string IrPrefix;
};

LiftReport CompileProgram(const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, const LiftOptions& lift, const CodegenOptions& codegen, const ProgramOptions& program);

}

#endif
