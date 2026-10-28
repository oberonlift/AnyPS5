#include <lifter/Discovery.hpp>

#include <llvm/Support/Base64.h>
#include <llvm/Support/SHA1.h>

#include <algorithm>
#include <array>
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

using Predecessors = std::map<std::uint64_t, std::set<std::uint64_t>>;

Predecessors PredecessorsOf(const GuestFunction& function) {
    Predecessors predecessors;
    for (const auto& [address, instruction] : function.Instructions) {
        const auto mnemonic = instruction.Info.mnemonic;
        if (!EndsFlow(instruction)) predecessors[instruction.Next()].insert(address);
        if (const auto target = DirectTarget(instruction); target && (mnemonic == ZYDIS_MNEMONIC_JMP || IsConditionalJump(mnemonic))) predecessors[*target].insert(address);
    }
    for (const auto& [jump, targets] : function.JumpTables) {
        for (const auto target : targets) predecessors[target].insert(jump);
    }
    return predecessors;
}

const Instruction* UniquePredecessor(const GuestFunction& function, const Predecessors& predecessors, const std::uint64_t address) {
    const auto found = predecessors.find(address);
    if (address == function.Entry || found == predecessors.end() || found->second.size() != 1) return nullptr;
    return &function.Instructions.at(*found->second.begin());
}

ZydisRegister Enclosing(const ZydisRegister reg) {
    return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
}

bool ClobberedByCall(const ZydisRegister reg) {
    switch (Enclosing(reg)) {
    case ZYDIS_REGISTER_RBX:
    case ZYDIS_REGISTER_RBP:
    case ZYDIS_REGISTER_RSP:
    case ZYDIS_REGISTER_R12:
    case ZYDIS_REGISTER_R13:
    case ZYDIS_REGISTER_R14:
    case ZYDIS_REGISTER_R15:
        return false;
    default:
        return true;
    }
}

bool WritesRegister(const Instruction& instruction, const ZydisRegister reg) {
    for (std::uint8_t i = 0; i < instruction.Info.operand_count; ++i) {
        const auto& operand = instruction.Operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && Enclosing(operand.reg.value) == Enclosing(reg)) return true;
    }
    return false;
}

std::optional<std::uint64_t> BranchBound(const GuestFunction& function, const Instruction& compare) {
    const auto branch = function.Instructions.find(compare.Next());
    if (branch == function.Instructions.end()) return std::nullopt;
    const auto value = compare.Operands[1].imm.value.u;
    if (branch->second.Info.mnemonic == ZYDIS_MNEMONIC_JNBE) return value + 1;
    if (branch->second.Info.mnemonic == ZYDIS_MNEMONIC_JNB) return value;
    return std::nullopt;
}

bool CopiesRegister(const Instruction& instruction, const ZydisRegister to, const ZydisRegister from) {
    const auto& left = instruction.Operands[0];
    const auto& right = instruction.Operands[1];
    return instruction.Info.mnemonic == ZYDIS_MNEMONIC_MOV && left.type == ZYDIS_OPERAND_TYPE_REGISTER && left.size >= 32 && right.type == ZYDIS_OPERAND_TYPE_REGISTER &&
           Enclosing(left.reg.value) == Enclosing(to) && Enclosing(right.reg.value) == Enclosing(from);
}

// The instructions that set a register on the paths to an address, if one does on every path. A path from the entry,
// from code without a known predecessor, or through a call that need not preserve the register has none.
std::optional<std::vector<const Instruction*>> ReachingDefinitions(const GuestFunction& function, const Predecessors& predecessors, const std::uint64_t address,
                                                                   const ZydisRegister reg) {
    std::vector<const Instruction*> definitions;
    std::set<std::uint64_t> visited{address};
    std::vector<std::uint64_t> pending{address};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto found = predecessors.find(current);
        if (current == function.Entry || found == predecessors.end()) return std::nullopt;
        for (const auto predecessor : found->second) {
            if (!visited.insert(predecessor).second) continue;
            const auto& instruction = function.Instructions.at(predecessor);
            if (WritesRegister(instruction, reg)) {
                definitions.push_back(&instruction);
            } else if (instruction.Info.mnemonic == ZYDIS_MNEMONIC_CALL && ClobberedByCall(reg)) {
                return std::nullopt;
            } else {
                pending.push_back(predecessor);
            }
        }
    }
    return definitions;
}

