#include <lifter/InstructionSet.hpp>

#include <cstdint>

namespace Ps5Lift {

namespace {

constexpr std::uint8_t HintPrefetchOpcode = 0x0D;
constexpr std::uint8_t FirstHintOpcode = 0x18;
constexpr std::uint8_t LastHintOpcode = 0x1F;

bool InHintSpace(const ZydisDecodedInstruction& instruction) {
    return instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY && instruction.opcode_map == ZYDIS_OPCODE_MAP_0F &&
           (instruction.opcode == HintPrefetchOpcode || (instruction.opcode >= FirstHintOpcode && instruction.opcode <= LastHintOpcode));
}

bool OnPs5Cpu(const ZydisISAExt extension) {
    switch (extension) {
    case ZYDIS_ISA_EXT_BASE: case ZYDIS_ISA_EXT_X87: case ZYDIS_ISA_EXT_MMX: case ZYDIS_ISA_EXT_SSE: case ZYDIS_ISA_EXT_SSE2: case ZYDIS_ISA_EXT_SSE3:
    case ZYDIS_ISA_EXT_SSSE3: case ZYDIS_ISA_EXT_SSE4: case ZYDIS_ISA_EXT_SSE4A: case ZYDIS_ISA_EXT_AVX: case ZYDIS_ISA_EXT_AVX2: case ZYDIS_ISA_EXT_AVX2GATHER:
    case ZYDIS_ISA_EXT_AVXAES: case ZYDIS_ISA_EXT_FMA: case ZYDIS_ISA_EXT_F16C: case ZYDIS_ISA_EXT_AES: case ZYDIS_ISA_EXT_PCLMULQDQ: case ZYDIS_ISA_EXT_SHA:
    case ZYDIS_ISA_EXT_BMI1: case ZYDIS_ISA_EXT_BMI2: case ZYDIS_ISA_EXT_LZCNT: case ZYDIS_ISA_EXT_ADOX_ADCX: case ZYDIS_ISA_EXT_MOVBE: case ZYDIS_ISA_EXT_RDRAND:
    case ZYDIS_ISA_EXT_RDSEED: case ZYDIS_ISA_EXT_CLFSH: case ZYDIS_ISA_EXT_CLFLUSHOPT: case ZYDIS_ISA_EXT_CLWB: case ZYDIS_ISA_EXT_XSAVE: case ZYDIS_ISA_EXT_XSAVEOPT:
    case ZYDIS_ISA_EXT_XSAVEC: case ZYDIS_ISA_EXT_XSAVES: case ZYDIS_ISA_EXT_RDPID: case ZYDIS_ISA_EXT_RDTSCP: case ZYDIS_ISA_EXT_PAUSE: case ZYDIS_ISA_EXT_LONGMODE:
    case ZYDIS_ISA_EXT_AMD3DNOW_PREFETCH: case ZYDIS_ISA_EXT_MONITORX: case ZYDIS_ISA_EXT_CLZERO:
        return true;
    default:
        return false;
    }
}

bool NamesSegmentRegister(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    for (std::uint8_t i = 0; i < instruction.operand_count_visible; ++i) {
        const auto& operand = operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_SEGMENT) return true;
    }
    return false;
}

}

InstructionGroup ClassifyInstruction(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    if (InHintSpace(instruction)) return InstructionGroup::Set;
    if (!OnPs5Cpu(instruction.meta.isa_ext)) return InstructionGroup::NotOnPs5;
    if ((instruction.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) != 0) return InstructionGroup::Privileged;
    if (instruction.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR || NamesSegmentRegister(instruction, operands)) return InstructionGroup::SegmentTables;
    return InstructionGroup::Set;
}

}
