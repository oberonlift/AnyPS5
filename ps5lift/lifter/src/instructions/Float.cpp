#include "../FunctionLifter.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace Ps5Lift::Lifting {

unsigned FunctionLifter::FractionBits(const unsigned element) {
    return element == 16 ? 10u : element == 32 ? 23u : 52u;
}

std::uint64_t FunctionLifter::ExponentBias(const unsigned element) {
    return (std::uint64_t{1} << (element - 2 - FractionBits(element))) - 1;
}

std::uint64_t FunctionLifter::InfinityBits(const unsigned element) {
    return ((std::uint64_t{1} << (element - 1)) - 1) & ~((std::uint64_t{1} << FractionBits(element)) - 1);
}

llvm::Type* FunctionLifter::FloatType(const unsigned bits) {
    if (bits == 16) return _builder.getHalfTy();
    return bits == 32 ? _builder.getFloatTy() : _builder.getDoubleTy();
}

llvm::Value* FunctionLifter::AsFloats(llvm::Value* value, const unsigned element) {
    const auto bits = value->getType()->getIntegerBitWidth();
    if (bits == element) return _builder.CreateBitCast(value, FloatType(element));
    return _builder.CreateBitCast(value, llvm::FixedVectorType::get(FloatType(element), bits / element));
}

llvm::Value* FunctionLifter::FloatBits(llvm::Value* value) {
    return _builder.CreateBitCast(value, Int(value->getType()->getPrimitiveSizeInBits().getFixedValue()));
}

llvm::Type* FunctionLifter::IntegerLike(llvm::Type* type) {
    const auto element = type->getScalarSizeInBits();
    if (auto* vector = llvm::dyn_cast<llvm::FixedVectorType>(type)) return llvm::FixedVectorType::get(Int(element), vector->getNumElements());
    return Int(element);
}

llvm::Value* FunctionLifter::Quiet(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* integerType = IntegerLike(value->getType());
    auto* bit = llvm::ConstantInt::get(integerType, std::uint64_t{1} << (FractionBits(element) - 1));
    return _builder.CreateBitCast(_builder.CreateOr(_builder.CreateBitCast(value, integerType), bit), value->getType());
}

llvm::Value* FunctionLifter::DefaultNan(llvm::Type* type) {
    const auto element = type->getScalarSizeInBits();
    const auto bits = element == 32 ? std::uint64_t{0xffc00000} : element == 64 ? std::uint64_t{0xfff8000000000000} : std::uint64_t{0xfe00};
    return _builder.CreateBitCast(llvm::ConstantInt::get(IntegerLike(type), bits), type);
}

llvm::Value* FunctionLifter::IsNan(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* type = IntegerLike(value->getType());
    auto* magnitude = _builder.CreateAnd(_builder.CreateBitCast(value, type), llvm::ConstantInt::get(type, (std::uint64_t{1} << (element - 1)) - 1));
    return _builder.CreateICmpUGT(magnitude, llvm::ConstantInt::get(type, InfinityBits(element)));
}

llvm::Value* FunctionLifter::IsSignalingNan(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* type = IntegerLike(value->getType());
    auto* quiet = _builder.CreateAnd(_builder.CreateBitCast(value, type), llvm::ConstantInt::get(type, std::uint64_t{1} << (FractionBits(element) - 1)));
    return _builder.CreateAnd(IsNan(value), _builder.CreateICmpEQ(quiet, llvm::Constant::getNullValue(type)));
}

bool FunctionLifter::ExactFloat() const {
    return _module.Options().Float == FloatModel::Exact;
}

llvm::Value* FunctionLifter::NanResult(llvm::Value* result, std::initializer_list<llvm::Value*> sources) {
    if (!ExactFloat()) return result;
    result = _builder.CreateUnaryIntrinsic(llvm::Intrinsic::arithmetic_fence, result);
    llvm::Value* value = _builder.CreateSelect(IsNan(result), DefaultNan(result->getType()), result);
    std::vector<llvm::Value*> ordered(sources);
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) value = _builder.CreateSelect(IsNan(*it), Quiet(*it), value);
    return value;
}

llvm::Value* FunctionLifter::IsDenormal(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* integerType = IntegerLike(value->getType());
    auto* bits = _builder.CreateBitCast(value, integerType);
    const auto fraction = (std::uint64_t{1} << FractionBits(element)) - 1;
    auto* zero = llvm::Constant::getNullValue(integerType);
    return _builder.CreateAnd(_builder.CreateICmpEQ(_builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, InfinityBits(element))), zero), _builder.CreateICmpNE(_builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, fraction)), zero));
}

