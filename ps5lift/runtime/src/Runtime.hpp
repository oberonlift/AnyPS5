#ifndef PS5LIFT_RUNTIME_HPP
#define PS5LIFT_RUNTIME_HPP

#include <ps5lift/State.h>

#include <dlfcn.h>

#include <cstdint>

namespace Ps5Lift {

// A function of AnyPS5's libraries, when they are in the process, or null. The dynamic loader looks it up, so the
// runtime names no AnyPS5 library at link time, which a weak reference needs on macOS.
template <typename TFunction>
TFunction* AnyPs5Function(const char* name) {
    return reinterpret_cast<TFunction*>(dlsym(RTLD_DEFAULT, name));
}

// The lifted program's tables, as its executable handed them over.
const Ps5LiftProgram& Program();

// The lifted function that starts at a guest address, or null.
Ps5LiftFunction LookupFunction(std::uint64_t address);

// Lets native code call a guest function through a plain function pointer, as it does with one it reads from guest
// memory.
void InstallNativeCalls();

}

#endif
