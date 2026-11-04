#include "../FunctionLifter.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace Ps5Lift::Lifting {

namespace {

constexpr std::uint64_t UserModeFlagImage = 0x202;
constexpr std::pair<Ps5LiftFlag, unsigned> FlagImageBits[] = {
    {PS5LIFT_CF, 0}, {PS5LIFT_PF, 2}, {PS5LIFT_AF, 4}, {PS5LIFT_ZF, 6}, {PS5LIFT_SF, 7}, {PS5LIFT_DF, 10}, {PS5LIFT_OF, 11}};

}

void FunctionLifter::RandomNumber() {
    auto* value = _builder.CreateCall(_module.Runtime("ps5lift_random", llvm::FunctionType::get(I64(), false)));
    Write(0, _builder.CreateTrunc(value, Int(Bits(0))));
    SetFlag(PS5LIFT_CF, _builder.getTrue());
    for (const auto flag : {PS5LIFT_OF, PS5LIFT_SF, PS5LIFT_ZF, PS5LIFT_AF, PS5LIFT_PF}) SetFlag(flag, _builder.getFalse());
}

void FunctionLifter::ProcessorId() {
    Write(0, _builder.CreateZExt(_builder.CreateCall(_module.Runtime("ps5lift_processor_id", llvm::FunctionType::get(_builder.getInt32Ty(), false))), Int(Bits(0))));
}

void FunctionLifter::TimeStamp() {
    auto* ticks = _builder.CreateCall(_module.Runtime("ps5lift_read_tsc", llvm::FunctionType::get(I64(), false)));
    SetGpr(PS5LIFT_RAX, _builder.CreateAnd(ticks, _builder.getInt64(0xffffffff)));
    SetGpr(PS5LIFT_RDX, _builder.CreateLShr(ticks, _builder.getInt64(32)));
}

void FunctionLifter::TimeStampAndProcessor() {
    TimeStamp();
    SetGpr(PS5LIFT_RCX, _builder.CreateZExt(_builder.CreateCall(_module.Runtime("ps5lift_processor_id", llvm::FunctionType::get(_builder.getInt32Ty(), false))), I64()));
}

llvm::Value* FunctionLifter::FlagImage(const unsigned bits) {
    llvm::Value* image = Const(bits, UserModeFlagImage);
    for (const auto& [flag, bit] : FlagImageBits) {
        if (bit < bits) image = _builder.CreateOr(image, _builder.CreateShl(_builder.CreateZExt(Flag(flag), Int(bits)), bit));
    }
    return image;
}

void FunctionLifter::SetFlagsFromImage(llvm::Value* image) {
    const auto bits = image->getType()->getIntegerBitWidth();
    for (const auto& [flag, bit] : FlagImageBits) {
        if (bit < bits) SetFlag(flag, _builder.CreateTrunc(_builder.CreateLShr(image, bit), _builder.getInt1Ty()));
    }
}

void FunctionLifter::PopFlags() {
    constexpr std::uint64_t trap = 1 << 8;
    constexpr std::uint64_t nestedTask = 1 << 14;
    constexpr std::uint64_t alignmentCheck = 1 << 18;
    constexpr std::uint64_t identification = 1 << 21;
    const auto bits = _instruction->Info.operand_width;
    auto* image = Pop(bits);
    SetFlagsFromImage(image);
    auto* unmodelled = _builder.CreateAnd(image, Const(bits, trap | nestedTask | alignmentCheck | identification));
    StopIf(_builder.CreateICmpNE(unmodelled, Const(bits, 0)), PS5LIFT_STOP_MISSING_TRANSLATION, 0, "popf of TF, NT, AC or ID");
}

void FunctionLifter::CacheLine(const char* function) {
    _builder.CreateCall(_module.Runtime(function, llvm::FunctionType::get(_builder.getVoidTy(), {I64()}, false)), {Address(Operand(0))});
}

void FunctionLifter::ClearCacheLine() {
    constexpr unsigned lineBits = 512;
    auto* line = _builder.CreateAnd(_builder.CreateZExtOrTrunc(Read(0), I64()), _builder.getInt64(~std::uint64_t{lineBits / 8 - 1}));
    GuestStore(line, llvm::ConstantInt::get(Int(lineBits), 0));
}

void FunctionLifter::Cpuid() {
    for (const int index : {PS5LIFT_RAX, PS5LIFT_RCX}) StateStore(Gpr(index), offsetof(Ps5LiftState, Gpr) + 8 * static_cast<std::size_t>(index));
    _builder.CreateCall(_module.Runtime("ps5lift_cpuid", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), I64()}, false)), {_state, _builder.getInt64(_instruction->Address)});
    for (const int index : {PS5LIFT_RAX, PS5LIFT_RBX, PS5LIFT_RCX, PS5LIFT_RDX}) SetGpr(index, StateLoad(I64(), offsetof(Ps5LiftState, Gpr) + 8 * static_cast<std::size_t>(index)));
}

void FunctionLifter::ExtendedControlRegister() {
    auto* value = _builder.CreateCall(_module.Runtime("ps5lift_xgetbv", llvm::FunctionType::get(I64(), {I64(), _builder.getInt32Ty()}, false)),
                                      {_builder.getInt64(_instruction->Address), _builder.CreateTrunc(Gpr(PS5LIFT_RCX), _builder.getInt32Ty())});
    SetGpr(PS5LIFT_RAX, _builder.CreateAnd(value, _builder.getInt64(0xffffffff)));
    SetGpr(PS5LIFT_RDX, _builder.CreateLShr(value, _builder.getInt64(32)));
}

}