llvm::Value* FunctionLifter::DenormalsAreZero(llvm::Value* value) {
    const auto element = value->getType()->getScalarSizeInBits();
    auto* integerType = IntegerLike(value->getType());
    auto* bits = _builder.CreateBitCast(value, integerType);
    llvm::Value* enabled = MxcsrSet(ExactFloat() ? PS5LIFT_MXCSR_DENORMALS_ARE_ZERO : PS5LIFT_MXCSR_DENORMALS_ARE_ZERO | PS5LIFT_MXCSR_FLUSH_TO_ZERO);
    if (value->getType()->isVectorTy()) enabled = _builder.CreateVectorSplat(llvm::cast<llvm::FixedVectorType>(value->getType())->getNumElements(), enabled);
    auto* signOnly = _builder.CreateAnd(bits, llvm::ConstantInt::get(integerType, std::uint64_t{1} << (element - 1)));
    return _builder.CreateBitCast(_builder.CreateSelect(_builder.CreateAnd(enabled, IsDenormal(value)), signOnly, bits), value->getType());
}

llvm::Value* FunctionLifter::FloatConstant(llvm::Type* type, const double value) {
    return llvm::ConstantFP::get(type, value);
}

llvm::Value* FunctionLifter::Sum(llvm::Value* a, llvm::Value* b, llvm::Value* addend) {
    return NanResult(ExactOperation(PS5LIFT_SIMD_ADD, {a, addend}, [&] { return SumExceptional(a, b); }, [&] { return _builder.CreateFAdd(a, addend); }), {a, b});
}

llvm::Value* FunctionLifter::MinMax(const FloatKind kind, llvm::Value* a, llvm::Value* b) {
    llvm::Value* left = a;
    llvm::Value* right = b;
    if (ExactFloat()) {
        RaiseSimdExceptions(_builder.CreateOr(MxcsrFlag(_builder.CreateOr(IsNan(a), IsNan(b)), PS5LIFT_MXCSR_INVALID), DenormalFlag({a, b})), _builder.getInt32(0));
        left = DenormalsAreZero(a);
        right = DenormalsAreZero(b);
    }
    auto* compare = kind == FloatKind::Min ? _builder.CreateFCmpOLT(left, right) : _builder.CreateFCmpOGT(left, right);
    return _builder.CreateSelect(compare, left, right);
}

llvm::Value* FunctionLifter::Arithmetic(const FloatKind kind, llvm::Value* a, llvm::Value* b) {
    switch (kind) {
    case FloatKind::Add: return Sum(a, b, b);
    case FloatKind::Sub: return Sum(a, b, _builder.CreateFNeg(b));
    case FloatKind::Mul: return NanResult(ExactOperation(PS5LIFT_SIMD_MULTIPLY, {a, b}, [&] { return MultiplyExceptional(a, b); }, [&] { return _builder.CreateFMul(a, b); }), {a, b});
    case FloatKind::Div: return NanResult(ExactOperation(PS5LIFT_SIMD_DIVIDE, {a, b}, [&] { return DivideExceptional(a, b); }, [&] { return _builder.CreateFDiv(a, b); }), {a, b});
    case FloatKind::Min:
    case FloatKind::Max: return MinMax(kind, a, b);
    case FloatKind::Sqrt: return NanResult(ExactOperation(PS5LIFT_SIMD_SQRT, {b}, [&] { return AnyLane(IsDenormal(b)); }, [&] { return _builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, b); }), {b});
    case FloatKind::Reciprocal: return Reciprocal(b);
    case FloatKind::ReciprocalSqrt: return ReciprocalSqrt(b);
    default: throw std::logic_error("not an arithmetic kind");
    }
}

llvm::Value* FunctionLifter::RoundValue(llvm::Value* value, const std::uint64_t control) {
    llvm::Intrinsic::ID intrinsic = llvm::Intrinsic::nearbyint;
    if ((control & 4) == 0) {
        switch (control & 3) {
        case 0: intrinsic = llvm::Intrinsic::roundeven; break;
        case 1: intrinsic = llvm::Intrinsic::floor; break;
        case 2: intrinsic = llvm::Intrinsic::ceil; break;
        default: intrinsic = llvm::Intrinsic::trunc; break;
        }
    }
    auto* source = ExactFloat() ? DenormalsAreZero(value) : value;
    auto* rounded = _builder.CreateUnaryIntrinsic(intrinsic, source);
    if (ExactFloat()) {
        auto* type = IntegerLike(value->getType());
        auto* inexact = _builder.CreateAnd(_builder.CreateNot(IsNan(value)), _builder.CreateICmpNE(_builder.CreateBitCast(rounded, type), _builder.CreateBitCast(source, type)));
        RaiseSimdExceptions(MxcsrFlag(IsSignalingNan(value), PS5LIFT_MXCSR_INVALID), (control & 8) != 0 ? _builder.getInt32(0) : MxcsrFlag(inexact, PS5LIFT_MXCSR_PRECISION));
    }
    return NanResult(rounded, {value});
}

