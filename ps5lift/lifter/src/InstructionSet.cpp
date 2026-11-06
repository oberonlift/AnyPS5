#include <lifter/InstructionSet.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

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

bool WaitsForX87(const ZydisDecodedInstruction& instruction) {
    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_FNINIT: case ZYDIS_MNEMONIC_FNCLEX: case ZYDIS_MNEMONIC_FNSTCW: case ZYDIS_MNEMONIC_FNSTSW: case ZYDIS_MNEMONIC_FNSTENV: case ZYDIS_MNEMONIC_FNSAVE:
    case ZYDIS_MNEMONIC_FDISI8087_NOP: case ZYDIS_MNEMONIC_FENI8087_NOP: case ZYDIS_MNEMONIC_FSETPM287_NOP:
        return false;
    case ZYDIS_MNEMONIC_FISTTP:
        return true;
    default:
        return instruction.meta.isa_ext == ZYDIS_ISA_EXT_X87;
    }
}

const ZydisDecodedOperand* MemoryOperand(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    for (std::uint8_t i = 0; i < instruction.operand_count_visible; ++i) {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY) return &operands[i];
    }
    return nullptr;
}

AlignmentRule RequiredAlignment(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands) {
    constexpr AlignmentRule unaligned{0, false};
    const auto* memory = MemoryOperand(instruction, operands);
    if (memory == nullptr) return unaligned;
    switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_CMPXCHG16B: case ZYDIS_MNEMONIC_FXSAVE: case ZYDIS_MNEMONIC_FXSAVE64: case ZYDIS_MNEMONIC_FXRSTOR: case ZYDIS_MNEMONIC_FXRSTOR64:
        return {16, false};
    case ZYDIS_MNEMONIC_XSAVE: case ZYDIS_MNEMONIC_XSAVE64: case ZYDIS_MNEMONIC_XSAVEOPT: case ZYDIS_MNEMONIC_XSAVEOPT64:
    case ZYDIS_MNEMONIC_XSAVEC: case ZYDIS_MNEMONIC_XSAVEC64: case ZYDIS_MNEMONIC_XRSTOR: case ZYDIS_MNEMONIC_XRSTOR64:
        return {64, false};
    case ZYDIS_MNEMONIC_CVTPD2PI: case ZYDIS_MNEMONIC_CVTTPD2PI:
        return {16, true};
    case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVUPD: case ZYDIS_MNEMONIC_MOVDQU: case ZYDIS_MNEMONIC_LDDQU:
    case ZYDIS_MNEMONIC_PCMPESTRI: case ZYDIS_MNEMONIC_PCMPESTRM: case ZYDIS_MNEMONIC_PCMPISTRI: case ZYDIS_MNEMONIC_PCMPISTRM:
        return unaligned;
    default:
        break;
    }
    const unsigned bytes = memory->size / 8;
    switch (instruction.meta.exception_class) {
    case ZYDIS_EXCEPTION_CLASS_SSE1: case ZYDIS_EXCEPTION_CLASS_AVX1:
        return {bytes, false};
    case ZYDIS_EXCEPTION_CLASS_SSE2: case ZYDIS_EXCEPTION_CLASS_SSE4:
        if (bytes != 16) throw std::logic_error(std::string(ZydisMnemonicGetString(instruction.mnemonic)) + " has a " + std::to_string(bytes) + "-byte memory operand in an SSE exception class whose alignment applies to 16-byte operands");
        return {16, true};
    default:
        return unaligned;
    }
}

}
