#ifndef PS5LIFT_DISCOVERY_HPP
#define PS5LIFT_DISCOVERY_HPP

#include <lifter/GuestImage.hpp>

#include <Zydis/Zydis.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ps5Lift {

struct Instruction {
    std::uint64_t Address;
    ZydisDecodedInstruction Info;
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> Operands;

    std::uint64_t Next() const;
};

// Where one of a function's instructions lies; its bytes stay in the image.
struct InstructionSpan {
    std::uint64_t Address;
    std::uint8_t Length;

    std::uint64_t Next() const;
};

// What discovery learns about a function's control flow.
struct FunctionFlow {
    std::uint64_t Entry = 0;
    std::set<std::uint64_t> Leaders;
    std::map<std::uint64_t, std::vector<std::uint64_t>> JumpTables;
    std::set<std::uint64_t> UnguardedJumpTables;
    std::set<std::uint64_t> ClosedJumpTables;
    std::set<std::uint64_t> ResumePoints;
};

// A discovered function as every function is kept: its instructions only by where they lie, since a decoded instruction
// takes some 1.1 KiB and a large title has tens of millions of them.
struct GuestFunction : FunctionFlow {
    std::string Name;
    std::optional<std::string> Import;
    std::vector<InstructionSpan> Code;
};

// One function with its instructions decoded, while discovery or the lifter works on it.
struct DecodedFunction : FunctionFlow {
    std::map<std::uint64_t, Instruction> Instructions;
};

class Decoder {
public:
    Decoder();
    Instruction Decode(const GuestImage& image, std::uint64_t address) const;
    std::optional<Instruction> TryDecode(std::span<const std::uint8_t> code, std::uint64_t address) const;

private:
    ZydisDecoder _decoder;
};

bool IsConditionalJump(ZydisMnemonic mnemonic);
std::optional<std::uint64_t> DirectTarget(const Instruction& instruction);
std::optional<std::uint64_t> ImportTarget(const GuestImage& image, const Instruction& instruction);
std::string BridgeSymbol(const std::string& import);
std::string Nid(std::string_view name);
// setjmp, longjmp and their variants save and restore the guest's registers, which only the runtime can do for lifted
// code, so a call to one goes to the runtime even when a guest module exports it, as the relinker names a module's
// export `<NID>#guest`. Returns the name the runtime knows it by.
std::optional<std::string> JumpBufferFunction(const std::string& import);
bool ReturnsTwice(const std::string& import);

std::map<std::uint64_t, GuestFunction> DiscoverFunctions(const GuestImage& image);
DecodedFunction Decode(const GuestImage& image, const GuestFunction& function);

}

#endif