llvm::Value* FunctionLifter::CompareMask(llvm::Value* a, llvm::Value* b, const std::uint64_t predicate) {
    static constexpr llvm::CmpInst::Predicate Predicates[16] = {
        llvm::CmpInst::FCMP_OEQ, llvm::CmpInst::FCMP_OLT, llvm::CmpInst::FCMP_OLE, llvm::CmpInst::FCMP_UNO,
        llvm::CmpInst::FCMP_UNE, llvm::CmpInst::FCMP_UGE, llvm::CmpInst::FCMP_UGT, llvm::CmpInst::FCMP_ORD,
        llvm::CmpInst::FCMP_UEQ, llvm::CmpInst::FCMP_ULT, llvm::CmpInst::FCMP_ULE, llvm::CmpInst::FCMP_FALSE,
        llvm::CmpInst::FCMP_ONE, llvm::CmpInst::FCMP_OGE, llvm::CmpInst::FCMP_OGT, llvm::CmpInst::FCMP_TRUE};
    const bool legacy = _instruction->Info.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY;
    const auto code = predicate & (legacy ? 7 : 31);
    const auto relation = Predicates[code & 15];
    llvm::Value* compare = nullptr;
    if (ExactFloat()) {
        const bool signaling = ((code & 3) == 1 || (code & 3) == 2) != ((code & 16) != 0);
        auto* nan = _builder.CreateOr(IsNan(a), IsNan(b));
        auto* invalid = signaling ? nan : _builder.CreateOr(IsSignalingNan(a), IsSignalingNan(b));
        RaiseSimdExceptions(_builder.CreateOr(MxcsrFlag(invalid, PS5LIFT_MXCSR_INVALID), DenormalFlag({a, b})), _builder.getInt32(0));
        auto* zero = llvm::Constant::getNullValue(a->getType());
        auto* ordered = _builder.CreateFCmp(relation, _builder.CreateSelect(nan, zero, DenormalsAreZero(a)), _builder.CreateSelect(nan, zero, DenormalsAreZero(b)));
        const bool unordered = (static_cast<unsigned>(relation) & llvm::CmpInst::FCMP_UNO) != 0;
        compare = _builder.CreateSelect(nan, llvm::ConstantInt::getBool(nan->getType(), unordered), ordered);
    } else {
        compare = _builder.CreateFCmp(relation, a, b);
    }
    return _builder.CreateBitCast(_builder.CreateSExt(compare, IntegerLike(a->getType())), a->getType());
}

llvm::Value* FunctionLifter::FusedMultiplyAdd(const FloatSpec& spec, llvm::Value* x1, llvm::Value* x2, llvm::Value* x3) {
    llvm::Value* left = nullptr;
    llvm::Value* right = nullptr;
    llvm::Value* addend = nullptr;
    if (spec.Order == 132) {
        left = x1;
        right = x3;
        addend = x2;
    } else if (spec.Order == 213) {
        left = x2;
        right = x1;
        addend = x3;
    } else {
        left = x2;
        right = x3;
        addend = x1;
    }
    auto* type = left->getType();
    auto* product = spec.NegateProduct ? _builder.CreateFNeg(left) : left;
    llvm::Value* signedAddend = spec.SubtractAddend ? _builder.CreateFNeg(addend) : addend;
    if (spec.Alternate != 0) {
        const auto count = llvm::cast<llvm::FixedVectorType>(type)->getNumElements();
        std::vector<llvm::Constant*> subtract(count);
        for (unsigned i = 0; i < count; ++i) subtract[i] = _builder.getInt1((i % 2 == 0) == (spec.Alternate == 1));
        signedAddend = _builder.CreateSelect(llvm::ConstantVector::get(subtract), _builder.CreateFNeg(addend), addend);
    }
    auto* fused = ExactOperation(PS5LIFT_SIMD_FMA, {product, right, signedAddend}, [&] { return FmaExceptional(left, right, addend); }, [&] { return _builder.CreateIntrinsic(llvm::Intrinsic::fma, {type}, {product, right, signedAddend}); });
    return NanResult(fused, {left, right, addend});
}

llvm::Value* FunctionLifter::LoadScalarFloat(const int index, const unsigned element) {
    return _builder.CreateBitCast(ReadAs(index, element), FloatType(element));
}

