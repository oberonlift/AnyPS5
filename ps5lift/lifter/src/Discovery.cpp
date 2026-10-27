#include <lifter/Discovery.hpp>

#include <deque>
#include <sstream>
#include <stdexcept>

namespace Ps5Lift {

namespace {

std::string Hex(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

bool EndsFlow(const Instruction& instruction) {
    switch (instruction.Info.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_JMP:
    case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_INT3:
        return true;
    default:
        return false;
    }
}

std::string ResolveImport(const GuestImage& image, const Decoder& decoder, const std::uint64_t entry) {
    const auto instruction = decoder.Decode(image, entry);
    const auto& operand = instruction.Operands[0];
    if (instruction.Info.mnemonic != ZYDIS_MNEMONIC_JMP || operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.base != ZYDIS_REGISTER_RIP || operand.mem.index != ZYDIS_REGISTER_NONE)
        throw std::runtime_error("PLT entry at 0x" + Hex(entry) + " is not jmp [rip+slot]");
    const auto slot = instruction.Next() + static_cast<std::uint64_t>(operand.mem.disp.value);
    const auto found = image.ImportSlots().find(slot);
    if (found == image.ImportSlots().end()) throw std::runtime_error("PLT entry at 0x" + Hex(entry) + " uses slot 0x" + Hex(slot) + " without a relocation");
    return found->second;
}

const Instruction* Previous(const GuestFunction& function, const std::uint64_t address) {
    auto found = function.Instructions.find(address);
    if (found == function.Instructions.begin() || found == function.Instructions.end()) return nullptr;
    --found;
    return found->second.Next() == address ? &found->second : nullptr;
}

bool WritesRegister(const Instruction& instruction, const ZydisRegister reg) {
    const auto& operand = instruction.Operands[0];
    return instruction.Info.operand_count_visible > 0 && operand.type == ZYDIS_OPERAND_TYPE_REGISTER && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value) == ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

std::optional<std::uint64_t> TableBound(const GuestFunction& function, const std::uint64_t jump, const ZydisRegister index) {
    const auto* instruction = Previous(function, jump);
    for (int depth = 0; instruction != nullptr && depth < 12; ++depth, instruction = Previous(function, instruction->Address)) {
        const auto mnemonic = instruction->Info.mnemonic;
        if (mnemonic != ZYDIS_MNEMONIC_CMP && mnemonic != ZYDIS_MNEMONIC_SUB) continue;
        const auto& left = instruction->Operands[0];
        const auto& right = instruction->Operands[1];
        if (left.type != ZYDIS_OPERAND_TYPE_REGISTER || right.type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return std::nullopt;
        if (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, left.reg.value) != ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, index)) return std::nullopt;
        const auto* branch = function.Instructions.contains(instruction->Next()) ? &function.Instructions.at(instruction->Next()) : nullptr;
        if (branch == nullptr) return std::nullopt;
        if (branch->Info.mnemonic == ZYDIS_MNEMONIC_JNBE) return right.imm.value.u + 1;
        if (branch->Info.mnemonic == ZYDIS_MNEMONIC_JNB) return right.imm.value.u;
        return std::nullopt;
    }
    return std::nullopt;
}

struct TableShape {
    std::uint64_t Address;
    unsigned EntrySize;
    ZydisRegister Index;
    std::uint64_t BoundFrom;
};

std::optional<TableShape> MatchTable(const GuestFunction& function, const Instruction& jump) {
    const auto& target = jump.Operands[0];
    const auto absolute = [](const ZydisDecodedOperand& operand) {
        return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index != ZYDIS_REGISTER_NONE && operand.mem.scale == 8;
    };
    if (absolute(target)) return TableShape{static_cast<std::uint64_t>(target.mem.disp.value), 8, target.mem.index, jump.Address};
    if (target.type != ZYDIS_OPERAND_TYPE_REGISTER) return std::nullopt;
    const auto* previous = Previous(function, jump.Address);
    if (previous == nullptr || !WritesRegister(*previous, target.reg.value)) return std::nullopt;
    if (previous->Info.mnemonic == ZYDIS_MNEMONIC_MOV && absolute(previous->Operands[1]))
        return TableShape{static_cast<std::uint64_t>(previous->Operands[1].mem.disp.value), 8, previous->Operands[1].mem.index, previous->Address};
    if (previous->Info.mnemonic != ZYDIS_MNEMONIC_ADD || previous->Operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER) return std::nullopt;
    const auto base = previous->Operands[1].reg.value;
    const auto* load = Previous(function, previous->Address);
    if (load == nullptr || load->Info.mnemonic != ZYDIS_MNEMONIC_MOVSXD || !WritesRegister(*load, target.reg.value)) return std::nullopt;
    const auto& entry = load->Operands[1];
    if (entry.type != ZYDIS_OPERAND_TYPE_MEMORY || entry.mem.base != base || entry.mem.scale != 4 || entry.mem.disp.value != 0) return std::nullopt;
    for (const auto* instruction = Previous(function, load->Address); instruction != nullptr; instruction = Previous(function, instruction->Address)) {
        if (!WritesRegister(*instruction, base)) continue;
        const auto& source = instruction->Operands[1];
        if (instruction->Info.mnemonic == ZYDIS_MNEMONIC_LEA && source.mem.base == ZYDIS_REGISTER_RIP && source.mem.index == ZYDIS_REGISTER_NONE)
            return TableShape{instruction->Next() + static_cast<std::uint64_t>(source.mem.disp.value), 4, entry.mem.index, load->Address};
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::uint64_t> RecoverJumpTable(const GuestImage& image, const GuestFunction& function, const Instruction& jump, const std::uint64_t limit) {
    constexpr std::uint64_t MaximumEntries = 4096;
    const auto shape = MatchTable(function, jump);
    if (!shape) return {};
    const auto bound = TableBound(function, shape->BoundFrom, shape->Index);
    if (bound && *bound > MaximumEntries) return {};
    std::vector<std::uint64_t> targets;
    for (std::uint64_t i = 0; i < bound.value_or(MaximumEntries); ++i) {
        const auto raw = image.ReadData(shape->Address + shape->EntrySize * i, shape->EntrySize);
        if (!raw) break;
        const auto destination = shape->EntrySize == 8 ? *raw : shape->Address + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(*raw)));
        const bool inside = destination >= function.Entry && destination < limit && image.IsExecutable(destination);
        if (!inside) {
            if (bound) return {};
            break;
        }
        targets.push_back(destination);
    }
    return targets;
}

GuestFunction DiscoverFunction(const GuestImage& image, const Decoder& decoder, const std::uint64_t entry, const std::set<std::uint64_t>& starts, std::set<std::uint64_t>& callTargets) {
    GuestFunction function;
    function.Entry = entry;
    const auto symbol = image.FunctionSymbols().find(entry);
    function.Name = symbol != image.FunctionSymbols().end() ? symbol->second : "sub_" + Hex(entry);
    if (image.IsPlt(entry)) {
        function.Import = ResolveImport(image, decoder, entry);
        function.Name = *function.Import;
        return function;
    }

    function.Leaders.insert(entry);
    std::deque<std::uint64_t> pending{entry};
    while (!pending.empty()) {
        auto address = pending.front();
        pending.pop_front();
        while (!function.Instructions.contains(address)) {
            const auto instruction = decoder.Decode(image, address);
            function.Instructions.emplace(address, instruction);
            const auto target = DirectTarget(instruction);
            const auto mnemonic = instruction.Info.mnemonic;
            if (mnemonic == ZYDIS_MNEMONIC_CALL && target) callTargets.insert(*target);
            if ((mnemonic == ZYDIS_MNEMONIC_JMP || IsConditionalJump(mnemonic)) && target && !(starts.contains(*target) && *target != entry)) {
                function.Leaders.insert(*target);
                pending.push_back(*target);
            }
            if (IsConditionalJump(mnemonic)) function.Leaders.insert(instruction.Next());
            if (mnemonic == ZYDIS_MNEMONIC_JMP && !target) {
                const auto next = starts.upper_bound(entry);
                auto targets = RecoverJumpTable(image, function, instruction, next == starts.end() ? ~std::uint64_t{0} : *next);
                for (const auto destination : targets) {
                    if (starts.contains(destination) && destination != entry) continue;
                    function.Leaders.insert(destination);
                    pending.push_back(destination);
                }
                if (!targets.empty()) function.JumpTables.emplace(address, std::move(targets));
            }
            if (EndsFlow(instruction)) break;
            address = instruction.Next();
            if (starts.contains(address) && address != entry) break;
        }
    }
    return function;
}

}

std::uint64_t Instruction::Next() const {
    return Address + Info.length;
}

Decoder::Decoder() {
    if (ZYAN_FAILED(ZydisDecoderInit(&_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) throw std::runtime_error("Zydis decoder initialization failed");
}

Instruction Decoder::Decode(const GuestImage& image, const std::uint64_t address) const {
    const auto code = image.CodeAt(address);
    if (code.empty()) throw std::runtime_error("Control flow reaches 0x" + Hex(address) + ", outside executable segments");
    Instruction instruction{};
    instruction.Address = address;
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&_decoder, code.data(), code.size(), &instruction.Info, instruction.Operands.data())))
        throw std::runtime_error("Cannot decode the instruction at 0x" + Hex(address));
    return instruction;
}

bool IsConditionalJump(const ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_JZ:
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_JECXZ:
        return true;
    default:
        return false;
    }
}

std::optional<std::uint64_t> DirectTarget(const Instruction& instruction) {
    const auto& operand = instruction.Operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative) return std::nullopt;
    return instruction.Next() + operand.imm.value.u;
}

std::map<std::uint64_t, GuestFunction> DiscoverFunctions(const GuestImage& image) {
    const Decoder decoder;
    std::set<std::uint64_t> starts{image.Entry()};
    for (const auto& [address, name] : image.FunctionSymbols()) {
        if (image.IsExecutable(address)) starts.insert(address);
    }
    for (const auto address : image.ExceptionFunctionStarts()) starts.insert(address);

    while (true) {
        std::map<std::uint64_t, GuestFunction> functions;
        std::set<std::uint64_t> callTargets;
        for (const auto entry : starts) functions.emplace(entry, DiscoverFunction(image, decoder, entry, starts, callTargets));
        const auto before = starts.size();
        starts.insert(callTargets.begin(), callTargets.end());
        if (starts.size() == before) return functions;
    }
}

}
