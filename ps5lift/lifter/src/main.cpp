#include <lifter/Codegen.hpp>
#include <lifter/Discovery.hpp>
#include <lifter/GuestImage.hpp>
#include <lifter/Lifter.hpp>

#include <llvm/ADT/StringMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view Usage = "usage: ps5lift <input.elf> -o <output.o> [--triple <triple>] [--cpu <cpu>|native] [--features <list>] [--memory tso|relaxed] [--float exact|fast] [--preserve-flags] [--import-nids] [--emit-ir <file.ll>] [-O0]\n";

}

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    std::string ir;
    Ps5Lift::LiftOptions lift;
    Ps5Lift::CodegenOptions codegen{"aarch64-unknown-linux-gnu", "", "", true};

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << Usage;
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "-o") output = value();
        else if (argument == "--triple") codegen.Triple = value();
        else if (argument == "--cpu") codegen.Cpu = value();
        else if (argument == "--features") codegen.Features = value();
        else if (argument == "--emit-ir") ir = value();
        else if (argument == "-O0") codegen.Optimize = false;
        else if (argument == "--preserve-flags") lift.PreserveFlags = true;
        else if (argument == "--import-nids") lift.ImportNids = true;
        else if (argument == "--memory") {
            const auto model = value();
            if (model == "tso") lift.Memory = Ps5Lift::MemoryModel::Tso;
            else if (model == "relaxed") lift.Memory = Ps5Lift::MemoryModel::Relaxed;
            else {
                std::cerr << "unknown memory model " << model << "\n";
                return 2;
            }
        } else if (argument == "--float") {
            const auto model = value();
            if (model == "exact") lift.Float = Ps5Lift::FloatModel::Exact;
            else if (model == "fast") lift.Float = Ps5Lift::FloatModel::Fast;
            else {
                std::cerr << "unknown float model " << model << "\n";
                return 2;
            }
        } else if (input.empty() && !argument.starts_with("-")) input = argument;
        else {
            std::cerr << Usage;
            return 2;
        }
    }
    if (input.empty() || (output.empty() && ir.empty())) {
        std::cerr << Usage;
        return 2;
    }

    if (codegen.Cpu == "native") {
        codegen.Cpu = llvm::sys::getHostCPUName().str();
#if LLVM_VERSION_MAJOR >= 19
        const auto features = llvm::sys::getHostCPUFeatures();
#else
        llvm::StringMap<bool> features;
        llvm::sys::getHostCPUFeatures(features);
#endif
        for (const auto& feature : features) {
            if (!codegen.Features.empty()) codegen.Features += ",";
            codegen.Features += (feature.getValue() ? "+" : "-") + feature.getKey().str();
        }
    } else if (codegen.Cpu.empty()) {
        codegen.Cpu = codegen.Triple.starts_with("aarch64") ? "neoverse-v2" : "generic";
    }

    try {
        const auto image = Ps5Lift::GuestImage::Load(input);
        const auto functions = Ps5Lift::DiscoverFunctions(image);
        llvm::LLVMContext context;
        Ps5Lift::LiftReport report;
        auto module = Ps5Lift::LiftImage(context, image, functions, lift, report);
        Ps5Lift::Optimize(*module, codegen);
        if (!ir.empty()) Ps5Lift::WriteIr(*module, ir);
        if (!output.empty()) Ps5Lift::WriteObject(*module, codegen, output);

        std::cerr << "ps5lift: " << report.Functions << " functions, " << report.Instructions << " instructions";
        std::size_t unsupported = 0;
        for (const auto& [mnemonic, count] : report.Unsupported) unsupported += count;
        std::size_t tables = 0;
        std::size_t unguarded = 0;
        for (const auto& [entry, function] : functions) {
            tables += function.JumpTables.size();
            unguarded += function.UnguardedJumpTables.size();
        }
        std::cerr << ", " << unsupported << " unsupported, " << tables << " jump tables (" << unguarded << " sized by layout)\n";
        for (const auto& [mnemonic, count] : report.Unsupported) std::cerr << "  unsupported " << mnemonic << ": " << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "ps5lift: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