void FunctionLifter::FloatArithmetic(const FloatSpec& spec) {
    const auto element = spec.Element;
    const auto kind = spec.Kind;
    const bool unary = kind == FloatKind::Sqrt || kind == FloatKind::Reciprocal || kind == FloatKind::ReciprocalSqrt;
    if (spec.Scalar) {
        int first = 0;
        int second = 1;
        if (kind == FloatKind::Fma) {
            auto* destination = AsFloats(ReadVector(0, 128), element);
            auto* x1 = _builder.CreateExtractElement(destination, _builder.getInt64(0));
            auto* x2 = _builder.CreateExtractElement(AsFloats(ReadVector(1, 128), element), _builder.getInt64(0));
            auto* x3 = LoadScalarFloat(2, element);
            Write(0, FloatBits(_builder.CreateInsertElement(destination, FusedMultiplyAdd(spec, x1, x2, x3), _builder.getInt64(0))));
            return;
        }
        const bool withImmediate = kind == FloatKind::Round || kind == FloatKind::Compare;
        first = Base(withImmediate ? 3 : 2);
        second = first + 1;
        auto* source = AsFloats(ReadVector(first, 128), element);
        auto* a = _builder.CreateExtractElement(source, _builder.getInt64(0));
        auto* b = LoadScalarFloat(second, element);
        llvm::Value* value = nullptr;
        if (kind == FloatKind::Round) value = RoundValue(b, Immediate(second + 1));
        else if (kind == FloatKind::Compare) value = CompareMask(a, b, Immediate(second + 1));
        else value = Arithmetic(kind, a, b);
        Write(0, FloatBits(_builder.CreateInsertElement(source, value, _builder.getInt64(0))));
        return;
    }

    const auto width = Bits(0);
    const unsigned count = width / element;
    const unsigned perLane = 128 / element;
    if (kind == FloatKind::Fma) {
        Write(0, FloatBits(FusedMultiplyAdd(spec, AsFloats(ReadAs(0, width), element), AsFloats(ReadAs(1, width), element), AsFloats(ReadAs(2, width), element))));
        return;
    }
    if (unary || kind == FloatKind::Round) {
        auto* source = AsFloats(ReadAs(1, width), element);
        Write(0, FloatBits(kind == FloatKind::Round ? RoundValue(source, Immediate(2)) : Arithmetic(kind, source, source)));
        return;
    }
    const int base = Base(kind == FloatKind::Compare ? 3 : 2);
    auto* a = AsFloats(ReadAs(base, width), element);
    auto* b = AsFloats(ReadAs(base + 1, width), element);
    llvm::Value* result = nullptr;
    switch (kind) {
    case FloatKind::Compare:
        result = CompareMask(a, b, Immediate(base + 2));
        break;
    case FloatKind::AddSub: {
        std::vector<llvm::Constant*> subtract(count);
        for (unsigned i = 0; i < count; ++i) subtract[i] = _builder.getInt1(i % 2 == 0);
        result = Sum(a, b, _builder.CreateSelect(llvm::ConstantVector::get(subtract), _builder.CreateFNeg(b), b));
        break;
    }
    case FloatKind::HorizontalAdd:
    case FloatKind::HorizontalSub: {
        const auto [left, right] = HorizontalOperands(a, b, count, perLane);
        result = Arithmetic(kind == FloatKind::HorizontalAdd ? FloatKind::Add : FloatKind::Sub, left, right);
        break;
    }
    default:
        result = Arithmetic(kind, a, b);
        break;
    }
    Write(0, FloatBits(result));
}

llvm::Value* FunctionLifter::ConvertNan(llvm::Value* source, llvm::Value* converted) {
    if (!ExactFloat()) return converted;
    const auto from = source->getType()->getScalarSizeInBits();
    const auto to = converted->getType()->getScalarSizeInBits();
    auto* integerSource = _builder.CreateBitCast(source, IntegerLike(source->getType()));
    auto* targetInteger = IntegerLike(converted->getType());
    auto* sign = _builder.CreateICmpSLT(integerSource, llvm::Constant::getNullValue(integerSource->getType()));
    auto* payload = _builder.CreateAnd(integerSource, llvm::ConstantInt::get(integerSource->getType(), (std::uint64_t{1} << FractionBits(from)) - 1));
    llvm::Value* moved = nullptr;
    if (FractionBits(to) >= FractionBits(from)) moved = _builder.CreateShl(_builder.CreateZExt(payload, targetInteger), FractionBits(to) - FractionBits(from));
    else moved = _builder.CreateTrunc(_builder.CreateLShr(payload, FractionBits(from) - FractionBits(to)), targetInteger);
    const auto exponentAndQuiet = ((std::uint64_t{1} << (to - 1 - FractionBits(to))) - 1) << FractionBits(to) | (std::uint64_t{1} << (FractionBits(to) - 1));
    auto* signBit = _builder.CreateSelect(sign, llvm::ConstantInt::get(targetInteger, std::uint64_t{1} << (to - 1)), llvm::Constant::getNullValue(targetInteger));
    auto* nan = _builder.CreateOr(_builder.CreateOr(moved, llvm::ConstantInt::get(targetInteger, exponentAndQuiet)), signBit);
    return _builder.CreateSelect(IsNan(source), _builder.CreateBitCast(nan, converted->getType()), converted);
}

llvm::Value* FunctionLifter::ConvertFloat(llvm::Value* source, const unsigned element) {
    auto* type = source->getType()->isVectorTy() ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(FloatType(element), llvm::cast<llvm::FixedVectorType>(source->getType())->getNumElements())) : FloatType(element);
    if (element < source->getType()->getScalarSizeInBits()) {
        return ConvertNan(source, ExactOperation(PS5LIFT_SIMD_NARROW, {source}, [&] { return NarrowExceptional(source); }, [&] { return _builder.CreateFPTrunc(source, type); }));
    }
    return ConvertNan(source, ExactOperation(PS5LIFT_SIMD_WIDEN, {source}, [&] { return AnyLane(IsDenormal(source)); }, [&] { return _builder.CreateFPExt(source, type); }));
}

llvm::Value* FunctionLifter::HalfToSingle(llvm::Value* source) {
    auto* type = llvm::FixedVectorType::get(FloatType(32), llvm::cast<llvm::FixedVectorType>(source->getType())->getNumElements());
    if (!ExactFloat()) return _builder.CreateFPExt(source, type);
    RaiseSimdExceptions(MxcsrFlag(IsSignalingNan(source), PS5LIFT_MXCSR_INVALID), _builder.getInt32(0));
    auto* ordered = _builder.CreateSelect(IsNan(source), llvm::Constant::getNullValue(source->getType()), source);
    return ConvertNan(source, _builder.CreateFPExt(ordered, type));
}

