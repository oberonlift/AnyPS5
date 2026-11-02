#include "SpirvBackend/SpirvMemory/SpirvAtomicEmulation.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include "SpirvBackend/SpirvMemory/SpirvSubgroup.hpp"
#include "RuntimeAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <stdexcept>

namespace ShaderRecompiler {

std::uint32_t AtomicNext(SpirvEmitterState& state, std::uint32_t type, std::uint32_t opcode, std::uint32_t old, std::uint32_t value, std::uint32_t comparator) {
    const auto winner = [&](spv::Op compare) {
        return Select(state, type, Binary(state, compare, TypeBool(state), old, value), old, value);
    };
    switch (opcode) {
    case spv::OpAtomicExchange:
        return value;
    case spv::OpAtomicCompareExchange:
        return Select(state, type, Binary(state, spv::OpIEqual, TypeBool(state), old, comparator), value, old);
    case spv::OpAtomicIAdd:
        return Binary(state, spv::OpIAdd, type, old, value);
    case spv::OpAtomicISub:
        return Binary(state, spv::OpISub, type, old, value);
    case spv::OpAtomicSMin:
        return winner(spv::OpSLessThan);
    case spv::OpAtomicUMin:
        return winner(spv::OpULessThan);
    case spv::OpAtomicSMax:
        return winner(spv::OpSGreaterThan);
    case spv::OpAtomicUMax:
        return winner(spv::OpUGreaterThan);
    case spv::OpAtomicAnd:
        return Binary(state, spv::OpBitwiseAnd, type, old, value);
    case spv::OpAtomicOr:
        return Binary(state, spv::OpBitwiseOr, type, old, value);
    case spv::OpAtomicXor:
        return Binary(state, spv::OpBitwiseXor, type, old, value);
    default:
        throw std::runtime_error("AtomicNext: opcode has no replacement");
    }
}

std::uint32_t EmitLockedUpdate(SpirvEmitterState& state, const SpirvLock& lock, std::uint32_t type, std::uint32_t zero, const std::function<std::uint32_t()>& update) {
    const auto entry = state.currentLabel;
    const auto header = state.module.AllocateId();
    const auto body = state.module.AllocateId();
    const auto pending = state.module.AllocateId();
    const auto attempt = state.module.AllocateId();
    const auto attemptMerge = state.module.AllocateId();
    const auto critical = state.module.AllocateId();
    const auto servedMerge = state.module.AllocateId();
    const auto pendingMerge = state.module.AllocateId();
    const auto cont = state.module.AllocateId();
    const auto merge = state.module.AllocateId();
    const auto done = state.module.AllocateId();
    const auto result = state.module.AllocateId();
    const auto doneNext = state.module.AllocateId();
    const auto resultNext = state.module.AllocateId();
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, header);
    state.module.AddFunction(spv::OpPhi, TypeBool(state), done, ConstantBool(state, false), entry, doneNext, cont);
    state.module.AddFunction(spv::OpPhi, type, result, zero, entry, resultNext, cont);
    state.module.AddFunction(spv::OpLoopMerge, merge, cont, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranch, body);
    EmitLabel(state, body);
    state.module.AddFunction(spv::OpSelectionMerge, pendingMerge, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, done, pendingMerge, pending);
    EmitLabel(state, pending);
    const auto elected = state.module.AllocateId();
    EmitSubgroupElect(state, elected);
    state.module.AddFunction(spv::OpSelectionMerge, servedMerge, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, elected, attempt, servedMerge);
    EmitLabel(state, attempt);
    const auto previous = state.module.AllocateId();
    state.module.AddFunction(spv::OpAtomicCompareExchange, TypeU32(state), previous, lock.pointer, ConstantU32(state, lock.scope), ConstantU32(state, spv::MemorySemanticsAcquireMask | lock.semantics), ConstantU32(state, spv::MemorySemanticsMaskNone), ConstantU32(state, 1u), ConstantU32(state, 0u));
    const auto acquired = Binary(state, spv::OpIEqual, TypeBool(state), previous, ConstantU32(state, 0u));
    state.module.AddFunction(spv::OpSelectionMerge, attemptMerge, spv::SelectionControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, acquired, critical, attemptMerge);
    EmitLabel(state, critical);
    if (lock.fences) state.module.AddFunction(spv::OpMemoryBarrier, ConstantU32(state, lock.scope), ConstantU32(state, spv::MemorySemanticsAcquireMask | lock.semantics));
    const auto old = update();
    if (lock.fences) state.module.AddFunction(spv::OpMemoryBarrier, ConstantU32(state, lock.scope), ConstantU32(state, spv::MemorySemanticsReleaseMask | lock.semantics));
    const auto released = state.module.AllocateId();
    state.module.AddFunction(spv::OpAtomicExchange, TypeU32(state), released, lock.pointer, ConstantU32(state, lock.scope), ConstantU32(state, spv::MemorySemanticsReleaseMask | lock.semantics), ConstantU32(state, 0u));
    const auto criticalExit = state.currentLabel;
    state.module.AddFunction(spv::OpBranch, attemptMerge);
    EmitLabel(state, attemptMerge);
    const auto attempted = state.module.AllocateId();
    const auto attemptedResult = state.module.AllocateId();
    state.module.AddFunction(spv::OpPhi, TypeBool(state), attempted, ConstantBool(state, true), criticalExit, ConstantBool(state, false), attempt);
    state.module.AddFunction(spv::OpPhi, type, attemptedResult, old, criticalExit, result, attempt);
    state.module.AddFunction(spv::OpBranch, servedMerge);
    EmitLabel(state, servedMerge);
    const auto served = state.module.AllocateId();
    const auto servedResult = state.module.AllocateId();
    state.module.AddFunction(spv::OpPhi, TypeBool(state), served, attempted, attemptMerge, ConstantBool(state, false), pending);
    state.module.AddFunction(spv::OpPhi, type, servedResult, attemptedResult, attemptMerge, result, pending);
    state.module.AddFunction(spv::OpBranch, pendingMerge);
    EmitLabel(state, pendingMerge);
    state.module.AddFunction(spv::OpPhi, TypeBool(state), doneNext, served, servedMerge, done, body);
    state.module.AddFunction(spv::OpPhi, type, resultNext, servedResult, servedMerge, result, body);
    const auto ballot = state.module.AllocateId();
    EmitHostSubgroupBallot(state, ballot, Unary(state, spv::OpLogicalNot, TypeBool(state), doneNext));
    std::uint32_t remaining = ConstantU32(state, 0u);
    for (std::uint32_t component = 0; component < 4u; ++component) {
        const auto word = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), word, ballot, component);
        remaining = Binary(state, spv::OpBitwiseOr, TypeU32(state), remaining, word);
    }
    state.module.AddFunction(spv::OpBranchConditional, Binary(state, spv::OpINotEqual, TypeBool(state), remaining, ConstantU32(state, 0u)), cont, merge);
    EmitLabel(state, cont);
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, merge);
    return resultNext;
}

std::uint32_t EmitAtomicLockPointer(SpirvEmitterState& state, std::span<const std::uint32_t> coordinates) {
    if (state.atomicLocksVariable == 0u) FailEmit("an emulated 64-bit atomic needs the atomic lock table binding");
    constexpr std::array<std::uint32_t, 3> multipliers{0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du};
    if (coordinates.size() > multipliers.size()) FailEmit("an emulated atomic's lock hashes at most three coordinates");
    std::uint32_t hash = ConstantU32(state, 0u);
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        hash = Binary(state, spv::OpIAdd, TypeU32(state), hash, Binary(state, spv::OpIMul, TypeU32(state), coordinates[index], ConstantU32(state, multipliers[index])));
    }
    hash = Binary(state, spv::OpBitwiseXor, TypeU32(state), hash, Binary(state, spv::OpShiftRightLogical, TypeU32(state), hash, ConstantU32(state, 16u)));
    const auto index = Binary(state, spv::OpBitwiseAnd, TypeU32(state), hash, ConstantU32(state, RuntimeAbi::AtomicLockCount - 1u));
    const auto pointer = state.module.AllocateId();
    state.module.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer, state.atomicLocksVariable, ConstantU32(state, 0u), index);
    return pointer;
}

}