const Instruction* SingleDefinition(const GuestFunction& function, const Predecessors& predecessors, const std::uint64_t address, const ZydisRegister reg) {
    const auto definitions = ReachingDefinitions(function, predecessors, address, reg);
    return definitions && definitions->size() == 1 ? definitions->front() : nullptr;
}

std::optional<std::uint64_t> RipRelativeAddress(const Instruction& instruction) {
    const auto& source = instruction.Operands[1];
    if (instruction.Info.mnemonic != ZYDIS_MNEMONIC_LEA || source.mem.base != ZYDIS_REGISTER_RIP || source.mem.index != ZYDIS_REGISTER_NONE) return std::nullopt;
    return instruction.Next() + static_cast<std::uint64_t>(source.mem.disp.value);
}

std::optional<ZydisRegister> CopiedRegister(const Instruction& instruction) {
    const auto& left = instruction.Operands[0];
    const auto& right = instruction.Operands[1];
    if (instruction.Info.mnemonic != ZYDIS_MNEMONIC_MOV || left.type != ZYDIS_OPERAND_TYPE_REGISTER || right.type != ZYDIS_OPERAND_TYPE_REGISTER || left.size != 64) return std::nullopt;
    return right.reg.value;
}

// The address a register holds at an instruction, if every definition that reaches it loads the same RIP-relative
// address or copies a register that does: the compiler may set the register again, or keep it in another, on a path
// that reuses it.
std::optional<std::uint64_t> RipRelativeValue(const GuestFunction& function, const Predecessors& predecessors, const std::uint64_t address, const ZydisRegister reg) {
    std::optional<std::uint64_t> value;
    std::set<std::pair<std::uint64_t, ZydisRegister>> visited;
    std::vector<std::pair<std::uint64_t, ZydisRegister>> pending{{address, reg}};
    while (!pending.empty()) {
        const auto [at, current] = pending.back();
        pending.pop_back();
        if (!visited.emplace(at, current).second) continue;
        const auto definitions = ReachingDefinitions(function, predecessors, at, current);
        if (!definitions || definitions->empty()) return std::nullopt;
        for (const auto* definition : *definitions) {
            if (const auto loaded = RipRelativeAddress(*definition)) {
                if (value && *value != *loaded) return std::nullopt;
                value = loaded;
            } else if (const auto source = CopiedRegister(*definition)) {
                pending.emplace_back(definition->Address, *source);
            } else {
                return std::nullopt;
            }
        }
    }
    return value;
}