llvm::Value* FunctionLifter::SingleToHalf(llvm::Value* source, llvm::Value* mode) {
    auto* type = llvm::cast<llvm::FixedVectorType>(source->getType());
    const auto count = type->getNumElements();
    auto* i32 = llvm::FixedVectorType::get(Int(32), count);
    auto* x = _builder.CreateBitCast(DenormalsAreZero(source), i32);
    const auto constant = [&](const std::uint32_t value) { return llvm::ConstantInt::get(i32, value); };
    const auto modeIs = [&](const std::uint32_t value) { return _builder.CreateVectorSplat(count, _builder.CreateICmpEQ(mode, _builder.getInt32(value))); };
    auto* sign = _builder.CreateAnd(_builder.CreateLShr(x, 16), constant(0x8000));
    auto* negative = _builder.CreateICmpNE(sign, constant(0));
    const auto increment = [&](llvm::Value* remainder, llvm::Value* half, llvm::Value* kept) {
        auto* inexact = _builder.CreateICmpNE(remainder, constant(0));
        auto* nearest = _builder.CreateOr(_builder.CreateICmpUGT(remainder, half), _builder.CreateAnd(_builder.CreateICmpEQ(remainder, half), _builder.CreateICmpNE(_builder.CreateAnd(kept, constant(1)), constant(0))));
        auto* down = _builder.CreateAnd(inexact, negative);
        auto* up = _builder.CreateAnd(inexact, _builder.CreateNot(negative));
        auto* none = llvm::Constant::getNullValue(nearest->getType());
        return _builder.CreateSelect(modeIs(0), nearest, _builder.CreateSelect(modeIs(1), down, _builder.CreateSelect(modeIs(2), up, none)));
    };
    auto* magnitude = _builder.CreateAnd(x, constant(0x7fffffff));
    auto* exponent = _builder.CreateLShr(magnitude, 23);
    auto* implicit = _builder.CreateSelect(_builder.CreateICmpEQ(exponent, constant(0)), constant(0), constant(0x800000));
    auto* mantissa = _builder.CreateOr(_builder.CreateAnd(magnitude, constant(0x7fffff)), implicit);
    auto* subnormal = _builder.CreateICmpULT(magnitude, constant(0x38800000));
    auto* rawShift = _builder.CreateSub(constant(126), exponent);
    auto* shift = _builder.CreateSelect(subnormal, _builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, rawShift, constant(25)), constant(13));
    auto* base = _builder.CreateSelect(subnormal, _builder.CreateLShr(mantissa, shift), _builder.CreateLShr(_builder.CreateSub(magnitude, constant(0x38000000)), 13));
    auto* remainderMask = _builder.CreateSub(_builder.CreateShl(constant(1), shift), constant(1));
    auto* remainder = _builder.CreateAnd(_builder.CreateSelect(subnormal, mantissa, magnitude), remainderMask);
    auto* half = _builder.CreateShl(constant(1), _builder.CreateSub(shift, constant(1)));
    auto* rounded = _builder.CreateAdd(base, _builder.CreateZExt(increment(remainder, half, base), i32));
    auto* largestPositive = _builder.CreateSelect(_builder.CreateOr(modeIs(0), modeIs(2)), constant(0x7c00), constant(0x7bff));
    auto* largestNegative = _builder.CreateSelect(_builder.CreateOr(modeIs(0), modeIs(1)), constant(0x7c00), constant(0x7bff));
    auto* overflowValue = _builder.CreateSelect(negative, largestNegative, largestPositive);
    auto* overflow = _builder.CreateOr(_builder.CreateICmpUGE(magnitude, constant(0x47800000)), _builder.CreateICmpUGT(rounded, constant(0x7bff)));
    llvm::Value* finite = _builder.CreateSelect(overflow, overflowValue, rounded);
    auto* nan = _builder.CreateOr(constant(0x7e00), _builder.CreateLShr(_builder.CreateAnd(magnitude, constant(0x7fffff)), 13));
    auto* isNan = _builder.CreateICmpUGT(magnitude, constant(0x7f800000));
    auto* isInfinity = _builder.CreateICmpEQ(magnitude, constant(0x7f800000));
    if (ExactFloat()) {
        auto* isFinite = _builder.CreateICmpULT(magnitude, constant(0x7f800000));
        auto* overflowed = _builder.CreateAnd(isFinite, overflow);
        auto* inexact = _builder.CreateAnd(isFinite, _builder.CreateOr(overflow, _builder.CreateICmpNE(remainder, constant(0))));
        auto* normalRemainder = _builder.CreateAnd(magnitude, constant(0x1fff));
        auto* normalKept = _builder.CreateLShr(magnitude, 13);
        auto* normalRounded = _builder.CreateAdd(normalKept, _builder.CreateZExt(increment(normalRemainder, constant(0x1000), normalKept), i32));
        auto* tiny = _builder.CreateAnd(_builder.CreateICmpNE(magnitude, constant(0)), _builder.CreateICmpULT(normalRounded, constant(0x38800000 >> 13)));
        auto* underflowMasked = _builder.CreateVectorSplat(count, MxcsrSet(PS5LIFT_MXCSR_UNDERFLOW << PS5LIFT_MXCSR_MASK_SHIFT));
        auto* underflow = _builder.CreateAnd(tiny, _builder.CreateSelect(underflowMasked, inexact, llvm::Constant::getAllOnesValue(inexact->getType())));
        auto* before = _builder.CreateOr(MxcsrFlag(IsSignalingNan(source), PS5LIFT_MXCSR_INVALID), DenormalFlag({source}));
        auto* after = _builder.CreateOr(_builder.CreateOr(MxcsrFlag(overflowed, PS5LIFT_MXCSR_OVERFLOW), MxcsrFlag(underflow, PS5LIFT_MXCSR_UNDERFLOW)), MxcsrFlag(inexact, PS5LIFT_MXCSR_PRECISION));
        RaiseSimdExceptions(before, after);
    }
    finite = _builder.CreateSelect(isInfinity, constant(0x7c00), finite);
    finite = _builder.CreateSelect(isNan, nan, finite);
    auto* result = _builder.CreateOr(finite, sign);
    return _builder.CreateTrunc(result, llvm::FixedVectorType::get(Int(16), count));
}

