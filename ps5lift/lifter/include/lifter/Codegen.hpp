#ifndef PS5LIFT_CODEGEN_HPP
#define PS5LIFT_CODEGEN_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

#include <string>

namespace Ps5Lift {

struct CodegenOptions {
    llvm::Triple Triple;
    std::string Cpu;
    std::string Features;
    bool Optimize = true;
};

void ValidateTarget(const CodegenOptions& options);
void Optimize(llvm::Module& module, const CodegenOptions& options);
llvm::SmallVector<char, 0> CompileObject(llvm::Module& module, const CodegenOptions& options);
void WriteIr(const llvm::Module& module, const std::string& path);

}

#endif
