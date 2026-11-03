#include "Recompiler.hpp"
#include "SpirvBackend/SpirvModule.hpp"
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include "SpirvBackend/SpirvOptimizer.hpp"
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <stdexcept>
#include <string>

namespace ShaderRecompiler {
namespace {

constexpr std::uint32_t PushWords = (MeshDrawPushOffsetBytes + MeshDrawPushBytes) / 4u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Mesh readback vertex SPIR-V: " + message);
}

class MeshReadbackEmitter {
public:
    explicit MeshReadbackEmitter(std::uint32_t version) : builder(version), listsAllGlobals(version >= 0x00010400u) {
        voidType = builder.Type(spv::OpTypeVoid);
        boolType = builder.Type(spv::OpTypeBool);
        uintType = builder.Type(spv::OpTypeInt, 32u, 0u);
        intType = builder.Type(spv::OpTypeInt, 32u, 1u);
        ulongType = builder.Type(spv::OpTypeInt, 64u, 0u);
        floatType = builder.Type(spv::OpTypeFloat, 32u);
        vec3FloatType = builder.Type(spv::OpTypeVector, floatType, 3u);
        vec4FloatType = builder.Type(spv::OpTypeVector, floatType, 4u);
        functionType = builder.Type(spv::OpTypeFunction, voidType);
    }

    std::vector<std::uint32_t> Emit(const std::vector<std::uint32_t>& exports, std::uint32_t layout, const EmulatedInterface* emulated) {
        const auto clipDistances = MeshRecordClipDistances(layout);
        const auto cullDistances = MeshRecordCullDistances(layout);
        const bool layer = MeshRecordLayer(layout);
        const auto size = MeshRecordPrimitiveSize(layout);
        require(emulated == nullptr || size == 3u, "emulated barycentrics need triangle primitives");
        const auto slot = [&](std::uint32_t location) {
            const auto found = std::find(exports.begin(), exports.end(), location);
            require(found != exports.end(), "fragment parameter " + std::to_string(location) + " is not exported by the mesh program");
            return static_cast<std::uint32_t>(found - exports.begin());
        };
        builder.EmitCapability(spv::CapabilityShader);
        if (clipDistances != 0u) builder.EmitCapability(spv::CapabilityClipDistance);
        if (cullDistances != 0u) builder.EmitCapability(spv::CapabilityCullDistance);
        if (layer) {
            builder.EmitCapability(spv::CapabilityShaderViewportIndexLayerEXT);
            builder.EmitExtension("SPV_EXT_shader_viewport_index_layer");
        }
        builder.EmitCapability(spv::CapabilityInt64);
        builder.EmitCapability(spv::CapabilityPhysicalStorageBufferAddresses);
        builder.EmitExtension("SPV_KHR_physical_storage_buffer");
        builder.AddMemoryModel(spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450);
        main = result(spv::OpFunction, voidType, spv::FunctionControlMaskNone, functionType);

        const auto wordArray = builder.Type(spv::OpTypeArray, uintType, constant(uintType, PushWords));
        builder.AddAnnotation(spv::OpDecorate, wordArray, spv::DecorationArrayStride, 4u);
        const auto pushBlock = builder.DecoratedType(spv::OpTypeStruct, {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 0u}}, {spv::OpDecorate, {spv::DecorationBlock}}}, wordArray);
        const auto push = addInterface(spv::StorageClassPushConstant, pushBlock);
        const auto vertexIndex = addInterface(spv::StorageClassInput, intType);
        builder.AddAnnotation(spv::OpDecorate, vertexIndex, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
        const auto position = addInterface(spv::StorageClassOutput, vec4FloatType);
        builder.AddAnnotation(spv::OpDecorate, position, spv::DecorationBuiltIn, spv::BuiltInPosition);
        const auto& passed = emulated != nullptr ? emulated->passed : exports;
        std::vector<std::uint32_t> outputs;
        for (const auto location : passed) {
            outputs.push_back(addInterface(spv::StorageClassOutput, vec4FloatType));
            builder.AddAnnotation(spv::OpDecorate, outputs.back(), spv::DecorationLocation, location);
        }
        const BarycentricEmulationLayout unemulated;
        const auto& emulation = emulated != nullptr ? emulated->layout : unemulated;
        const auto barycentricOutput = [&](std::uint32_t location) {
            if (location == BarycentricEmulationLayout::NoLocation) return 0u;
            const auto variable = addInterface(spv::StorageClassOutput, vec3FloatType);
            builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, location);
            return variable;
        };
        const auto smooth = barycentricOutput(emulation.smoothLocation);
        const auto linear = barycentricOutput(emulation.linearLocation);
        const auto reciprocalW = barycentricOutput(emulation.reciprocalWLocation);
        if (reciprocalW != 0u) builder.AddAnnotation(spv::OpDecorate, reciprocalW, spv::DecorationNoPerspective);
        const auto cornerArrayType = builder.Type(spv::OpTypeArray, vec4FloatType, constant(uintType, 3u));
        std::vector<std::uint32_t> perVertexOutputs;
        if (emulated != nullptr) {
            for (const auto& parameter : emulated->perVertex) {
                perVertexOutputs.push_back(addInterface(spv::StorageClassOutput, cornerArrayType));
                builder.AddAnnotation(spv::OpDecorate, perVertexOutputs.back(), spv::DecorationLocation, parameter.location);
            }
        }
        const auto distanceOutput = [&](std::uint32_t count, spv::BuiltIn builtin) {
            if (count == 0u) return 0u;
            const auto variable = addInterface(spv::StorageClassOutput, builder.Type(spv::OpTypeArray, floatType, constant(uintType, count)));
            builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
            return variable;
        };
        const auto clip = distanceOutput(clipDistances, spv::BuiltInClipDistance);
        const auto cull = distanceOutput(cullDistances, spv::BuiltInCullDistance);
        const auto layerOutput = layer ? addInterface(spv::StorageClassOutput, intType) : 0u;
        if (layer) builder.AddAnnotation(spv::OpDecorate, layerOutput, spv::DecorationBuiltIn, spv::BuiltInLayer);
        builder.EmitEntryPoint(spv::ExecutionModelVertex, main, "main", interfaces);
        builder.AddFunction(spv::OpLabel, builder.AllocateId());

