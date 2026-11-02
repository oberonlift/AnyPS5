#ifndef CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVMEMORY_SPIRVATOMICEMULATION_HPP
#define CORE_SHADER_RECOMPILIER_SPIRVBACKEND_INCLUDE_SPIRVBACKEND_SPIRVMEMORY_SPIRVATOMICEMULATION_HPP

#include "SpirvBackend/SpirvEmitterState.hpp"
#include <cstdint>
#include <functional>
#include <span>

namespace ShaderRecompiler {

struct SpirvLock {
    std::uint32_t pointer;
    std::uint32_t scope;
    std::uint32_t semantics;
    bool fences;
};

std::uint32_t AtomicNext(SpirvEmitterState& state, std::uint32_t type, std::uint32_t opcode, std::uint32_t old, std::uint32_t value, std::uint32_t comparator);
std::uint32_t EmitLockedUpdate(SpirvEmitterState& state, const SpirvLock& lock, std::uint32_t type, std::uint32_t zero, const std::function<std::uint32_t()>& update);
std::uint32_t EmitAtomicLockPointer(SpirvEmitterState& state, std::span<const std::uint32_t> coordinates);

}

#endif