std::optional<std::uint64_t> TableBound(const GuestFunction& function, const Predecessors& predecessors, const std::uint64_t jump, ZydisRegister index) {
    struct Compared {
        ZydisRegister Register;
        std::uint64_t Bound;
    };
    std::optional<Compared> compared;
    const auto* instruction = UniquePredecessor(function, predecessors, jump);
    for (int depth = 0; instruction != nullptr && depth < 12; ++depth, instruction = UniquePredecessor(function, predecessors, instruction->Address)) {
        const auto mnemonic = instruction->Info.mnemonic;
        const auto& left = instruction->Operands[0];
        if (mnemonic == ZYDIS_MNEMONIC_CMP || mnemonic == ZYDIS_MNEMONIC_SUB) {
            if (left.type != ZYDIS_OPERAND_TYPE_REGISTER || instruction->Operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return std::nullopt;
            const auto bound = BranchBound(function, *instruction);
            if (!bound) return std::nullopt;
            if (Enclosing(left.reg.value) == Enclosing(index)) return bound;
            if (mnemonic == ZYDIS_MNEMONIC_SUB || compared) return std::nullopt;
            compared = Compared{left.reg.value, *bound};
            continue;
        }
        if (compared && WritesRegister(*instruction, compared->Register)) return CopiesRegister(*instruction, compared->Register, index) ? std::optional(compared->Bound) : std::nullopt;
        if (!WritesRegister(*instruction, index)) continue;
        if (compared && CopiesRegister(*instruction, index, compared->Register)) return compared->Bound;
        if (instruction->Operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER || !CopiesRegister(*instruction, index, instruction->Operands[1].reg.value)) return std::nullopt;
        index = instruction->Operands[1].reg.value;
    }
    return std::nullopt;
}

struct TableShape {
    std::uint64_t Address;
    unsigned EntrySize;
    ZydisRegister Index;
    std::uint64_t BoundFrom;
};

std::optional<TableShape> MatchTable(const GuestFunction& function, const Predecessors& predecessors, const Instruction& jump) {
    const auto& target = jump.Operands[0];
    const auto absolute = [](const ZydisDecodedOperand& operand) {
        return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index != ZYDIS_REGISTER_NONE && operand.mem.scale == 8;
    };
    if (absolute(target)) return TableShape{static_cast<std::uint64_t>(target.mem.disp.value), 8, target.mem.index, jump.Address};
    if (target.type != ZYDIS_OPERAND_TYPE_REGISTER) return std::nullopt;
    // The scheduler may move other instructions between the load of an entry, the addition of the base and the jump.
    const auto* previous = SingleDefinition(function, predecessors, jump.Address, target.reg.value);
    if (previous == nullptr) return std::nullopt;
    if (previous->Info.mnemonic == ZYDIS_MNEMONIC_MOV && absolute(previous->Operands[1]))
        return TableShape{static_cast<std::uint64_t>(previous->Operands[1].mem.disp.value), 8, previous->Operands[1].mem.index, previous->Address};
    if (previous->Info.mnemonic != ZYDIS_MNEMONIC_ADD || previous->Operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER) return std::nullopt;
    const auto base = previous->Operands[1].reg.value;
    const auto* load = SingleDefinition(function, predecessors, previous->Address, target.reg.value);
    if (load == nullptr || load->Info.mnemonic != ZYDIS_MNEMONIC_MOVSXD) return std::nullopt;
    const auto& entry = load->Operands[1];
    if (entry.type != ZYDIS_OPERAND_TYPE_MEMORY || entry.mem.base != base || entry.mem.scale != 4 || entry.mem.disp.value != 0) return std::nullopt;
    const auto table = RipRelativeValue(function, predecessors, load->Address, base);
    if (!table || RipRelativeValue(function, predecessors, previous->Address, base) != table) return std::nullopt;
    return TableShape{*table, 4, entry.mem.index, load->Address};
}

std::optional<std::uint64_t> MemoryReference(const Instruction& instruction, const ZydisDecodedOperand& operand) {
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.segment == ZYDIS_REGISTER_FS || operand.mem.segment == ZYDIS_REGISTER_GS) return std::nullopt;
    if (operand.mem.base == ZYDIS_REGISTER_RIP) return instruction.Next() + static_cast<std::uint64_t>(operand.mem.disp.value);
    if (operand.mem.base == ZYDIS_REGISTER_NONE) return static_cast<std::uint64_t>(operand.mem.disp.value);
    return std::nullopt;
}

std::set<std::uint64_t> DataReferences(const GuestImage& image, const Decoder& decoder) {
    std::set<std::uint64_t> references;
    for (const auto& segment : image.Segments()) {
        if (!segment.IsExecutable()) continue;
        const std::span<const std::uint8_t> bytes(segment.Bytes);
        for (std::uint64_t offset = 0; offset < bytes.size();) {
            const auto instruction = decoder.TryDecode(bytes.subspan(offset), segment.Address + offset);
            if (!instruction) {
                ++offset;
                continue;
            }
            for (std::uint8_t i = 0; i < instruction->Info.operand_count_visible; ++i) {
                const auto reference = MemoryReference(*instruction, instruction->Operands[i]);
                if (reference && !image.IsExecutable(*reference) && image.ReadData(*reference, 1)) references.insert(*reference);
            }
            offset += instruction->Info.length;
        }
    }
    return references;
}

struct RecoveredTable {
    std::vector<std::uint64_t> Targets;
    bool Guarded = false;
};

