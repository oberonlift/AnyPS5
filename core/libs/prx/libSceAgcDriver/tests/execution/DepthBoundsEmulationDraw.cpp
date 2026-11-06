#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "Recompiler.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

namespace Harness = DepthFastClearHarness;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

struct Bounds {
    float min;
    float max;
};

struct Case {
    float stored;
    Bounds bounds;
    bool covered;
    const char* what;
};

// A draw over a depth gradient: column x of the target stores (2x + 1) / 128, so bounds and depths at multiples of
// 1/64 fall between columns.
struct GradientCase {
    Bounds bounds;
    std::optional<VkCompareOp> depthTest;
    float z;
    std::uint32_t firstColumn;
    std::uint32_t lastColumn;
    const char* what;
};

struct DrawSpec {
    float z;
    float rightZ;
    bool depthWrite;
    std::optional<VkCompareOp> depthTest;
    std::optional<Bounds> bounds;
    bool emulate;
};

struct GuestBuffers {
    std::span<std::byte> pixels = GuestTestArray<std::byte>(Harness::Pixels.size());
    std::span<std::array<float, 4>> vertices = GuestTestArray<std::array<float, 4>>(3);
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = GuestTestAddress(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

std::vector<bool> Run(AgcDriver::VulkanDevice& device, GuestBuffers& buffers, std::span<const float> depth, const DrawSpec& draw) {
    std::ranges::copy(std::array<std::array<float, 4>, 3>{{{-1.0f, -1.0f, draw.z, 1.0f}, {3.0f, -1.0f, draw.rightZ, 1.0f}, {-1.0f, 3.0f, draw.z, 1.0f}}}, buffers.vertices.begin());
    std::ranges::fill(buffers.pixels, std::byte{0});

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {GuestTestAddress(buffers.pixels.data()), {Harness::Width, Harness::Height}, VK_FORMAT_R8G8B8A8_UNORM, buffers.pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.depth = AgcDriver::Graphics::DepthTarget{GuestTestAddress(depth.data()), 0, {Harness::Width, Harness::Height}, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
    state.depthTest = draw.depthWrite || draw.depthTest.has_value();
    state.depthWrite = draw.depthWrite;
    state.depthCompare = draw.depthTest.value_or(VK_COMPARE_OP_ALWAYS);
    if (draw.bounds) {
        state.depthBoundsTest = true;
        state.minDepthBounds = draw.bounds->min;
        state.maxDepthBounds = draw.bounds->max;
    }
    state.renderExtent = {Harness::Width, Harness::Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Harness::Height), static_cast<float>(Harness::Width), -static_cast<float>(Harness::Height), 0.0f, 1.0f};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Harness::Width, Harness::Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = true;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    if (draw.bounds && draw.emulate) {
        auto context = device.graphicsContext();
        context.depthBounds = false;
        context.depthRangeUnrestricted = false;
        AgcDriver::Graphics::ApplyDepthBoundsEmulation(context, state, pixel, false);
        Require(!state.depthBoundsTest && !state.depthWrite && (pixel.depthBoundsSource.has_value() || state.depthTest), "the depth bounds test was not emulated");
    }

    auto target = device.Target();
    target.depthRangeRemap = false;
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(buffers.vertices.data(), 16u, static_cast<std::uint32_t>(buffers.vertices.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(Harness::VertexCode.data()), std::as_bytes(std::span(Harness::VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(Harness::VertexCode.data()), Harness::VertexCode, 0, {}},
        {32, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    const auto pixelPush = PixelPushOffset(vertexPush, target);
    std::vector<std::uint32_t> pixelUserData(4, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(Harness::PixelCode.data()), std::as_bytes(std::span(Harness::PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(Harness::PixelCode.data()), Harness::PixelCode, 0, {}},
        {32, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, pixelPush, AgcDriver::Graphics::StagePushLimit(state.stages, pixelPush, false, pixel.depthBoundsSource.has_value()) - pixelPush % AgcDriver::Graphics::PipelinePushSlotBytes}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    Require(pixelResult.readsDepthBoundsPush == pixel.depthBoundsSource.has_value(), "the pixel shader does not read the depth bounds exactly when it runs the test");
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, pixelPush}
    }};
    const AgcDriver::Pm4::DrawParameters parameters{0, static_cast<std::uint32_t>(buffers.vertices.size()), 0, 1, 0, false};
    device.Draw(state, parameters, shaders);
    device.WaitIdle();
    std::vector<bool> covered(buffers.pixels.size() / 4u);
    for (std::size_t index = 0; index < covered.size(); ++index) covered[index] = std::to_integer<std::uint8_t>(buffers.pixels[index * 4u]) == 255u;
    return covered;
}

std::uint32_t Count(const std::vector<bool>& covered) {
    return static_cast<std::uint32_t>(std::ranges::count(covered, true));
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = device->graphicsContext();
        GuestBuffers buffers;
        const float below = std::nextafter(0.25f, 0.0f);
        const float above = std::nextafter(0.25f, 1.0f);
        const std::array<Case, 12> cases{{
            {0.25f, {0.0f, 0.25f}, true, "stored 0.25, upper bound equal to it"},
            {0.25f, {0.0f, below}, false, "stored 0.25, upper bound one step below it"},
            {0.25f, {0.25f, 1.0f}, true, "stored 0.25, lower bound equal to it"},
            {0.25f, {above, 1.0f}, false, "stored 0.25, lower bound one step above it"},
            {0.000125f, {0.0f, 0.000125f}, true, "stored 0.000125 inside [0, 0.000125]"},
            {0.5f, {0.0f, 0.000125f}, false, "stored 0.5 outside [0, 0.000125]"},
            {0.5f, {0.000125f, 1.0f}, true, "stored 0.5 inside [0.000125, 1]"},
            {0.0f, {0.000125f, 1.0f}, false, "stored 0 outside [0.000125, 1]"},
            {0.0f, {0.0f, 1.0f}, true, "stored 0 inside [0, 1]"},
            {1.0f, {0.0f, 1.0f}, true, "stored 1 inside [0, 1]"},
            {1.0f, {0.0f, 0.5f}, false, "stored 1 outside [0, 0.5]"},
            {0.75f, {0.5f, 1.0f}, true, "stored 0.75 inside [0.5, 1]"},
        }};
        // In pixel-shader emulation: two-sided bounds, and bounds beside a depth test that also rejects columns.
        const std::array<GradientCase, 6> gradients{{
            {{0.25f, 0.8125f}, std::nullopt, 0.5f, 16u, 51u, "two-sided bounds [0.25, 0.8125] without a depth test"},
            {{0.25f, 0.8125f}, VK_COMPARE_OP_GREATER_OR_EQUAL, 0.59375f, 16u, 37u, "two-sided bounds [0.25, 0.8125] under a GREATER_OR_EQUAL depth test at 0.59375"},
            {{0.25f, 0.8125f}, VK_COMPARE_OP_LESS_OR_EQUAL, 0.59375f, 38u, 51u, "two-sided bounds [0.25, 0.8125] under a LESS_OR_EQUAL depth test at 0.59375"},
            {{0.0f, 0.5f}, VK_COMPARE_OP_LESS_OR_EQUAL, 0.25f, 16u, 31u, "the upper bound 0.5 under a LESS_OR_EQUAL depth test at 0.25"},
            {{0.5f, 1.0f}, VK_COMPARE_OP_GREATER_OR_EQUAL, 0.75f, 32u, 47u, "the lower bound 0.5 under a GREATER_OR_EQUAL depth test at 0.75"},
            {{0.125f, 0.25f}, VK_COMPARE_OP_GREATER_OR_EQUAL, 0.0f, 1u, 0u, "two-sided bounds [0.125, 0.25] under a depth test no column passes"},
        }};
        const auto depths = GuestTestArray<float>(static_cast<std::size_t>(Harness::Width) * Harness::Height * (cases.size() + gradients.size()));
        const auto depthTarget = [&](std::size_t index) { return depths.subspan(index * Harness::Width * Harness::Height, static_cast<std::size_t>(Harness::Width) * Harness::Height); };
        std::size_t compared = 0;
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto& test = cases[index];
            const auto depth = depthTarget(index);
            Require(Count(Run(*device, buffers, depth, {test.stored, test.stored, true, VK_COMPARE_OP_ALWAYS, std::nullopt, false})) == Harness::Covered, "the draw that stores the depth did not cover the target");
            const auto expected = test.covered ? Harness::Covered : 0u;
            for (const bool emulate : {true, false}) {
                if (!emulate && !context.depthBounds) continue;
                const auto passed = Count(Run(*device, buffers, depth, {0.5f, 0.5f, false, std::nullopt, test.bounds, emulate}));
                if (passed != expected) {
                    char detail[256];
                    std::snprintf(detail, sizeof(detail), "%s (%s): %u of %u pixels passed, expected %s", test.what, emulate ? "emulated" : "native", passed, Harness::Covered, test.covered ? "all" : "none");
                    throw std::runtime_error(detail);
                }
                if (!emulate) ++compared;
            }
        }
        for (std::size_t index = 0; index < gradients.size(); ++index) {
            const auto& test = gradients[index];
            const auto depth = depthTarget(cases.size() + index);
            Require(Count(Run(*device, buffers, depth, {0.0f, 2.0f, true, VK_COMPARE_OP_ALWAYS, std::nullopt, false})) == Harness::Covered, "the draw that stores the depth gradient did not cover the target");
            for (const bool emulate : {true, false}) {
                if (!emulate && !context.depthBounds) continue;
                const auto covered = Run(*device, buffers, depth, {test.z, test.z, false, test.depthTest, test.bounds, emulate});
                for (std::uint32_t y = 0; y < Harness::Height; ++y) {
                    for (std::uint32_t x = 0; x < Harness::Width; ++x) {
                        const bool expected = x >= test.firstColumn && x <= test.lastColumn;
                        if (covered[y * Harness::Width + x] == expected) continue;
                        char detail[256];
                        std::snprintf(detail, sizeof(detail), "%s (%s): pixel (%u, %u) %s, expected it %s", test.what, emulate ? "emulated" : "native", x, y, expected ? "failed" : "passed", expected ? "to pass" : "to fail");
                        throw std::runtime_error(detail);
                    }
                }
                if (!emulate) ++compared;
            }
        }
        std::printf("depth bounds emulation draw tests passed (%zu cases, %zu compared with the native test)\n", cases.size() + gradients.size(), compared);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
