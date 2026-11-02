#ifndef CORE_LIBS_PRX_LIBSCEAGC_SHADER_INCLUDE_CREATESHADER_HPP
#define CORE_LIBS_PRX_LIBSCEAGC_SHADER_INCLUDE_CREATESHADER_HPP

#include "SceTypes.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "SceShaders.hpp"

extern "C" {

int APS5_VABI sceAgcCreateShader(GuestArena::GuestPointer<Shader>* dst, void* header, const volatile void* code);

}

#endif
