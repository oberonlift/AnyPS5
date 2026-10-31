#ifndef PS5LIFT_RUNTIME_HPP
#define PS5LIFT_RUNTIME_HPP

#include <ps5lift/State.h>

#include <cstdint>

namespace Ps5Lift {

// The lifted program's tables, as its executable handed them over.
const Ps5LiftProgram& Program();

// The lifted function that starts at a guest address, or null.
Ps5LiftFunction LookupFunction(std::uint64_t address);

// Lets native code call a guest function through a plain function pointer, as it does with one it reads from guest
// memory.
void InstallNativeCalls();

}

#endif