llvm::Value* FunctionLifter::FloatToInteger(llvm::Value* value, const unsigned bits, const bool truncate) {
    auto* source = ExactFloat() ? DenormalsAreZero(value) : value;
    auto* rounded = _builder.CreateUnaryIntrinsic(truncate ? llvm::Intrinsic::trunc : llvm::Intrinsic::nearbyint, source);
    auto* lower = FloatConstant(value->getType(), -std::ldexp(1.0, static_cast<int>(bits) - 1));
    auto* upper = FloatConstant(value->getType(), std::ldexp(1.0, static_cast<int>(bits) - 1));
    auto* inRange = _builder.CreateAnd(_builder.CreateFCmpOGE(rounded, lower), _builder.CreateFCmpOLT(rounded, upper));
    if (ExactFloat()) {
        auto* type = IntegerLike(value->getType());
        auto* inexact = _builder.CreateAnd(inRange, _builder.CreateICmpNE(_builder.CreateBitCast(rounded, type), _builder.CreateBitCast(source, type)));
        RaiseSimdExceptions(MxcsrFlag(_builder.CreateNot(inRange), PS5LIFT_MXCSR_INVALID), MxcsrFlag(inexact, PS5LIFT_MXCSR_PRECISION));
    }
    auto* targetType = value->getType()->isVectorTy() ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(Int(bits), llvm::cast<llvm::FixedVectorType>(value->getType())->getNumElements())) : Int(bits);
    auto* safe = _builder.CreateSelect(inRange, rounded, llvm::Constant::getNullValue(value->getType()));
    auto* converted = _builder.CreateFPToSI(safe, targetType);
    return _builder.CreateSelect(inRange, converted, llvm::ConstantInt::get(targetType, std::uint64_t{1} << (bits - 1)));
}

llvm::Value* FunctionLifter::IntegerToFloat(llvm::Value* integer, llvm::Type* type) {
    auto* converted = _builder.CreateSIToFP(integer, type);
    const auto width = integer->getType()->getScalarSizeInBits();
    const auto precision = FractionBits(type->getScalarSizeInBits()) + 1;
    if (!ExactFloat() || width <= precision) return converted;
    auto* magnitude = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::abs, integer, _builder.getFalse());
    auto* leading = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, magnitude, _builder.getFalse());
    auto* trailing = _builder.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, magnitude, _builder.getFalse());
    auto* significant = _builder.CreateSub(_builder.CreateSub(llvm::ConstantInt::get(magnitude->getType(), width), leading), trailing);
    auto* inexact = _builder.CreateICmpSGT(significant, llvm::ConstantInt::get(magnitude->getType(), precision));
    RaiseSimdExceptions(_builder.getInt32(0), MxcsrFlag(inexact, PS5LIFT_MXCSR_PRECISION));
    return converted;
}

void FunctionLifter::WriteVectorLow(const int index, llvm::Value* vector, const unsigned width) {
    auto* bits = FloatBits(vector);
    Write(index, _builder.CreateZExtOrTrunc(bits, Int(width)));
}

