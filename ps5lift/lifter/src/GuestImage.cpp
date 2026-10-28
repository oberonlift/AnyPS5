#include <lifter/GuestImage.hpp>

#include <relinker/analysis/UnusedNidFilter/EhFrameReader.hpp>
#include <relinker/parsing/ElfReader.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace Ps5Lift {

namespace {

constexpr std::uint32_t ProgramLoad = 1;
constexpr std::uint32_t ProgramDynamic = 2;
constexpr std::uint32_t ProgramTls = 7;
constexpr std::uint32_t ProgramEhFrame = 0x6474e550;
constexpr std::uint32_t SectionSymbolTable = 2;
constexpr std::uint8_t SymbolObject = 1;
constexpr std::uint8_t SymbolFunction = 2;
constexpr std::int64_t DynamicStringTable = 5;
constexpr std::int64_t DynamicSymbolTable = 6;
constexpr std::int64_t DynamicRela = 7;
constexpr std::int64_t DynamicRelaSize = 8;
constexpr std::int64_t DynamicSymbolEntry = 11;
constexpr std::int64_t DynamicJumpRelocations = 23;
constexpr std::int64_t DynamicJumpRelocationsSize = 2;
constexpr std::uint32_t RelocationNone = 0;
constexpr std::uint32_t RelocationAbsolute = 1;
constexpr std::uint32_t RelocationGlobalData = 6;
constexpr std::uint32_t RelocationJumpSlot = 7;
constexpr std::uint32_t RelocationRelative = 8;
constexpr std::uint64_t ImportStride = 16;
constexpr std::uint64_t ImportAlignment = 0x10000;

std::string Hex(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

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

Symbol ReadSymbol(const std::vector<std::uint8_t>& bytes, const std::uint64_t offset, const std::uint64_t strings) {
    Symbol symbol;
    symbol.Name = ReadString(bytes, strings + ReadAt<std::uint32_t>(bytes, offset));
    symbol.Type = ReadAt<std::uint8_t>(bytes, offset + 4) & 0xf;
    symbol.Section = ReadAt<std::uint16_t>(bytes, offset + 6);
    symbol.Value = ReadAt<std::uint64_t>(bytes, offset + 8);
    return symbol;
}

struct Relocation {
    std::uint64_t Offset;
    std::uint32_t Type;
    std::uint32_t Symbol;
    std::int64_t Addend;
};

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
    std::map<std::uint64_t, std::uint64_t> relocated;
    std::uint64_t imageEnd = 0;
    for (const auto& program : programs) {
        if (program.Type == ProgramEhFrame) image._ehFrameHeader = program.MappedAddress;
        if (program.Type == ProgramTls) image._tls = {program.MappedAddress, program.FileSize, program.MemorySize, std::max<std::uint64_t>(program.Alignment, 1)};
        if (program.Type != ProgramLoad || program.MemorySize == 0) continue;
        GuestSegment segment;
        segment.Address = program.MappedAddress;
        segment.MemorySize = program.MemorySize;
        segment.Flags = program.Flags;
        segment.Bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset), bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset + program.FileSize));
        imageEnd = std::max(imageEnd, segment.Address + segment.MemorySize);
        image._segments.push_back(std::move(segment));
    }

    for (const auto& section : sections) {
        if (section.Type != SectionSymbolTable || section.EntrySize != 24) continue;
        const auto& strings = sections.at(section.Link);
        for (std::uint64_t offset = section.Offset; offset + 24 <= section.Offset + section.SectionSize; offset += 24) {
            const auto symbol = ReadSymbol(bytes, offset, strings.Offset);
            if (symbol.Type == SymbolFunction && symbol.Section != 0 && symbol.Value != 0) image._functionSymbols.emplace(symbol.Value, symbol.Name);
        }
    }

    const auto dynamic = std::find_if(programs.begin(), programs.end(), [](const auto& program) { return program.Type == ProgramDynamic; });
    if (dynamic != programs.end()) {
        std::map<std::int64_t, std::uint64_t> tags;
        for (const auto& tag : reader.ReadDynamicTags(*dynamic)) tags.emplace(tag.Tag, tag.Value);
        const auto fileOffset = [&](const std::int64_t tag) { return reader.TranslateVirtualAddress(tags.at(tag)); };
        std::vector<Relocation> relocations;
        const auto readTable = [&](const std::int64_t table, const std::int64_t size) {
            if (!tags.contains(table) || !tags.contains(size)) return;
            const auto begin = fileOffset(table);
            for (std::uint64_t offset = begin; offset + 24 <= begin + tags.at(size); offset += 24) {
                const auto info = ReadAt<std::uint64_t>(bytes, offset + 8);
                relocations.push_back({ReadAt<std::uint64_t>(bytes, offset), static_cast<std::uint32_t>(info), static_cast<std::uint32_t>(info >> 32), ReadAt<std::int64_t>(bytes, offset + 16)});
            }
        };
        readTable(DynamicRela, DynamicRelaSize);
        readTable(DynamicJumpRelocations, DynamicJumpRelocationsSize);

        const auto symbolEntry = tags.contains(DynamicSymbolEntry) ? tags.at(DynamicSymbolEntry) : 24;
        const auto symbolAt = [&](const std::uint32_t index) {
            if (!tags.contains(DynamicSymbolTable) || !tags.contains(DynamicStringTable)) throw std::runtime_error("Relocations name symbols but the dynamic segment has no symbol table");
            return ReadSymbol(bytes, fileOffset(DynamicSymbolTable) + index * symbolEntry, fileOffset(DynamicStringTable));
        };

        auto nextImport = (imageEnd + ImportAlignment - 1) & ~(ImportAlignment - 1);
        std::map<std::string, std::uint64_t> importByName;
        for (const auto& relocation : relocations) {
            if (relocation.Type == RelocationNone) continue;
            std::uint64_t value = 0;
            if (relocation.Type == RelocationRelative) {
                value = static_cast<std::uint64_t>(relocation.Addend);
            } else if (relocation.Type == RelocationAbsolute || relocation.Type == RelocationGlobalData || relocation.Type == RelocationJumpSlot) {
                const auto symbol = symbolAt(relocation.Symbol);
                if (symbol.Section == 0 && symbol.Type == SymbolObject) {
                    image._dataImports.push_back({relocation.Offset, symbol.Name, relocation.Type == RelocationAbsolute ? relocation.Addend : 0});
                    image.Write64(relocation.Offset, 0);
                    continue;
                }
                if (symbol.Section != 0) {
                    value = symbol.Value;
                } else {
                    auto [found, inserted] = importByName.emplace(symbol.Name, nextImport);
                    if (inserted) {
                        image._imports.emplace(nextImport, symbol.Name);
                        nextImport += ImportStride;
                    }
                    value = found->second;
                    if (relocation.Addend == 0) image._importSlots.emplace(relocation.Offset, value);
                }
                if (relocation.Type == RelocationAbsolute) value += static_cast<std::uint64_t>(relocation.Addend);
            } else {
                throw std::runtime_error("Unsupported relocation type " + std::to_string(relocation.Type) + " at 0x" + Hex(relocation.Offset));
            }
            image.Write64(relocation.Offset, value);
            relocated[relocation.Offset] = value;
            if (image.IsExecutable(value)) image._codePointers.insert(value);
        }
    }

    for (const auto& region : Relinker::UnusedNidFilter::ReadExceptionFunctions(bytes, programs, relocated, {})) {
        image._exceptionStarts.insert(region.Begin);
        for (const auto target : region.ExtraTargets) {
            if (target >= region.Begin && target < region.End) image._landingPads[region.Begin].insert(target);
        }
    }
    return image;
}

