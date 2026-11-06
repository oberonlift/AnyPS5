#ifndef PS5LIFT_INSTRUCTIONSET_HPP
#define PS5LIFT_INSTRUCTIONSET_HPP

#include <Zydis/Zydis.h>

namespace Ps5Lift {

enum class InstructionGroup {
    Set,
    NotOnPs5,
    Privileged,
    SegmentTables
};

InstructionGroup ClassifyInstruction(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands);

}

#endif