RecoveredTable RecoverJumpTable(const GuestImage& image, const GuestFunction& function, const Predecessors& predecessors, const Instruction& jump, const std::uint64_t limit,
                                const std::set<std::uint64_t>& references) {
    constexpr std::uint64_t MaximumEntries = 4096;
    const auto shape = MatchTable(function, predecessors, jump);
    if (!shape) return {};
    const auto bound = TableBound(function, predecessors, shape->BoundFrom, shape->Index);
    if (bound && *bound > MaximumEntries) return {};
    auto count = bound.value_or(MaximumEntries);
    if (const auto next = references.upper_bound(shape->Address); !bound && next != references.end()) count = std::min(count, (*next - shape->Address) / shape->EntrySize);
    RecoveredTable table{{}, bound.has_value()};
    auto& targets = table.Targets;
    for (std::uint64_t i = 0; i < count; ++i) {
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
    return table;
}

std::optional<std::string> CalledImport(const GuestImage& image, const Decoder& decoder, const Instruction& instruction) {
    auto import = ImportTarget(image, instruction);
    if (const auto target = DirectTarget(instruction); !import && target) {
        if (image.Imports().contains(*target)) {
            import = *target;
        } else if (!image.CodeAt(*target).empty()) {
            const auto stub = decoder.Decode(image, *target);
            if (stub.Info.mnemonic == ZYDIS_MNEMONIC_JMP) import = ImportTarget(image, stub);
        }
    }
    if (!import) return std::nullopt;
    const auto name = image.Imports().find(*import);
    if (name == image.Imports().end()) return std::nullopt;
    return name->second;
}

bool PassesArgument(const ZydisRegister reg) {
    switch (Enclosing(reg)) {
    case ZYDIS_REGISTER_RDI:
    case ZYDIS_REGISTER_RSI:
    case ZYDIS_REGISTER_RDX:
    case ZYDIS_REGISTER_RCX:
    case ZYDIS_REGISTER_R8:
    case ZYDIS_REGISTER_R9:
        return true;
    default:
        return false;
    }
}

// Position-independent code takes a function's address with a RIP-relative lea, which needs no relocation. Code also
// keeps strings and tables among its instructions and takes their addresses the same way, so an address counts as a
// function's only if the code stores it, branches to it or passes it to a call before it reads memory through it.
std::optional<std::uint64_t> TakenFunctionAddress(const GuestImage& image, const GuestFunction& function, const Instruction& lea) {
    const auto address = RipRelativeAddress(lea);
    if (!address || !image.IsExecutable(*address)) return std::nullopt;
    const auto reg = lea.Operands[0].reg.value;
    auto next = function.Instructions.find(lea.Next());
    for (int depth = 0; depth < 16 && next != function.Instructions.end(); ++depth, next = function.Instructions.find(next->second.Next())) {
        const auto& instruction = next->second;
        const auto mnemonic = instruction.Info.mnemonic;
        const auto& target = instruction.Operands[0];
        for (std::uint8_t i = 0; i < instruction.Info.operand_count_visible; ++i) {
            const auto& operand = instruction.Operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && (Enclosing(operand.mem.base) == Enclosing(reg) || Enclosing(operand.mem.index) == Enclosing(reg))) return std::nullopt;
        }
        if ((mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_JMP) && target.type == ZYDIS_OPERAND_TYPE_REGISTER && target.reg.value == reg) return address;
        if (mnemonic == ZYDIS_MNEMONIC_CALL) return PassesArgument(reg) ? address : std::nullopt;
        if (mnemonic == ZYDIS_MNEMONIC_MOV && target.type == ZYDIS_OPERAND_TYPE_MEMORY && instruction.Operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && instruction.Operands[1].reg.value == reg)
            return address;
        if (WritesRegister(instruction, reg) || EndsFlow(instruction) || IsConditionalJump(mnemonic)) return std::nullopt;
    }
    return std::nullopt;
}

Instruction DecodeReached(const GuestImage& image, const Decoder& decoder, const std::uint64_t entry, const std::uint64_t address, const std::uint64_t start, const std::uint64_t from) {
    try {
        return decoder.Decode(image, address);
    } catch (const std::runtime_error& error) {
        if (start == entry) throw std::runtime_error(std::string(error.what()) + ", in code from the start of the function at 0x" + Hex(entry));
        throw std::runtime_error(std::string(error.what()) + ", in code from 0x" + Hex(start) + ", where 0x" + Hex(from) + " in the function at 0x" + Hex(entry) + " leads");
    }
}

