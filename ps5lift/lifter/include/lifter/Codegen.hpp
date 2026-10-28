#ifndef PS5LIFT_CODEGEN_HPP
#define PS5LIFT_CODEGEN_HPP

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Module.h>

#include <string>

namespace Ps5Lift {

struct CodegenOptions {
    std::string Triple;
    std::string Cpu;
    std::string Features;
    bool Optimize = true;
};

void Optimize(llvm::Module& module, const CodegenOptions& options);
llvm::SmallVector<char, 0> CompileObject(llvm::Module& module, const CodegenOptions& options);
void WriteIr(const llvm::Module& module, const std::string& path);

}

#endif
