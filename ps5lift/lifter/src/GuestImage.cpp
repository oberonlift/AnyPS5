#include <lifter/GuestImage.hpp>

#include <relinker/analysis/UnusedNidFilter/EhFrameReader.hpp>
#include <relinker/parsing/ElfReader.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace Ps5Lift {

namespace {

constexpr std::uint32_t ProgramLoad = 1;
constexpr std::uint32_t SectionSymbolTable = 2;
constexpr std::uint32_t SectionRela = 4;
constexpr std::uint32_t SectionDynamicSymbols = 11;
constexpr std::uint32_t RelocationGlobalData = 6;
constexpr std::uint32_t RelocationJumpSlot = 7;
constexpr std::uint8_t SymbolFunction = 2;
constexpr std::uint32_t SegmentExecute = 1;

template <typename T>
T ReadAt(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("ELF read out of bounds at " + std::to_string(offset));
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::string ReadString(const std::vector<std::uint8_t>& bytes, std::uint64_t offset) {
    if (offset >= bytes.size()) throw std::runtime_error("ELF string out of bounds at " + std::to_string(offset));
    const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
    return std::string(begin, strnlen(begin, bytes.size() - offset));
}

struct Symbol {
    std::string Name;
    std::uint8_t Type;
    std::uint16_t Section;
    std::uint64_t Value;
};

std::vector<Symbol> ReadSymbols(const std::vector<std::uint8_t>& bytes, const Relinker::SectionHeader& table, const Relinker::SectionHeader& strings) {
    std::vector<Symbol> symbols;
    if (table.EntrySize != 24) throw std::runtime_error("Unexpected ELF symbol size in " + table.Name);
    for (std::uint64_t offset = table.Offset; offset + 24 <= table.Offset + table.SectionSize; offset += 24) {
        Symbol symbol;
        symbol.Name = ReadString(bytes, strings.Offset + ReadAt<std::uint32_t>(bytes, offset));
        symbol.Type = ReadAt<std::uint8_t>(bytes, offset + 4) & 0xf;
        symbol.Section = ReadAt<std::uint16_t>(bytes, offset + 6);
        symbol.Value = ReadAt<std::uint64_t>(bytes, offset + 8);
        symbols.push_back(std::move(symbol));
    }
    return symbols;
}

}

GuestImage GuestImage::Load(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    Relinker::ElfReader reader(bytes);
    const auto header = reader.ReadHeader();
    const auto programs = reader.ReadProgramHeaders();
    const auto sections = reader.ReadSectionHeaders();

    GuestImage image;
    image._entry = header.EntryPoint;
    for (const auto& program : programs) {
        if (program.Type != ProgramLoad || program.MemorySize == 0) continue;
        GuestSegment segment;
        segment.Address = program.MappedAddress;
        segment.MemorySize = program.MemorySize;
        segment.Flags = program.Flags;
        segment.Bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset), bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset + program.FileSize));
        image._segments.push_back(std::move(segment));
    }

    for (const auto& section : sections) {
        if (section.Name == ".plt" || section.Name == ".plt.sec") image._pltRanges.emplace_back(section.MappedAddress, section.MappedAddress + section.SectionSize);
        if (section.Type == SectionSymbolTable) {
            for (const auto& symbol : ReadSymbols(bytes, section, sections.at(section.Link))) {
                if (symbol.Type == SymbolFunction && symbol.Section != 0 && symbol.Value != 0) image._functionSymbols.emplace(symbol.Value, symbol.Name);
            }
        }
    }

    for (const auto& section : sections) {
        if (section.Type != SectionRela) continue;
        const auto& symbolTable = sections.at(section.Link);
        if (symbolTable.Type != SectionDynamicSymbols) continue;
        const auto symbols = ReadSymbols(bytes, symbolTable, sections.at(symbolTable.Link));
        for (std::uint64_t offset = section.Offset; offset + 24 <= section.Offset + section.SectionSize; offset += 24) {
            const auto slot = ReadAt<std::uint64_t>(bytes, offset);
            const auto info = ReadAt<std::uint64_t>(bytes, offset + 8);
            const auto type = static_cast<std::uint32_t>(info);
            if (type != RelocationJumpSlot && type != RelocationGlobalData) continue;
            image._importSlots.emplace(slot, symbols.at(info >> 32).Name);
        }
    }

    for (const auto& region : Relinker::UnusedNidFilter::ReadExceptionFunctions(bytes, programs, {}, {})) image._exceptionStarts.insert(region.Begin);
    return image;
}

std::uint64_t GuestImage::Entry() const {
    return _entry;
}

const std::vector<GuestSegment>& GuestImage::Segments() const {
    return _segments;
}

std::span<const std::uint8_t> GuestImage::CodeAt(const std::uint64_t address) const {
    for (const auto& segment : _segments) {
        if ((segment.Flags & SegmentExecute) == 0) continue;
        if (address >= segment.Address && address < segment.Address + segment.Bytes.size()) {
            const auto offset = address - segment.Address;
            return {segment.Bytes.data() + offset, segment.Bytes.size() - offset};
        }
    }
    return {};
}

std::optional<std::uint64_t> GuestImage::ReadData(const std::uint64_t address, const unsigned size) const {
    for (const auto& segment : _segments) {
        if (address < segment.Address || address + size > segment.Address + segment.Bytes.size()) continue;
        std::uint64_t value = 0;
        std::memcpy(&value, segment.Bytes.data() + (address - segment.Address), size);
        return value;
    }
    return std::nullopt;
}

bool GuestImage::IsExecutable(const std::uint64_t address) const {
    return !CodeAt(address).empty();
}

bool GuestImage::IsPlt(const std::uint64_t address) const {
    for (const auto& [begin, end] : _pltRanges) {
        if (address >= begin && address < end) return true;
    }
    return false;
}

const std::map<std::uint64_t, std::string>& GuestImage::FunctionSymbols() const {
    return _functionSymbols;
}

const std::map<std::uint64_t, std::string>& GuestImage::ImportSlots() const {
    return _importSlots;
}

const std::set<std::uint64_t>& GuestImage::ExceptionFunctionStarts() const {
    return _exceptionStarts;
}

}
