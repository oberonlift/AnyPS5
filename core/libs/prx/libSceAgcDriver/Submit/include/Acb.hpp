#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_SUBMIT_INCLUDE_ACB_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_SUBMIT_INCLUDE_ACB_HPP

#include "SceTypes.hpp"

extern "C" int APS5_VABI sceAgcDriverSubmitAcb(std::uint32_t queue, const Packet* packet);
extern "C" int APS5_VABI sceAgcDriverSubmitMultiAcbs(std::uint32_t queue, const GuestArena::GuestPointer<std::uint32_t>* acbs, const std::uint32_t* sizesInDwords, std::uint32_t count);

#endif
