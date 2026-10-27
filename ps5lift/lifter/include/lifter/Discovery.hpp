#ifndef PS5LIFT_DISCOVERY_HPP
#define PS5LIFT_DISCOVERY_HPP

#include <lifter/GuestImage.hpp>

#include <Zydis/Zydis.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Ps5Lift {

struct Instruction {
    std::uint64_t Address;
    ZydisDecodedInstruction Info;
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> Operands;

    std::uint64_t Next() const;
};

struct GuestFunction {
    std::uint64_t Entry;
    std::string Name;
    std::optional<std::string> Import;
    std::map<std::uint64_t, Instruction> Instructions;
    std::set<std::uint64_t> Leaders;
    std::map<std::uint64_t, std::vector<std::uint64_t>> JumpTables;
};

class Decoder {
public:
    Decoder();
    Instruction Decode(const GuestImage& image, std::uint64_t address) const;

private:
    ZydisDecoder _decoder;
};

bool IsConditionalJump(ZydisMnemonic mnemonic);
std::optional<std::uint64_t> DirectTarget(const Instruction& instruction);
std::optional<std::uint64_t> ImportTarget(const GuestImage& image, const Instruction& instruction);
std::string BridgeSymbol(const std::string& import);

std::map<std::uint64_t, GuestFunction> DiscoverFunctions(const GuestImage& image);

}

#endif
