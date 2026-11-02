#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP

#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/include/Api.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>

constexpr int VulkanTestSkipped = 77;

inline bool TargetHasCapability(const ShaderRecompiler::SpirvTarget& target, spv::Capability capability) {
    const auto& capabilities = target.supportedCapabilities;
    return std::find(capabilities.begin(), capabilities.end(), static_cast<std::uint32_t>(capability)) != capabilities.end();
}

inline std::unique_ptr<AgcDriver::VulkanDevice> OpenVulkanTestDevice() {
    try {
        return std::make_unique<AgcDriver::VulkanDevice>();
    } catch (const AgcDriver::VulkanUnavailable& error) {
        if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
        std::printf("skipped, no usable Vulkan device: %s\n", error.what());
        return nullptr;
    }
}

template <typename T>
std::span<T> GuestTestArray(std::size_t count) {
    void* mapped = mmap_nid_postfix(nullptr, count * sizeof(T), 3, 0x1002, -1, 0);
    if (mapped == reinterpret_cast<void*>(~std::uintptr_t{0})) throw std::runtime_error("guest mmap of a test array failed");
    return {static_cast<T*>(mapped), count};
}

inline std::uint64_t GuestTestAddress(const void* pointer) {
    return GuestArena::HostToGuest(pointer);
}

inline std::uint32_t PixelPushOffset(std::uint32_t vertexPush, const ShaderRecompiler::SpirvTarget& target) {
    return AgcDriver::Graphics::StagePushOffset(vertexPush, ShaderRecompiler::ShaderStage::Fragment, target.fixedPushSlots);
}

inline ShaderRecompiler::BindingLayout PixelPushLayout(std::uint32_t vertexPush, const ShaderRecompiler::SpirvTarget& target) {
    const auto offset = PixelPushOffset(vertexPush, target);
    return {0, 0, offset, AgcDriver::Graphics::PipelinePushSlotBytes - offset % AgcDriver::Graphics::PipelinePushSlotBytes};
}

#endif
