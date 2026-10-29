#ifndef PS5LIFT_LLVM_COMPATIBILITY_HPP
#define PS5LIFT_LLVM_COMPATIBILITY_HPP

#include <llvm/ADT/StringMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <string>

namespace Ps5Lift {

// LLVM 21 takes a target triple as an llvm::Triple where earlier versions take its text.
#if LLVM_VERSION_MAJOR >= 21
inline const llvm::Triple& TripleArgument(const llvm::Triple& triple) {
    return triple;
}
#else
inline std::string TripleArgument(const llvm::Triple& triple) {
    return triple.str();
}
#endif

inline llvm::StringMap<bool> HostCpuFeatures() {
#if LLVM_VERSION_MAJOR >= 19
    return llvm::sys::getHostCPUFeatures();
#else
    llvm::StringMap<bool> features;
    llvm::sys::getHostCPUFeatures(features);
    return features;
#endif
}

}

#endif
