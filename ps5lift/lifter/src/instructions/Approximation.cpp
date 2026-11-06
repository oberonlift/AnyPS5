#include "../FunctionLifter.hpp"

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace Ps5Lift::Lifting {

namespace {

constexpr unsigned TableSize = 2048;
constexpr double MantissaScale = 8388608.0;
constexpr std::uint32_t SingleSign = 0x80000000u;
constexpr std::uint32_t SingleQuiet = 0x400000u;
constexpr std::uint32_t SingleInfinity = 0x7f800000u;
constexpr std::uint32_t SingleIndefinite = 0xffc00000u;

std::vector<std::uint32_t> ReciprocalTable() {
    std::vector<std::uint32_t> table(TableSize);
    for (unsigned i = 0; i < TableSize; ++i) table[i] = static_cast<std::uint32_t>(std::lround((2.0 / (1.0 + (i + 0.5) / TableSize) - 1.0) * MantissaScale));
    return table;
}

std::vector<std::uint32_t> ReciprocalSqrtTable() {
    constexpr unsigned Half = TableSize / 2;
    std::vector<std::uint32_t> table(TableSize);
    for (unsigned i = 0; i < TableSize; ++i) {
        const double significand = (i < Half ? 2.0 : 1.0) * (1.0 + (i % Half + 0.5) / Half);
        table[i] = static_cast<std::uint32_t>(std::lround((2.0 / std::sqrt(significand) - 1.0) * MantissaScale));
    }
    return table;
}

}

llvm::GlobalVariable* FunctionLifter::Table(const char* name, std::vector<std::uint32_t> (*build)()) {
    auto& module = *_host->getParent();
    if (auto* table = module.getGlobalVariable(name, true)) return table;
    const auto values = build();
    auto* contents = llvm::ConstantDataArray::get(Context(), llvm::ArrayRef<std::uint32_t>(values));
    return new llvm::GlobalVariable(module, contents->getType(), true, llvm::GlobalValue::PrivateLinkage, contents, name);
}

llvm::Value* FunctionLifter::Gather(llvm::GlobalVariable* table, llvm::Value* index) {
    auto* element = _builder.getInt32Ty();
    auto* type = index->getType();
    if (!type->isVectorTy()) return _builder.CreateLoad(element, _builder.CreateGEP(element, table, _builder.CreateZExt(index, I64())));
    const auto count = llvm::cast<llvm::FixedVectorType>(type)->getNumElements();
    auto* pointers = _builder.CreateGEP(element, table, _builder.CreateZExt(index, llvm::FixedVectorType::get(I64(), count)));
    return _builder.CreateMaskedGather(llvm::FixedVectorType::get(element, count), pointers, llvm::Align(4));
}

llvm::Value* FunctionLifter::Reciprocal(llvm::Value* value) {
    auto* type = IntegerLike(value->getType());
    const auto constant = [&](const std::uint32_t number) { return llvm::ConstantInt::get(type, number); };
    auto* bits = _builder.CreateBitCast(value, type);
    auto* sign = _builder.CreateAnd(bits, constant(SingleSign));
    auto* exponent = _builder.CreateAnd(_builder.CreateLShr(bits, constant(23)), constant(0xff));
    auto* fraction = _builder.CreateAnd(bits, constant(0x7fffff));
    auto* power = _builder.CreateICmpEQ(fraction, constant(0));
    auto* mantissa = Gather(Table("ps5lift_reciprocal", ReciprocalTable), _builder.CreateLShr(fraction, constant(12)));
    auto* exact = _builder.CreateShl(_builder.CreateSub(constant(254), exponent), constant(23));
    auto* estimate = _builder.CreateOr(_builder.CreateShl(_builder.CreateSub(constant(253), exponent), constant(23)), mantissa);
    llvm::Value* magnitude = _builder.CreateSelect(power, exact, estimate);
    auto* tiny = _builder.CreateOr(_builder.CreateICmpUGE(exponent, constant(254)), _builder.CreateAnd(_builder.CreateICmpEQ(exponent, constant(253)), _builder.CreateNot(power)));
    magnitude = _builder.CreateSelect(tiny, constant(0), magnitude);
    magnitude = _builder.CreateSelect(_builder.CreateICmpEQ(exponent, constant(0)), constant(SingleInfinity), magnitude);
    auto* special = _builder.CreateSelect(power, sign, _builder.CreateOr(bits, constant(SingleQuiet)));
    auto* result = _builder.CreateSelect(_builder.CreateICmpEQ(exponent, constant(0xff)), special, _builder.CreateOr(magnitude, sign));
    return _builder.CreateBitCast(result, value->getType());
}

llvm::Value* FunctionLifter::ReciprocalSqrt(llvm::Value* value) {
    auto* type = IntegerLike(value->getType());
    const auto constant = [&](const std::uint32_t number) { return llvm::ConstantInt::get(type, number); };
    auto* bits = _builder.CreateBitCast(value, type);
    auto* sign = _builder.CreateAnd(bits, constant(SingleSign));
    auto* exponent = _builder.CreateAnd(_builder.CreateLShr(bits, constant(23)), constant(0xff));
    auto* fraction = _builder.CreateAnd(bits, constant(0x7fffff));
    auto* index = _builder.CreateOr(_builder.CreateShl(_builder.CreateAnd(exponent, constant(1)), constant(10)), _builder.CreateLShr(fraction, constant(13)));
    auto* mantissa = Gather(Table("ps5lift_reciprocal_sqrt", ReciprocalSqrtTable), index);
    auto* halved = _builder.CreateAShr(_builder.CreateSub(exponent, constant(127)), constant(1));
    llvm::Value* result = _builder.CreateOr(_builder.CreateShl(_builder.CreateSub(constant(126), halved), constant(23)), mantissa);
    auto* negative = _builder.CreateICmpNE(sign, constant(0));
    result = _builder.CreateSelect(negative, constant(SingleIndefinite), result);
    result = _builder.CreateSelect(_builder.CreateAnd(_builder.CreateICmpEQ(exponent, constant(0xff)), _builder.CreateNot(negative)), constant(0), result);
    result = _builder.CreateSelect(_builder.CreateICmpEQ(exponent, constant(0)), _builder.CreateOr(sign, constant(SingleInfinity)), result);
    result = _builder.CreateSelect(IsNan(value), _builder.CreateOr(bits, constant(SingleQuiet)), result);
    return _builder.CreateBitCast(result, value->getType());
}

}