void FunctionLifter::Convert(const ZydisMnemonic mnemonic) {
    const bool scalarSource = mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSI2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SD
        || mnemonic == ZYDIS_MNEMONIC_CVTSS2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSS2SD || mnemonic == ZYDIS_MNEMONIC_CVTSD2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SS;
    if (scalarSource) {
        const int first = Base(2);
        const auto target = mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSD2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SS ? 32u : 64u;
        auto* destination = AsFloats(ReadVector(first, 128), target);
        llvm::Value* value = nullptr;
        if (mnemonic == ZYDIS_MNEMONIC_CVTSI2SS || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SS || mnemonic == ZYDIS_MNEMONIC_CVTSI2SD || mnemonic == ZYDIS_MNEMONIC_VCVTSI2SD) {
            value = IntegerToFloat(Read(first + 1), FloatType(target));
        } else {
            auto* source = LoadScalarFloat(first + 1, target == 32 ? 64 : 32);
            value = ConvertFloat(source, target);
        }
        Write(0, FloatBits(_builder.CreateInsertElement(destination, value, _builder.getInt64(0))));
        return;
    }
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_VCVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI: case ZYDIS_MNEMONIC_VCVTSS2SI:
    case ZYDIS_MNEMONIC_CVTTSD2SI: case ZYDIS_MNEMONIC_VCVTTSD2SI: case ZYDIS_MNEMONIC_CVTSD2SI: case ZYDIS_MNEMONIC_VCVTSD2SI: {
        const bool isDouble = mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_CVTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTSD2SI;
        const bool truncate = mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSS2SI || mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI || mnemonic == ZYDIS_MNEMONIC_VCVTTSD2SI;
        Write(0, FloatToInteger(LoadScalarFloat(1, isDouble ? 64 : 32), Bits(0), truncate));
        return;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PS: case ZYDIS_MNEMONIC_VCVTDQ2PS: {
        const auto width = Bits(0);
        Write(0, FloatBits(IntegerToFloat(AsVector(ReadAs(1, width), 32), llvm::FixedVectorType::get(FloatType(32), width / 32))));
        return;
    }
    case ZYDIS_MNEMONIC_CVTPI2PS: {
        auto* converted = IntegerToFloat(AsVector(ReadAs(1, 64), 32), llvm::FixedVectorType::get(FloatType(32), 2));
        llvm::Value* destination = AsFloats(ReadVector(0, 128), 32);
        for (unsigned lane = 0; lane < 2; ++lane) destination = _builder.CreateInsertElement(destination, _builder.CreateExtractElement(converted, lane), lane);
        Write(0, FloatBits(destination));
        return;
    }
    case ZYDIS_MNEMONIC_CVTTPS2DQ: case ZYDIS_MNEMONIC_VCVTTPS2DQ: case ZYDIS_MNEMONIC_CVTPS2DQ: case ZYDIS_MNEMONIC_VCVTPS2DQ:
    case ZYDIS_MNEMONIC_CVTTPS2PI: case ZYDIS_MNEMONIC_CVTPS2PI: {
        const auto width = Bits(0);
        const bool truncate = mnemonic == ZYDIS_MNEMONIC_CVTTPS2DQ || mnemonic == ZYDIS_MNEMONIC_VCVTTPS2DQ || mnemonic == ZYDIS_MNEMONIC_CVTTPS2PI;
        Write(0, AsInteger(FloatToInteger(AsFloats(ReadAs(1, width), 32), 32, truncate)));
        return;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PD: case ZYDIS_MNEMONIC_VCVTDQ2PD: case ZYDIS_MNEMONIC_CVTPI2PD: {
        const auto count = Bits(0) / 64;
        auto* source = AsVector(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 32), Int(count * 32)), 32);
        Write(0, FloatBits(IntegerToFloat(source, llvm::FixedVectorType::get(FloatType(64), count))));
        return;
    }
    case ZYDIS_MNEMONIC_CVTPS2PD: case ZYDIS_MNEMONIC_VCVTPS2PD: {
        const auto count = Bits(0) / 64;
        auto* source = AsFloats(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 32), Int(count * 32)), 32);
        Write(0, FloatBits(ConvertFloat(source, 64)));
        return;
    }
    case ZYDIS_MNEMONIC_CVTPD2PS: case ZYDIS_MNEMONIC_VCVTPD2PS:
    case ZYDIS_MNEMONIC_CVTPD2DQ: case ZYDIS_MNEMONIC_VCVTPD2DQ: case ZYDIS_MNEMONIC_CVTTPD2DQ: case ZYDIS_MNEMONIC_VCVTTPD2DQ:
    case ZYDIS_MNEMONIC_CVTPD2PI: case ZYDIS_MNEMONIC_CVTTPD2PI: {
        auto* source = AsFloats(ReadAs(1, Bits(1)), 64);
        llvm::Value* converted = nullptr;
        if (mnemonic == ZYDIS_MNEMONIC_CVTPD2PS || mnemonic == ZYDIS_MNEMONIC_VCVTPD2PS) converted = ConvertFloat(source, 32);
        else converted = FloatToInteger(source, 32, mnemonic == ZYDIS_MNEMONIC_CVTTPD2DQ || mnemonic == ZYDIS_MNEMONIC_VCVTTPD2DQ || mnemonic == ZYDIS_MNEMONIC_CVTTPD2PI);
        WriteVectorLow(0, converted, Bits(0));
        return;
    }
    case ZYDIS_MNEMONIC_VCVTPH2PS: {
        const auto count = Bits(0) / 32;
        auto* source = _builder.CreateBitCast(_builder.CreateZExtOrTrunc(ReadRegisterOrMemory(1, count * 16), Int(count * 16)), llvm::FixedVectorType::get(FloatType(16), count));
        Write(0, FloatBits(HalfToSingle(source)));
        return;
    }
    case ZYDIS_MNEMONIC_VCVTPS2PH: {
        const auto control = Immediate(2);
        auto* source = AsFloats(ReadRegister(Operand(1).reg.value), 32);
        auto* mode = (control & 4) != 0 ? _builder.CreateAnd(_builder.CreateLShr(Mxcsr(), 13), 3) : _builder.getInt32(static_cast<std::uint32_t>(control & 3));
        auto* bits = AsInteger(SingleToHalf(source, mode));
        if (Operand(0).type == ZYDIS_OPERAND_TYPE_MEMORY) Write(0, bits);
        else WriteRegister(Operand(0).reg.value, _builder.CreateZExt(bits, Int(128)));
        return;
    }
    default:
        UnexpectedMnemonic("Convert", mnemonic);
    }
}