        const auto pushWordPointer = builder.Type(spv::OpTypePointer, spv::StorageClassPushConstant, uintType);
        const auto pushWord = [&](std::uint32_t word) { return widen(load(uintType, result(spv::OpAccessChain, pushWordPointer, push, constant(intType, 0u), constant(intType, word)))); };
        const auto arguments = combine(pushWord(MeshDrawPushOffsetBytes / 4u + MeshArgumentAddressDword), pushWord(MeshDrawPushOffsetBytes / 4u + MeshArgumentAddressDword + 1u));
        const auto output = combine(widen(loadAt(uintType, offset(arguments, 4ull * MeshOutputAddressDword))), widen(loadAt(uintType, offset(arguments, 4ull * (MeshOutputAddressDword + 1u)))));
        const auto triangles = offset(output, MeshOutputHeaderBytes);
        const auto corners = widen(loadAt(uintType, output));
        const auto records = result(spv::OpIAdd, ulongType, triangles, result(spv::OpShiftLeftLogical, ulongType, corners, constant(uintType, 2u)));
        const auto corner = widen(result(spv::OpBitcast, uintType, load(intType, vertexIndex)));
        const auto parameterCount = static_cast<std::uint32_t>(exports.size());
        const auto stride = constant64(MeshRecordBytes(parameterCount, layout));
        const auto recordOf = [&](std::uint32_t cornerIndex) {
            const auto vertex = widen(loadAt(uintType, result(spv::OpIAdd, ulongType, triangles, result(spv::OpShiftLeftLogical, ulongType, cornerIndex, constant(uintType, 2u)))));
            return result(spv::OpIAdd, ulongType, records, result(spv::OpIMul, ulongType, vertex, stride));
        };
        const auto record = recordOf(corner);
        const auto clipPosition = loadAt(vec4FloatType, record);
        store(position, clipPosition);
        for (std::size_t i = 0; i < outputs.size(); ++i) store(outputs[i], loadAt(vec4FloatType, offset(record, 16ull * (1u + slot(passed[i])))));
        if (emulated != nullptr) {
            const auto inPrimitive = result(spv::OpUMod, ulongType, corner, constant64(size));
            const auto first = result(spv::OpISub, ulongType, corner, inPrimitive);
            const auto zero = constant(floatType, 0u);
            const auto weights = [&](std::uint32_t value) {
                std::array<std::uint32_t, 3> components{};
                for (std::uint32_t k = 0; k < 3u; ++k) components[k] = result(spv::OpSelect, floatType, result(spv::OpIEqual, boolType, inPrimitive, constant64(k)), value, zero);
                return result(spv::OpCompositeConstruct, vec3FloatType, components[0], components[1], components[2]);
            };
            const auto one = constant(floatType, std::bit_cast<std::uint32_t>(1.0f));
            if (smooth != 0u || linear != 0u) {
                const auto unit = weights(one);
                if (smooth != 0u) store(smooth, unit);
                if (linear != 0u) store(linear, unit);
            }
            if (reciprocalW != 0u) store(reciprocalW, weights(result(spv::OpFDiv, floatType, one, result(spv::OpCompositeExtract, floatType, clipPosition, 3u))));
            if (!perVertexOutputs.empty()) {
                std::array<std::uint32_t, 3> cornerRecords{};
                for (std::uint32_t k = 0; k < 3u; ++k) cornerRecords[k] = recordOf(result(spv::OpIAdd, ulongType, first, constant64(k)));
                const auto outputVec4Pointer = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, vec4FloatType);
                for (std::size_t i = 0; i < perVertexOutputs.size(); ++i) {
                    const auto source = 16ull * (1u + slot(emulated->perVertex[i].sourceLocation));
                    for (std::uint32_t k = 0; k < 3u; ++k) store(result(spv::OpAccessChain, outputVec4Pointer, perVertexOutputs[i], constant(intType, k)), loadAt(vec4FloatType, offset(cornerRecords[k], source)));
                }
            }
        }
        const auto extras = MeshRecordExtraOffset(parameterCount);
        const auto outputPointer = builder.Type(spv::OpTypePointer, spv::StorageClassOutput, floatType);
        for (std::uint32_t i = 0; i < clipDistances; ++i) store(result(spv::OpAccessChain, outputPointer, clip, constant(intType, i)), loadAt(floatType, offset(record, extras + 4ull * i)));
        for (std::uint32_t i = 0; i < cullDistances; ++i) store(result(spv::OpAccessChain, outputPointer, cull, constant(intType, i)), loadAt(floatType, offset(record, extras + 4ull * (clipDistances + i))));
        if (layer) {
            const auto primitive = result(spv::OpUDiv, ulongType, corner, constant64(size));
            const auto provokingCorner = result(spv::OpIAdd, ulongType, result(spv::OpIMul, ulongType, primitive, constant64(size)), constant64(std::min(MeshRecordProvokingVertex(layout), size - 1u)));
            const auto provokingRecord = recordOf(provokingCorner);
            store(layerOutput, result(spv::OpBitcast, intType, loadAt(uintType, offset(provokingRecord, extras + 4ull * (clipDistances + cullDistances)))));
        }
        builder.AddFunction(spv::OpReturn);
        builder.AddFunction(spv::OpFunctionEnd);
        return builder.Finalize();
    }

