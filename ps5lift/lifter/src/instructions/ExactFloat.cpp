#include "../FunctionLifter.hpp"

#include <cstddef>

namespace Ps5Lift::Lifting {

namespace {

constexpr std::uint32_t ExceptionMasks = PS5LIFT_MXCSR_EXCEPTIONS << PS5LIFT_MXCSR_MASK_SHIFT;
constexpr std::size_t SlotBytes = 32;
constexpr std::size_t SlotCount = 4;

}

llvm::Value* FunctionLifter::Mxcsr() {
    return StateLoad(_builder.getInt32Ty(), offsetof(Ps5LiftState, Mxcsr));
}

llvm::Value* FunctionLifter::MxcsrSet(const std::uint32_t bits) {
    return _builder.CreateICmpNE(_builder.CreateAnd(Mxcsr(), bits), _builder.getInt32(0));
}

llvm::Value* FunctionLifter::AnyLane(llvm::Value* condition) {
    return condition->getType()->isVectorTy() ? _builder.CreateOrReduce(condition) : condition;
}

llvm::Value* FunctionLifter::MxcsrFlag(llvm::Value* condition, const std::uint32_t flag) {
    return _builder.CreateSelect(AnyLane(condition), _builder.getInt32(flag), _builder.getInt32(0));
}

llvm::Value* FunctionLifter::DenormalFlag(std::initializer_list<llvm::Value*> sources) {
    llvm::Value* denormal = nullptr;
    llvm::Value* nan = nullptr;
    for (auto* source : sources) {
        denormal = denormal == nullptr ? IsDenormal(source) : _builder.CreateOr(denormal, IsDenormal(source));
        nan = nan == nullptr ? IsNan(source) : _builder.CreateOr(nan, IsNan(source));
    }
    auto* raised = _builder.CreateAnd(AnyLane(_builder.CreateAnd(denormal, _builder.CreateNot(nan))), _builder.CreateNot(MxcsrSet(PS5LIFT_MXCSR_DENORMALS_ARE_ZERO)));
    return _builder.CreateSelect(raised, _builder.getInt32(PS5LIFT_MXCSR_DENORMAL_OPERAND), _builder.getInt32(0));
}

void FunctionLifter::RaiseSimdExceptions(llvm::Value* before, llvm::Value* after) {
    auto* mxcsr = Mxcsr();
    auto* unmasked = _builder.CreateAnd(_builder.CreateNot(_builder.CreateLShr(mxcsr, PS5LIFT_MXCSR_MASK_SHIFT)), PS5LIFT_MXCSR_EXCEPTIONS);
    auto* stopsBefore = _builder.CreateICmpNE(_builder.CreateAnd(before, unmasked), _builder.getInt32(0));
    StateStore(_builder.CreateOr(mxcsr, _builder.CreateSelect(stopsBefore, before, _builder.CreateOr(before, after))), offsetof(Ps5LiftState, Mxcsr));
    auto* stopsAfter = _builder.CreateICmpNE(_builder.CreateAnd(after, unmasked), _builder.getInt32(0));
    StopIf(_builder.CreateOr(stopsBefore, stopsAfter), PS5LIFT_STOP_SIMD_FLOATING_POINT, 0, Mnemonic() + " raises an unmasked SIMD floating-point exception");
}

llvm::Value* FunctionLifter::BiasedExponent(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* type = IntegerLike(value->getType());
    const auto fraction = FractionBits(element);
    auto* exponent = _builder.CreateLShr(_builder.CreateBitCast(value, type), llvm::ConstantInt::get(type, fraction));
    return _builder.CreateAnd(exponent, llvm::ConstantInt::get(type, (std::uint64_t{1} << (element - 1 - fraction)) - 1));
}

llvm::Value* FunctionLifter::ExponentBelow(llvm::Value* value, const std::uint64_t limit) {
    auto* exponent = BiasedExponent(value);
    auto* one = llvm::ConstantInt::get(exponent->getType(), 1);
    return _builder.CreateICmpULT(_builder.CreateSub(exponent, one), llvm::ConstantInt::get(exponent->getType(), limit - 1));
}

llvm::Value* FunctionLifter::SumExceptional(llvm::Value* a, llvm::Value* b) {
    const auto precision = FractionBits(a->getType()->getScalarSizeInBits()) + 1;
    auto* exceptional = _builder.CreateOr(_builder.CreateOr(IsDenormal(a), IsDenormal(b)), _builder.CreateOr(ExponentBelow(a, precision), ExponentBelow(b, precision)));
    return AnyLane(exceptional);
}

llvm::Value* FunctionLifter::ProductExceptional(llvm::Value* a, llvm::Value* b, const std::uint64_t limit) {
    auto* left = BiasedExponent(a);
    auto* right = BiasedExponent(b);
    auto* zero = llvm::Constant::getNullValue(left->getType());
    auto* nonzero = _builder.CreateAnd(_builder.CreateICmpNE(left, zero), _builder.CreateICmpNE(right, zero));
    auto* small = _builder.CreateICmpULT(_builder.CreateAdd(left, right), llvm::ConstantInt::get(left->getType(), limit));
    return _builder.CreateOr(_builder.CreateOr(IsDenormal(a), IsDenormal(b)), _builder.CreateAnd(nonzero, small));
}

llvm::Value* FunctionLifter::MultiplyExceptional(llvm::Value* a, llvm::Value* b) {
    return AnyLane(ProductExceptional(a, b, ExponentBias(a->getType()->getScalarSizeInBits()) + 2));
}

llvm::Value* FunctionLifter::DivideExceptional(llvm::Value* a, llvm::Value* b) {
    const auto element = a->getType()->getScalarSizeInBits();
    auto* dividend = BiasedExponent(a);
    auto* divisor = BiasedExponent(b);
    auto* type = dividend->getType();
    auto* maximum = llvm::ConstantInt::get(type, 2 * ExponentBias(element) + 1);
    auto* finiteDivisor = _builder.CreateICmpNE(divisor, maximum);
    auto* nonzero = _builder.CreateICmpNE(dividend, llvm::Constant::getNullValue(type));
    auto* small = _builder.CreateICmpULE(_builder.CreateAdd(dividend, llvm::ConstantInt::get(type, ExponentBias(element))), _builder.CreateAdd(divisor, llvm::ConstantInt::get(type, 2)));
    auto* exceptional = _builder.CreateOr(_builder.CreateOr(IsDenormal(a), IsDenormal(b)), _builder.CreateAnd(_builder.CreateAnd(nonzero, finiteDivisor), small));
    return AnyLane(exceptional);
}

llvm::Value* FunctionLifter::FmaExceptional(llvm::Value* a, llvm::Value* b, llvm::Value* c) {
    const auto element = a->getType()->getScalarSizeInBits();
    const auto precision = FractionBits(element) + 1;
    auto* product = ProductExceptional(a, b, ExponentBias(element) + 2 * precision - 1);
    return AnyLane(_builder.CreateOr(product, _builder.CreateOr(IsDenormal(c), ExponentBelow(c, precision))));
}

llvm::Value* FunctionLifter::NarrowExceptional(llvm::Value* value) {
    constexpr std::uint64_t SmallestUnflushedDouble = 1023 - 125;
    return AnyLane(_builder.CreateOr(IsDenormal(value), ExponentBelow(value, SmallestUnflushedDouble)));
}

llvm::Value* FunctionLifter::SimdLanes() {
    if (_simdLanes == nullptr) {
        auto& entry = _host->getEntryBlock();
        llvm::IRBuilder<> builder(&entry, entry.getFirstInsertionPt());
        auto* slots = builder.CreateAlloca(llvm::ArrayType::get(_builder.getInt8Ty(), SlotBytes * SlotCount), nullptr, "simd_lanes");
        slots->setAlignment(llvm::Align(SlotBytes));
        _simdLanes = slots;
    }
    return _simdLanes;
}

llvm::Value* FunctionLifter::ExactOperation(const Ps5LiftSimdOperation operation, std::initializer_list<llvm::Value*> sources, const std::function<llvm::Value*()>& exceptional, const std::function<llvm::Value*()>& host) {
    if (!ExactFloat()) return host();
    auto* software = llvm::BasicBlock::Create(Context(), "simd_software", _host);
    auto* hardware = llvm::BasicBlock::Create(Context(), "simd_host", _host);
    auto* done = llvm::BasicBlock::Create(Context(), "simd_done", _host);
    auto* unmasked = _builder.CreateICmpNE(_builder.CreateAnd(Mxcsr(), ExceptionMasks), _builder.getInt32(ExceptionMasks));
    _builder.CreateCondBr(_builder.CreateOr(exceptional(), unmasked), software, hardware, _module.Unlikely());
    _builder.SetInsertPoint(hardware);
    auto* computed = host();
    auto* hardwareEnd = _builder.GetInsertBlock();
    _builder.CreateBr(done);
    _builder.SetInsertPoint(software);
    auto* lanes = SimdLanes();
    std::size_t slot = 1;
    for (auto* source : sources) _builder.CreateStore(source, _builder.CreateConstGEP1_64(_builder.getInt8Ty(), lanes, SlotBytes * slot++));
    auto* type = sources.begin()[0]->getType();
    const auto count = type->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(type)->getNumElements() : 1u;
    auto callee = _module.Runtime("ps5lift_simd_float", llvm::FunctionType::get(_builder.getInt32Ty(), {_module.Pointer(), _builder.getInt32Ty(), _builder.getInt32Ty(), _builder.getInt32Ty(), _module.Pointer()}, false));
    auto* stops = _builder.CreateCall(callee, {_state, _builder.getInt32(operation), _builder.getInt32(type->getScalarSizeInBits()), _builder.getInt32(count), lanes});
    StopIf(_builder.CreateICmpNE(stops, _builder.getInt32(0)), PS5LIFT_STOP_SIMD_FLOATING_POINT, 0, Mnemonic() + " raises an unmasked SIMD floating-point exception");
    auto* result = _builder.CreateLoad(computed->getType(), lanes);
    auto* softwareEnd = _builder.GetInsertBlock();
    _builder.CreateBr(done);
    _builder.SetInsertPoint(done);
    auto* merged = _builder.CreatePHI(computed->getType(), 2);
    merged->addIncoming(computed, hardwareEnd);
    merged->addIncoming(result, softwareEnd);
    return merged;
}

}