void FunctionLifter::CompareScalar(const unsigned element) {
    auto* a = LoadScalarFloat(0, element);
    auto* b = LoadScalarFloat(1, element);
    if (!ExactFloat()) {
        ComparisonFlags(_builder.CreateFCmpUNO(a, b), [&] { return _builder.CreateFCmpUEQ(a, b); }, [&] { return _builder.CreateFCmpULT(a, b); });
        return;
    }
    const auto mnemonic = _instruction->Info.mnemonic;
    const bool signaling = mnemonic == ZYDIS_MNEMONIC_COMISS || mnemonic == ZYDIS_MNEMONIC_VCOMISS || mnemonic == ZYDIS_MNEMONIC_COMISD || mnemonic == ZYDIS_MNEMONIC_VCOMISD;
    auto* nan = _builder.CreateOr(IsNan(a), IsNan(b));
    auto* invalid = signaling ? nan : _builder.CreateOr(IsSignalingNan(a), IsSignalingNan(b));
    RaiseSimdExceptions(_builder.CreateOr(MxcsrFlag(invalid, PS5LIFT_MXCSR_INVALID), DenormalFlag({a, b})), _builder.getInt32(0));
    auto* zero = llvm::Constant::getNullValue(a->getType());
    auto* left = _builder.CreateSelect(nan, zero, DenormalsAreZero(a));
    auto* right = _builder.CreateSelect(nan, zero, DenormalsAreZero(b));
    ComparisonFlags(nan, [&] { return _builder.CreateOr(nan, _builder.CreateFCmpOEQ(left, right)); }, [&] { return _builder.CreateOr(nan, _builder.CreateFCmpOLT(left, right)); });
}

void FunctionLifter::DotProduct(const unsigned element) {
    const int base = Base(3);
    const auto width = Bits(0);
    const auto control = Immediate(base + 2);
    const unsigned count = width / element;
    const unsigned perLane = 128 / element;
    auto* a = AsFloats(ReadAs(base, width), element);
    auto* b = AsFloats(ReadAs(base + 1, width), element);
    auto* type = a->getType();
    auto* zero = llvm::Constant::getNullValue(type);
    std::vector<llvm::Constant*> selected(count);
    for (unsigned i = 0; i < count; ++i) selected[i] = _builder.getInt1(((control >> (4 + i % perLane)) & 1) != 0);
    auto* chosen = llvm::ConstantVector::get(selected);
    auto* products = Arithmetic(FloatKind::Mul, _builder.CreateSelect(chosen, a, zero), _builder.CreateSelect(chosen, b, zero));
    llvm::Value* result = zero;
    for (unsigned lane = 0; lane < count; lane += perLane) {
        const auto product = [&](const unsigned i) { return _builder.CreateExtractElement(products, _builder.getInt64(lane + i)); };
        const auto add = [&](llvm::Value* a, llvm::Value* b) { return Arithmetic(FloatKind::Add, a, b); };
        auto* sum = perLane == 4 ? add(add(product(0), product(1)), add(product(2), product(3))) : add(product(0), product(1));
        for (unsigned i = 0; i < perLane; ++i) {
            if ((control >> i) & 1) result = _builder.CreateInsertElement(result, sum, _builder.getInt64(lane + i));
        }
    }
    Write(0, FloatBits(result));
}

void FunctionLifter::LoadMxcsrOperand() {
    Flush();
    LoadMxcsr(Read(0));
}

void FunctionLifter::LoadMxcsr(llvm::Value* value) {
    auto callee = _module.Runtime("ps5lift_load_mxcsr", llvm::FunctionType::get(_builder.getVoidTy(), {_module.Pointer(), _builder.getInt32Ty(), I64(), _module.Pointer()}, false));
    _builder.CreateCall(callee, {_state, value, _builder.getInt64(_instruction->Address), _builder.CreateGlobalString(Mnemonic())});
}

void FunctionLifter::StoreMxcsr() {
    auto callee = _module.Runtime("ps5lift_read_mxcsr", llvm::FunctionType::get(_builder.getInt32Ty(), {_module.Pointer()}, false));
    Write(0, _builder.CreateCall(callee, {_state}));
}

}