private:
    SpirvModule builder;
    bool listsAllGlobals;
    std::vector<std::uint32_t> interfaces;
    std::uint32_t main = 0;
    std::uint32_t voidType = 0;
    std::uint32_t boolType = 0;
    std::uint32_t uintType = 0;
    std::uint32_t intType = 0;
    std::uint32_t ulongType = 0;
    std::uint32_t floatType = 0;
    std::uint32_t vec3FloatType = 0;
    std::uint32_t vec4FloatType = 0;
    std::uint32_t functionType = 0;

    std::uint32_t constant(std::uint32_t type, std::uint32_t value) { return builder.Constant(spv::OpConstant, type, value); }

    std::uint32_t constant64(std::uint64_t value) { return builder.Constant(spv::OpConstant, ulongType, static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32u)); }

    template <typename... TArgs>
    std::uint32_t result(spv::Op opcode, std::uint32_t type, TArgs... operands) {
        const auto id = builder.AllocateId();
        builder.AddFunction(opcode, type, id, operands...);
        return id;
    }

    std::uint32_t load(std::uint32_t type, std::uint32_t pointer) { return result(spv::OpLoad, type, pointer); }

    std::uint32_t loadAt(std::uint32_t type, std::uint32_t address) {
        const auto pointer = result(spv::OpConvertUToPtr, builder.Type(spv::OpTypePointer, spv::StorageClassPhysicalStorageBuffer, type), address);
        return result(spv::OpLoad, type, pointer, spv::MemoryAccessAlignedMask, 4u);
    }

    std::uint32_t widen(std::uint32_t value) { return result(spv::OpUConvert, ulongType, value); }

    std::uint32_t combine(std::uint32_t low, std::uint32_t high) { return result(spv::OpBitwiseOr, ulongType, low, result(spv::OpShiftLeftLogical, ulongType, high, constant(uintType, 32u))); }

    std::uint32_t offset(std::uint32_t address, std::uint64_t bytes) { return result(spv::OpIAdd, ulongType, address, constant64(bytes)); }

    void store(std::uint32_t pointer, std::uint32_t value) { builder.AddFunction(spv::OpStore, pointer, value); }

    std::uint32_t addInterface(spv::StorageClass storage, std::uint32_t type) {
        const auto variable = builder.DefineGlobalVariable(builder.Type(spv::OpTypePointer, storage, type), storage);
        if (listsAllGlobals || storage != spv::StorageClassPushConstant) interfaces.push_back(variable);
        return variable;
    }
};

}

RecompileResult BuildMeshReadbackVertexShader(const RecompileResult& mesh, const RecompileResult& fragment, std::uint32_t vulkanVersion) {
    const auto& words = mesh.spirv.Words();
    require(words.size() >= 5u && words[1] >= 0x00010300u && words[1] <= 0x00010500u, "unsupported SPIR-V version of the mesh program");
    std::optional<EmulatedInterface> emulated;
    if (fragment.barycentricEmulation.active) emulated = DescribeEmulatedInterface(fragment.fragmentParameters, fragment.barycentricEmulation);
    MeshReadbackEmitter emitter(words[1]);
    RecompileResult result;
    result.variantId = mesh.variantId;
    result.runtimeAbiVersion = mesh.runtimeAbiVersion;
    result.spirv = emitter.Emit(mesh.parameterExports, mesh.meshRecordLayout, emulated ? &*emulated : nullptr);
#if ANYPS5_ENABLE_SPIRV_TOOLS
    result.spirv = ValidateAndOptimizeSpirv(result.spirv, vulkanVersion, words[1]);
#else
    static_cast<void>(vulkanVersion);
#endif
    return result;
}

}