GuestFunction DiscoverFunction(const GuestImage& image, const Decoder& decoder, const std::set<std::uint64_t>& references, const std::uint64_t entry, const std::set<std::uint64_t>& starts,
                               std::set<std::uint64_t>& reachedStarts) {
    GuestFunction function;
    function.Entry = entry;
    const auto symbol = image.FunctionSymbols().find(entry);
    function.Name = symbol != image.FunctionSymbols().end() ? symbol->second : "sub_" + Hex(entry);
    if (const auto import = image.Imports().find(entry); import != image.Imports().end()) {
        function.Import = import->second;
        function.Name = import->second;
        return function;
    }

    function.Leaders.insert(entry);
    std::deque<std::pair<std::uint64_t, std::uint64_t>> pending{{entry, entry}};
    if (const auto pads = image.LandingPads().find(entry); pads != image.LandingPads().end()) {
        for (const auto pad : pads->second) {
            function.Leaders.insert(pad);
            function.ResumePoints.insert(pad);
            pending.emplace_back(pad, entry);
        }
    }
    const auto next = starts.upper_bound(entry);
    const auto limit = next == starts.end() ? ~std::uint64_t{0} : *next;
    std::vector<std::uint64_t> indirectJumps;
    while (true) {
        while (!pending.empty()) {
            const auto [start, from] = pending.front();
            pending.pop_front();
            auto address = start;
            while (!function.Instructions.contains(address)) {
                const auto instruction = DecodeReached(image, decoder, entry, address, start, from);
                function.Instructions.emplace(address, instruction);
                const auto target = DirectTarget(instruction);
                const auto mnemonic = instruction.Info.mnemonic;
                if (mnemonic == ZYDIS_MNEMONIC_CALL && target) reachedStarts.insert(*target);
                if (mnemonic == ZYDIS_MNEMONIC_CALL) {
                    if (const auto import = CalledImport(image, decoder, instruction); import && ReturnsTwice(*import)) {
                        function.ResumePoints.insert(instruction.Next());
                        function.Leaders.insert(instruction.Next());
                    }
                }
                if (mnemonic == ZYDIS_MNEMONIC_JMP && !target && ImportTarget(image, instruction)) break;
                if ((mnemonic == ZYDIS_MNEMONIC_JMP || IsConditionalJump(mnemonic)) && target && !(starts.contains(*target) && *target != entry)) {
                    function.Leaders.insert(*target);
                    pending.emplace_back(*target, address);
                }
                if (IsConditionalJump(mnemonic)) function.Leaders.insert(instruction.Next());
                if (mnemonic == ZYDIS_MNEMONIC_JMP && !target) indirectJumps.push_back(address);
                if (EndsFlow(instruction)) break;
                address = instruction.Next();
                if (starts.contains(address) && address != entry) break;
            }
        }
        // A table's base or guard can lie on a path that only another table leads to, so tables are recovered once the
        // code reached without them is known, and again after each round of code they reveal.
        const auto predecessors = PredecessorsOf(function);
        std::vector<std::uint64_t> unresolved;
        for (const auto jump : indirectJumps) {
            auto table = RecoverJumpTable(image, function, predecessors, function.Instructions.at(jump), limit, references);
            if (table.Targets.empty()) {
                unresolved.push_back(jump);
                continue;
            }
            for (const auto destination : table.Targets) {
                if (starts.contains(destination) && destination != entry) continue;
                function.Leaders.insert(destination);
                pending.emplace_back(destination, jump);
            }
            function.JumpTables.emplace(jump, std::move(table.Targets));
            if (!table.Guarded) function.UnguardedJumpTables.insert(jump);
        }
        if (unresolved.size() == indirectJumps.size()) break;
        indirectJumps = std::move(unresolved);
    }
    for (const auto& [address, instruction] : function.Instructions) {
        if (const auto taken = TakenFunctionAddress(image, function, instruction)) reachedStarts.insert(*taken);
    }
    const auto predecessors = PredecessorsOf(function);
    for (const auto& [jump, targets] : function.JumpTables) {
        if (RecoverJumpTable(image, function, predecessors, function.Instructions.at(jump), limit, references).Targets != targets)
            throw std::runtime_error("The jump table of the jump at 0x" + Hex(jump) + " in the function at 0x" + Hex(entry) + " reads differently once the code it leads to is known");
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
    const auto instruction = TryDecode(code, address);
    if (!instruction) throw std::runtime_error("Cannot decode the instruction at 0x" + Hex(address));
    return *instruction;
}

std::optional<Instruction> Decoder::TryDecode(const std::span<const std::uint8_t> code, const std::uint64_t address) const {
    Instruction instruction{};
    instruction.Address = address;
    if (ZYAN_FAILED(ZydisDecoderDecodeFull(&_decoder, code.data(), code.size(), &instruction.Info, instruction.Operands.data()))) return std::nullopt;
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

std::optional<std::uint64_t> ImportTarget(const GuestImage& image, const Instruction& instruction) {
    const auto& operand = instruction.Operands[0];
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY || operand.mem.index != ZYDIS_REGISTER_NONE || operand.mem.segment == ZYDIS_REGISTER_FS || operand.mem.segment == ZYDIS_REGISTER_GS) return std::nullopt;
    const auto displacement = static_cast<std::uint64_t>(operand.mem.disp.value);
    if (operand.mem.base == ZYDIS_REGISTER_RIP) return image.ImportAt(instruction.Next() + displacement);
    if (operand.mem.base == ZYDIS_REGISTER_NONE) return image.ImportAt(displacement);
    return std::nullopt;
}

std::string BridgeSymbol(const std::string& import) {
    std::string symbol = "ps5lift_import_";
    for (const char character : import) {
        if (character == '_') symbol += "__";
        else if (character == '+') symbol += "_p";
        else if (character == '-') symbol += "_m";
        else if (character == '#') symbol += "_h";
        else if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) symbol += character;
        else throw std::runtime_error("Import name " + import + " has a character a bridge symbol cannot carry");
    }
    return symbol;
}