void GuestImage::Write64(const std::uint64_t address, const std::uint64_t value) {
    for (auto& segment : _segments) {
        if (address < segment.Address || address + 8 > segment.Address + segment.MemorySize) continue;
        const auto offset = address - segment.Address;
        if (offset + 8 > segment.Bytes.size()) segment.Bytes.resize(offset + 8, 0);
        std::memcpy(segment.Bytes.data() + offset, &value, 8);
        return;
    }
    throw std::runtime_error("Relocation at 0x" + Hex(address) + " is outside every loaded segment");
}

std::uint64_t GuestImage::Entry() const {
    return _entry;
}

bool GuestSegment::IsExecutable() const {
    constexpr std::uint32_t Execute = 1;
    return (Flags & Execute) != 0;
}

const std::vector<GuestSegment>& GuestImage::Segments() const {
    return _segments;
}

std::span<const std::uint8_t> GuestImage::CodeAt(const std::uint64_t address) const {
    for (const auto& segment : _segments) {
        if (!segment.IsExecutable()) continue;
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

const std::map<std::uint64_t, std::string>& GuestImage::FunctionSymbols() const {
    return _functionSymbols;
}

const std::map<std::uint64_t, std::string>& GuestImage::Imports() const {
    return _imports;
}

std::optional<std::uint64_t> GuestImage::ImportAt(const std::uint64_t slot) const {
    const auto found = _importSlots.find(slot);
    if (found == _importSlots.end()) return std::nullopt;
    return found->second;
}

const std::set<std::uint64_t>& GuestImage::CodePointers() const {
    return _codePointers;
}

const std::set<std::uint64_t>& GuestImage::ExceptionFunctionStarts() const {
    return _exceptionStarts;
}

const std::map<std::uint64_t, std::set<std::uint64_t>>& GuestImage::LandingPads() const {
    return _landingPads;
}

const std::vector<GuestDataImport>& GuestImage::DataImports() const {
    return _dataImports;
}

std::uint64_t GuestImage::EhFrameHeader() const {
    return _ehFrameHeader;
}

const GuestTls& GuestImage::Tls() const {
    return _tls;
}

}
