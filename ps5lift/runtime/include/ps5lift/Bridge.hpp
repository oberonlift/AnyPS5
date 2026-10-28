#ifndef PS5LIFT_BRIDGE_HPP
#define PS5LIFT_BRIDGE_HPP

#include <ps5lift/State.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Ps5Lift::Bridge {

constexpr int IntegerRegisters[6] = {PS5LIFT_RDI, PS5LIFT_RSI, PS5LIFT_RDX, PS5LIFT_RCX, PS5LIFT_R8, PS5LIFT_R9};

template <typename T>
T FromBits(const std::uint64_t bits) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(bits);
    } else if constexpr (std::is_same_v<T, bool>) {
        return (bits & 0xff) != 0;
    } else if constexpr (std::is_floating_point_v<T>) {
        T value;
        std::memcpy(&value, &bits, sizeof(T));
        return value;
    } else {
        return static_cast<T>(bits);
    }
}

template <typename T>
T Integer(const Ps5LiftState* state, const int index) {
    return FromBits<T>(state->Gpr[IntegerRegisters[index]]);
}

template <typename T>
T Floating(const Ps5LiftState* state, const int index) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, state->Vector[index], sizeof(T));
    return FromBits<T>(bits);
}

template <typename T>
T Stack(const Ps5LiftState* state, const int slot) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, reinterpret_cast<const void*>(state->Gpr[PS5LIFT_RSP] + 8 + 8 * static_cast<std::uint64_t>(slot)), sizeof(bits));
    return FromBits<T>(bits);
}

template <typename T>
T Integer128(const Ps5LiftState* state, const int index) {
    const auto low = static_cast<unsigned __int128>(state->Gpr[IntegerRegisters[index]]);
    const auto high = static_cast<unsigned __int128>(state->Gpr[IntegerRegisters[index + 1]]);
    return static_cast<T>(low | high << 64);
}

template <typename T>
T Stack128(const Ps5LiftState* state, const int slot) {
    unsigned __int128 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(state->Gpr[PS5LIFT_RSP] + 8 + 8 * static_cast<std::uint64_t>(slot)), sizeof(value));
    return static_cast<T>(value);
}

template <typename T>
void Return(Ps5LiftState* state, const T value) {
    if constexpr (std::is_same_v<T, __int128> || std::is_same_v<T, unsigned __int128>) {
        const auto bits = static_cast<unsigned __int128>(value);
        state->Gpr[PS5LIFT_RAX] = static_cast<std::uint64_t>(bits);
        state->Gpr[PS5LIFT_RDX] = static_cast<std::uint64_t>(bits >> 64);
    } else if constexpr (std::is_floating_point_v<T>) {
        std::memset(state->Vector[0], 0, 16);
        std::memcpy(state->Vector[0], &value, sizeof(T));
    } else if constexpr (std::is_pointer_v<T>) {
        state->Gpr[PS5LIFT_RAX] = reinterpret_cast<std::uint64_t>(value);
    } else if constexpr (std::is_signed_v<T>) {
        state->Gpr[PS5LIFT_RAX] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
    } else {
        state->Gpr[PS5LIFT_RAX] = static_cast<std::uint64_t>(value);
    }
}

}

#endif
