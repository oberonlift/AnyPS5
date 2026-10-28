#ifndef PS5LIFT_LIFTER_HPP
#define PS5LIFT_LIFTER_HPP

#include <lifter/Discovery.hpp>
#include <lifter/GuestImage.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

namespace Ps5Lift {

enum class MemoryModel {
    Tso,
    Relaxed
};

enum class FloatModel {
    Exact,
    Fast
};

struct LiftOptions {
    MemoryModel Memory = MemoryModel::Tso;
    FloatModel Float = FloatModel::Exact;
    bool PreserveFlags = false;
    bool ImportNids = false;
};

struct LiftReport {
    std::size_t Functions = 0;
    std::size_t Instructions = 0;
    std::map<std::string, std::size_t> Unsupported;
};

std::unique_ptr<llvm::Module> LiftFunctions(llvm::LLVMContext& context, const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, std::span<const std::uint64_t> addresses,
                                            const LiftOptions& options, LiftReport& report);
std::unique_ptr<llvm::Module> LiftTables(llvm::LLVMContext& context, const GuestImage& image, const std::map<std::uint64_t, GuestFunction>& functions, const LiftOptions& options);

}

#endif
