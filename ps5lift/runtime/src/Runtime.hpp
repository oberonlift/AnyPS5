#ifndef PS5LIFT_RUNTIME_HPP
#define PS5LIFT_RUNTIME_HPP

#include <ps5lift/State.h>

#include <cstdint>

namespace Ps5Lift {

const Ps5LiftProgram& Program();

Ps5LiftFunction LookupFunction(std::uint64_t address);

Ps5LiftState* BoundState();

void InstallNativeCalls();

std::uint16_t X87StatusWithSummary(std::uint16_t status, std::uint16_t control);

void LoadX87Control(Ps5LiftState& state, std::uint16_t control);

void RequireNoPendingX87Exceptions(const Ps5LiftState& state);

class SoftFloatEnvironment {
public:
    SoftFloatEnvironment();
    ~SoftFloatEnvironment();
    SoftFloatEnvironment(const SoftFloatEnvironment&) = delete;
    SoftFloatEnvironment& operator=(const SoftFloatEnvironment&) = delete;

private:
    std::uint_fast8_t _rounding;
    std::uint_fast8_t _tininess;
    std::uint_fast8_t _flags;
    std::uint_fast8_t _extendedPrecision;
};

}

#endif
