#include <lifter/Codegen.hpp>

#include "LlvmCompatibility.hpp"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Ps5Lift {

namespace {

void InitializeTargets() {
    static std::once_flag once;
    std::call_once(once, [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        // AArch64's complex deinterleaving pass (LLVM 18 and 19) can place the llvm.vector.deinterleave2 it forms
        // after a use of its result, which the instruction scheduler then crashes on. Lifted x86 code has little
        // complex arithmetic for it to form.
        auto& options = llvm::cl::getRegisteredOptions();
        const auto deinterleaving = options.find("enable-complex-deinterleaving");
        if (deinterleaving == options.end()) throw std::logic_error("LLVM has no enable-complex-deinterleaving option to turn the pass off");
        static_cast<llvm::cl::opt<bool>*>(deinterleaving->second)->setValue(false);
    });
}

// The Zen 2 instructions without a target-independent form need the target's own: AES-NI and PCLMULQDQ become the
// aarch64 crypto extension, which LLVM's aes feature enables with PMULL, and crc32 its CRC32C instructions; on x86-64
// they stay what they are. LLVM's x86 backend selects crc32 only with its own feature, which clang's -msse4.2 adds
// but LLVM's sse4.2 does not imply.
std::string Features(const CodegenOptions& options) {
    const auto required = options.Triple.isAArch64() ? "+aes,+crc" : "+aes,+pclmul,+sse4.2,+crc32";
    return options.Features.empty() ? required : options.Features + "," + required;
}

std::unique_ptr<llvm::TargetMachine> CreateTargetMachine(const llvm::Triple& triple, const std::string& cpu, const std::string& features, const bool optimize) {
    InitializeTargets();
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(TripleArgument(triple), error);
    if (target == nullptr) throw std::runtime_error("Unknown target " + triple.str() + ": " + error);
    llvm::TargetOptions targetOptions;
    auto* machine = target->createTargetMachine(TripleArgument(triple), cpu, features, targetOptions, llvm::Reloc::PIC_, std::nullopt, optimize ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None);
    if (machine == nullptr) throw std::runtime_error("Cannot create a target machine for " + triple.str());
    return std::unique_ptr<llvm::TargetMachine>(machine);
}

std::unique_ptr<llvm::TargetMachine> CreateTargetMachine(const CodegenOptions& options) {
    return CreateTargetMachine(options.Triple, options.Cpu, Features(options), options.Optimize);
}

void Configure(llvm::Module& module, llvm::TargetMachine& machine, const CodegenOptions& options) {
    module.setTargetTriple(TripleArgument(options.Triple));
    module.setDataLayout(machine.createDataLayout());
}

}

void ValidateTarget(const CodegenOptions& options) {
    if (!options.Triple.isAArch64() && options.Triple.getArch() != llvm::Triple::x86_64) throw std::runtime_error("ps5lift lifts for aarch64 and x86-64, not " + options.Triple.str());
    const auto generic = CreateTargetMachine(options.Triple, "", "", false);
    const auto& subtarget = SubtargetInfo(*generic);
    if (!subtarget.isCPUStringValid(options.Cpu)) throw std::runtime_error(options.Cpu + " is not a CPU of " + options.Triple.str());
    std::set<std::string_view> known;
    for (const auto& feature : subtarget.getAllProcessorFeatures()) known.insert(FeatureName(feature));
    if (options.Features.empty()) return;
    std::string_view rest = options.Features;
    while (true) {
        const auto comma = rest.find(',');
        const auto feature = rest.substr(0, comma);
        if (feature.size() < 2 || (feature[0] != '+' && feature[0] != '-')) throw std::runtime_error("features are +name or -name separated by commas, not '" + std::string(feature) + "'");
        if (!known.contains(feature.substr(1))) throw std::runtime_error(std::string(feature.substr(1)) + " is not a feature of " + options.Triple.str());
        if (comma == std::string_view::npos) return;
        rest.remove_prefix(comma + 1);
    }
}

void Optimize(llvm::Module& module, const CodegenOptions& options) {
    auto machine = CreateTargetMachine(options);
    Configure(module, *machine, options);
    if (!options.Optimize) return;

    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder builder(machine.get());
    builder.registerModuleAnalyses(modules);
    builder.registerCGSCCAnalyses(cgscc);
    builder.registerFunctionAnalyses(functions);
    builder.registerLoopAnalyses(loops);
    builder.crossRegisterProxies(loops, functions, cgscc, modules);
    builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(module, modules);
    std::string problems;
    llvm::raw_string_ostream stream(problems);
    if (llvm::verifyModule(module, &stream)) throw std::runtime_error("LLVM's optimizer left a module that does not verify: " + problems);
}

llvm::SmallVector<char, 0> CompileObject(llvm::Module& module, const CodegenOptions& options) {
    auto machine = CreateTargetMachine(options);
    Configure(module, *machine, options);
    llvm::SmallVector<char, 0> object;
    llvm::raw_svector_ostream stream(object);
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, stream, nullptr, llvm::CodeGenFileType::ObjectFile, false)) throw std::runtime_error("The target cannot emit object files");
    passes.run(module);
    return object;
}

void WriteIr(const llvm::Module& module, const std::string& path) {
    std::error_code error;
    llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_Text);
    if (error) throw std::runtime_error("Cannot write " + path + ": " + error.message());
    module.print(stream, nullptr);
}

}
