#ifndef PS5LIFT_CODEGEN_HPP
#define PS5LIFT_CODEGEN_HPP

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
void WriteObject(llvm::Module& module, const CodegenOptions& options, const std::string& path);
void WriteIr(const llvm::Module& module, const std::string& path);

}

#endif
