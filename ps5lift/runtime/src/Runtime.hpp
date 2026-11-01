#ifndef PS5LIFT_RUNTIME_HPP
#define PS5LIFT_RUNTIME_HPP

#include <ps5lift/State.h>

#include <dlfcn.h>

#include <cstdint>

namespace Ps5Lift {

template <typename TFunction>
TFunction* AnyPs5Function(const char* name) {
    return reinterpret_cast<TFunction*>(dlsym(RTLD_DEFAULT, name));
}

const Ps5LiftProgram& Program();

Ps5LiftFunction LookupFunction(std::uint64_t address);

Ps5LiftState* BoundState();

void InstallNativeCalls();

void RequireNoPendingX87Exceptions(const Ps5LiftState& state);

}

#endif
