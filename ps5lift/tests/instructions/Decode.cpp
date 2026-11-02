#include <lifter/Decoder.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Escaped(const std::string_view text) {
    std::string escaped;
    for (const char c : text) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return escaped;
}

std::string Quoted(const std::string_view text) {
    return "\"" + Escaped(text) + "\"";
}

std::string Action(const ZydisOperandActions actions) {
    std::string result;
    const auto add = [&](const ZydisOperandActions flag, const char* name) {
        if (!(actions & flag)) return;
        if (!result.empty()) result += '+';
        result += name;
    };
    add(ZYDIS_OPERAND_ACTION_READ, "read");
    add(ZYDIS_OPERAND_ACTION_WRITE, "write");
    add(ZYDIS_OPERAND_ACTION_CONDREAD, "condread");
    add(ZYDIS_OPERAND_ACTION_CONDWRITE, "condwrite");
    return result;
}

const char* Type(const ZydisOperandType type) {
    switch (type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: return "register";
    case ZYDIS_OPERAND_TYPE_MEMORY: return "memory";
    case ZYDIS_OPERAND_TYPE_POINTER: return "pointer";
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: return "immediate";
    default: return "unused";
    }
}

const char* Visibility(const ZydisOperandVisibility visibility) {
    switch (visibility) {
    case ZYDIS_OPERAND_VISIBILITY_EXPLICIT: return "explicit";
    case ZYDIS_OPERAND_VISIBILITY_IMPLICIT: return "implicit";
    case ZYDIS_OPERAND_VISIBILITY_HIDDEN: return "hidden";
    default: return "invalid";
    }
}

std::string Operand(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand& operand) {
    std::ostringstream json;
    json << "{\"type\":" << Quoted(Type(operand.type)) << ",\"visibility\":" << Quoted(Visibility(operand.visibility))
         << ",\"action\":" << Quoted(Action(operand.actions)) << ",\"size\":" << operand.size;
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        json << ",\"register\":" << Quoted(ZydisRegisterGetString(operand.reg.value));
    } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        json << ",\"base\":" << Quoted(ZydisRegisterGetString(operand.mem.base)) << ",\"index\":" << Quoted(ZydisRegisterGetString(operand.mem.index))
             << ",\"scale\":" << static_cast<unsigned>(operand.mem.scale) << ",\"displacement\":" << (operand.mem.disp.has_displacement ? operand.mem.disp.value : 0)
             << ",\"displacement_size\":" << (operand.mem.disp.has_displacement ? static_cast<unsigned>(instruction.raw.disp.size) : 0u);
    } else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        json << ",\"immediate\":";
        if (operand.imm.is_signed) json << static_cast<std::int64_t>(operand.imm.value.s);
        else json << operand.imm.value.u;
    }
    json << "}";
    return json.str();
}

std::string Decode(const ZydisDecoder& decoder, const ZydisFormatter& formatter, const std::vector<std::uint8_t>& bytes) {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const ZyanStatus status = ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &instruction, operands);
    if (ZYAN_FAILED(status)) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(status));
        return std::string("{\"error\":") + Quoted(hex) + "}";
    }
    char text[256];
    if (ZYAN_FAILED(ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text, sizeof text, 0, nullptr))) {
        std::cerr << "ps5lift_decode: formatting failed\n";
        std::exit(1);
    }
    std::ostringstream json;
    json << "{\"length\":" << static_cast<unsigned>(instruction.length) << ",\"mnemonic\":" << Quoted(ZydisMnemonicGetString(instruction.mnemonic))
         << ",\"text\":" << Quoted(text) << ",\"isa_ext\":" << Quoted(ZydisISAExtGetString(instruction.meta.isa_ext))
         << ",\"isa_set\":" << Quoted(ZydisISASetGetString(instruction.meta.isa_set)) << ",\"category\":" << Quoted(ZydisCategoryGetString(instruction.meta.category))
         << ",\"operand_width\":" << static_cast<unsigned>(instruction.operand_width)
         << ",\"privileged\":" << ((instruction.attributes & ZYDIS_ATTRIB_IS_PRIVILEGED) ? "true" : "false") << ",\"operands\":[";
    for (unsigned i = 0; i < instruction.operand_count; ++i) json << (i ? "," : "") << Operand(instruction, operands[i]);
    json << "]}";
    return json.str();
}

std::vector<std::uint8_t> ParseHex(const std::string& line) {
    std::vector<std::uint8_t> bytes;
    std::istringstream tokens(line);
    std::string token;
    while (tokens >> token) {
        std::size_t used = 0;
        const unsigned long value = std::stoul(token, &used, 16);
        if (used != token.size() || value > 0xff) throw std::invalid_argument("not a hex byte: " + token);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

}

int main(const int argc, const char* const argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--mnemonics") == 0) {
        for (int mnemonic = ZYDIS_MNEMONIC_INVALID + 1; mnemonic <= ZYDIS_MNEMONIC_MAX_VALUE; ++mnemonic) std::cout << ZydisMnemonicGetString(static_cast<ZydisMnemonic>(mnemonic)) << '\n';
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: ps5lift_decode [--mnemonics] < hex-lines\n";
        return 2;
    }
    try {
        ZydisDecoder decoder;
        Ps5Lift::InitializeDecoder(decoder);
        ZydisFormatter formatter;
        if (ZYAN_FAILED(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL))) throw std::runtime_error("Zydis formatter initialization failed");
        std::string line;
        while (std::getline(std::cin, line)) std::cout << Decode(decoder, formatter, ParseHex(line)) << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "ps5lift_decode: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
