#include <lifter/Codegen.hpp>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

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
    });
}

// The Zen 2 instructions without a target-independent form need the target's own: AES-NI and PCLMULQDQ become the
// aarch64 crypto extension, which LLVM's aes feature enables with PMULL, and stay AES-NI and PCLMULQDQ on x86-64.
std::string Features(const CodegenOptions& options) {
    const auto required = llvm::Triple(options.Triple).isAArch64() ? "+aes" : "+aes,+pclmul";
    return options.Features.empty() ? required : options.Features + "," + required;
}

std::unique_ptr<llvm::TargetMachine> CreateTargetMachine(const CodegenOptions& options) {
    InitializeTargets();
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(options.Triple, error);
    if (target == nullptr) throw std::runtime_error("Unknown target " + options.Triple + ": " + error);
    llvm::TargetOptions targetOptions;
    auto* machine = target->createTargetMachine(options.Triple, options.Cpu, Features(options), targetOptions, llvm::Reloc::PIC_, std::nullopt, options.Optimize ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::None);
    if (machine == nullptr) throw std::runtime_error("Cannot create a target machine for " + options.Triple);
    return std::unique_ptr<llvm::TargetMachine>(machine);
}

void Configure(llvm::Module& module, llvm::TargetMachine& machine, const CodegenOptions& options) {
    module.setTargetTriple(options.Triple);
    module.setDataLayout(machine.createDataLayout());
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
}

llvm::SmallVector<char, 0> CompileObject(llvm::Module& module, const CodegenOptions& options) {
    auto machine = CreateTargetMachine(options);
    Configure(module, *machine, options);
    llvm::SmallVector<char, 0> object;
    llvm::raw_svector_ostream stream(object);
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, stream, nullptr, llvm::CodeGenFileType::ObjectFile)) throw std::runtime_error("The target cannot emit object files");
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