std::string Nid(const std::string_view name) {
    static constexpr std::uint8_t Suffix[] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    llvm::SHA1 sha;
    sha.update(llvm::StringRef(name.data(), name.size()));
    sha.update(llvm::ArrayRef<std::uint8_t>(Suffix));
    const auto digest = sha.final();
    std::array<std::uint8_t, 8> prefix{};
    std::copy_n(digest.begin(), prefix.size(), prefix.begin());
    std::reverse(prefix.begin(), prefix.end());
    auto encoded = llvm::encodeBase64(prefix);
    std::replace(encoded.begin(), encoded.end(), '/', '-');
    return encoded.substr(0, 11);
}

bool ReturnsTwice(const std::string& import) {
    static const std::set<std::string> Names = [] {
        std::set<std::string> names;
        for (const std::string name : {"setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp"}) {
            names.insert(name);
            names.insert(Nid(name));
        }
        return names;
    }();
    return Names.contains(import);
}

std::map<std::uint64_t, GuestFunction> DiscoverFunctions(const GuestImage& image) {
    const Decoder decoder;
    const auto references = DataReferences(image, decoder);
    std::set<std::uint64_t> starts;
    for (const auto start : {image.Entry(), image.Init()}) {
        if (start != 0) starts.insert(start);
    }
    for (const auto& [address, name] : image.Imports()) starts.insert(address);
    for (const auto& [name, address] : image.Exports()) {
        if (image.IsExecutable(address)) starts.insert(address);
    }
    for (const auto address : image.CodePointers()) starts.insert(address);
    for (const auto& [address, name] : image.FunctionSymbols()) {
        if (image.IsExecutable(address)) starts.insert(address);
    }
    for (const auto address : image.ExceptionFunctionStarts()) starts.insert(address);

    while (true) {
        std::map<std::uint64_t, GuestFunction> functions;
        std::set<std::uint64_t> reachedStarts;
        for (const auto entry : starts) functions.emplace(entry, DiscoverFunction(image, decoder, references, entry, starts, reachedStarts));
        const auto before = starts.size();
        starts.insert(reachedStarts.begin(), reachedStarts.end());
        if (starts.size() == before) return functions;
    }
}

}
