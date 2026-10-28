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

constexpr std::uint16_t ElfExecutable = 2;
constexpr std::uint16_t ElfShared = 3;
constexpr std::uint32_t ProgramLoad = 1;
constexpr std::uint32_t ProgramDynamic = 2;
constexpr std::uint32_t ProgramTls = 7;
constexpr std::uint32_t ProgramEhFrame = 0x6474e550;
constexpr std::uint32_t SectionSymbolTable = 2;
constexpr std::uint32_t SectionDynamicSymbols = 11;
constexpr std::uint8_t SymbolObject = 1;
constexpr std::uint8_t SymbolFunction = 2;
constexpr std::int64_t DynamicNeeded = 1;
constexpr std::int64_t DynamicHash = 4;
constexpr std::int64_t DynamicStringTable = 5;
constexpr std::int64_t DynamicSymbolTable = 6;
constexpr std::int64_t DynamicRela = 7;
constexpr std::int64_t DynamicRelaSize = 8;
constexpr std::int64_t DynamicSymbolEntry = 11;
constexpr std::int64_t DynamicInit = 12;
constexpr std::int64_t DynamicInitArray = 25;
constexpr std::int64_t DynamicInitArraySize = 27;
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

std::uint64_t AlignUp(const std::uint64_t value, const std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
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
    std::uint8_t Binding;
    std::uint8_t Visibility;
    std::uint16_t Section;
    std::uint64_t Value;
};

Symbol ReadSymbol(const std::vector<std::uint8_t>& bytes, const std::uint64_t offset, const std::uint64_t strings) {
    Symbol symbol;
    symbol.Name = ReadString(bytes, strings + ReadAt<std::uint32_t>(bytes, offset));
    symbol.Type = ReadAt<std::uint8_t>(bytes, offset + 4) & 0xf;
    symbol.Binding = ReadAt<std::uint8_t>(bytes, offset + 4) >> 4;
    symbol.Visibility = ReadAt<std::uint8_t>(bytes, offset + 5) & 3;
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

bool Exported(const Symbol& symbol) {
    constexpr std::uint8_t Hidden = 2;
    constexpr std::uint8_t Internal = 1;
    return symbol.Section != 0 && symbol.Binding != 0 && symbol.Visibility != Hidden && symbol.Visibility != Internal && !symbol.Name.empty();
}

}

GuestImage GuestImage::Read(const std::string& path, const std::optional<std::uint64_t> base) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    Relinker::ElfReader reader(bytes);
    const auto header = reader.ReadHeader();
    const auto programs = reader.ReadProgramHeaders();
    const auto sections = reader.ReadSectionHeaders();
    if (header.Type != ElfExecutable && header.Type != ElfShared) throw std::runtime_error(path + " is neither an executable nor a shared object");
    if (header.Type == ElfExecutable && base) throw std::runtime_error(path + " is a fixed-address executable, which cannot be moved");
    if (header.Type == ElfShared && !base) throw std::runtime_error(path + " is position-independent and needs a base");
    std::uint64_t lowest = ~std::uint64_t{0};
    for (const auto& program : programs) {
        if (program.Type == ProgramLoad && program.MemorySize != 0) lowest = std::min(lowest, program.MappedAddress);
    }
    const std::uint64_t bias = base ? *base - lowest : 0;

    GuestImage image;
    image._path = path;
    image._entry = header.EntryPoint == 0 ? 0 : header.EntryPoint + bias;
    for (const auto& program : programs) {
        if (program.Type == ProgramEhFrame) image._ehFrameHeader = program.MappedAddress + bias;
        if (program.Type == ProgramTls) image._tls = {program.MappedAddress + bias, program.FileSize, program.MemorySize, std::max<std::uint64_t>(program.Alignment, 1)};
        if (program.Type != ProgramLoad || program.MemorySize == 0) continue;
        GuestSegment segment;
        segment.Address = program.MappedAddress + bias;
        segment.MemorySize = program.MemorySize;
        segment.Flags = program.Flags;
        segment.Bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset), bytes.begin() + static_cast<std::ptrdiff_t>(program.Offset + program.FileSize));
        image._mappedEnd = std::max(image._mappedEnd, segment.Address + segment.MemorySize);
        image._segments.push_back(std::move(segment));
    }

    for (const auto& section : sections) {
        if (section.Type != SectionSymbolTable || section.EntrySize != 24) continue;
        const auto& strings = sections.at(section.Link);
        for (std::uint64_t offset = section.Offset; offset + 24 <= section.Offset + section.SectionSize; offset += 24) {
            const auto symbol = ReadSymbol(bytes, offset, strings.Offset);
            if (symbol.Type == SymbolFunction && symbol.Section != 0 && symbol.Value != 0) image._functionSymbols.emplace(symbol.Value + bias, symbol.Name);
        }
    }

    std::map<std::uint64_t, std::uint64_t> relocated;
    std::set<std::uint64_t> importSlots;
    const auto dynamic = std::find_if(programs.begin(), programs.end(), [](const auto& program) { return program.Type == ProgramDynamic; });
    if (dynamic != programs.end()) {
        std::map<std::int64_t, std::uint64_t> tags;
        std::vector<std::uint64_t> needed;
        for (const auto& tag : reader.ReadDynamicTags(*dynamic)) {
            if (tag.Tag == DynamicNeeded) needed.push_back(tag.Value);
            tags.emplace(tag.Tag, tag.Value);
        }
        const auto fileOffset = [&](const std::int64_t tag) { return reader.TranslateVirtualAddress(tags.at(tag)); };
        if (tags.contains(DynamicStringTable)) {
            for (const auto name : needed) image._needed.push_back(ReadString(bytes, fileOffset(DynamicStringTable) + name));
        }
        if (tags.contains(DynamicInit)) image._init = tags.at(DynamicInit) + bias;
        if (tags.contains(DynamicInitArray)) image._initArray = {tags.at(DynamicInitArray) + bias, tags.contains(DynamicInitArraySize) ? tags.at(DynamicInitArraySize) / 8 : 0};

        const auto symbolEntry = tags.contains(DynamicSymbolEntry) ? tags.at(DynamicSymbolEntry) : 24;
        const auto symbolAt = [&](const std::uint32_t index) {
            if (!tags.contains(DynamicSymbolTable) || !tags.contains(DynamicStringTable)) throw std::runtime_error("Relocations name symbols but the dynamic segment has no symbol table");
            return ReadSymbol(bytes, fileOffset(DynamicSymbolTable) + index * symbolEntry, fileOffset(DynamicStringTable));
        };
        if (tags.contains(DynamicSymbolTable)) {
            std::uint64_t count = 0;
            if (tags.contains(DynamicHash)) {
                count = ReadAt<std::uint32_t>(bytes, fileOffset(DynamicHash) + 4);
            } else {
                const auto table = std::find_if(sections.begin(), sections.end(), [](const auto& section) { return section.Type == SectionDynamicSymbols; });
                if (table == sections.end()) throw std::runtime_error(path + " has dynamic symbols but neither DT_HASH nor a .dynsym section to count them");
                count = table->SectionSize / symbolEntry;
            }
            for (std::uint32_t index = 1; index < count; ++index) {
                const auto symbol = symbolAt(index);
                if (Exported(symbol)) image._exports.emplace(symbol.Name, symbol.Value + bias);
            }
        }

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

        std::set<std::string> imported;
        for (const auto& relocation : relocations) {
            if (relocation.Type == RelocationNone) continue;
            if (relocation.Type == RelocationRelative) {
                relocated[relocation.Offset] = static_cast<std::uint64_t>(relocation.Addend);
                image._relocations.push_back({relocation.Offset + bias, relocation.Type, relocation.Addend, "", true, false, static_cast<std::uint64_t>(relocation.Addend) + bias});
            } else if (relocation.Type == RelocationAbsolute || relocation.Type == RelocationGlobalData || relocation.Type == RelocationJumpSlot) {
                const auto symbol = symbolAt(relocation.Symbol);
                const bool defined = symbol.Section != 0;
                if (!defined && symbol.Type != SymbolObject) imported.insert(symbol.Name);
                if (defined) relocated[relocation.Offset] = symbol.Value + (relocation.Type == RelocationAbsolute ? static_cast<std::uint64_t>(relocation.Addend) : 0);
                else importSlots.insert(relocation.Offset);
                image._relocations.push_back({relocation.Offset + bias, relocation.Type, relocation.Addend, symbol.Name, defined, symbol.Type == SymbolObject, defined ? symbol.Value + bias : 0});
            } else {
                throw std::runtime_error("Unsupported relocation type " + std::to_string(relocation.Type) + " at 0x" + Hex(relocation.Offset + bias) + " in " + path);
            }
        }
        image._importCount = imported.size();
    }

    for (const auto& region : Relinker::UnusedNidFilter::ReadExceptionFunctions(bytes, programs, relocated, importSlots)) {
        image._exceptionStarts.insert(region.Begin + bias);
        for (const auto target : region.ExtraTargets) {
            if (target >= region.Begin && target < region.End) image._landingPads[region.Begin + bias].insert(target + bias);
        }
    }
    return image;
}

void GuestImage::Relocate(const Resolver& resolve) {
    auto nextImport = AlignUp(_mappedEnd, ImportAlignment);
    std::map<std::string, std::uint64_t> importByName;
    for (const auto& relocation : _relocations) {
        std::uint64_t value = relocation.Value;
        if (relocation.Type != RelocationRelative && !relocation.Defined) {
            if (const auto definition = resolve(relocation.Symbol)) {
                value = *definition;
            } else if (relocation.Object) {
                _dataImports.push_back({relocation.Offset, relocation.Symbol, relocation.Type == RelocationAbsolute ? relocation.Addend : 0});
                Write64(relocation.Offset, 0);
                continue;
            } else {
                auto [found, inserted] = importByName.emplace(relocation.Symbol, nextImport);
                if (inserted) {
                    _imports.emplace(nextImport, relocation.Symbol);
                    nextImport += ImportStride;
                }
                value = found->second;
                if (relocation.Addend == 0) _importSlots.emplace(relocation.Offset, value);
            }
        }
        if (relocation.Type == RelocationAbsolute) value += static_cast<std::uint64_t>(relocation.Addend);
        Write64(relocation.Offset, value);
        if (IsExecutable(value)) _codePointers.insert(value);
    }
    _relocations.clear();
}

const std::string& GuestImage::Path() const {
    return _path;
}

const std::vector<std::string>& GuestImage::Needed() const {
    return _needed;
}

std::uint64_t GuestImage::End() const {
    return AlignUp(_mappedEnd, ImportAlignment) + _importCount * ImportStride;
}

std::optional<std::uint64_t> GuestImage::Export(const std::string& name) const {
    const auto found = _exports.find(name);
    if (found == _exports.end()) return std::nullopt;
    return found->second;
}

std::uint64_t GuestImage::Init() const {
    return _init;
}

const GuestArray& GuestImage::InitArray() const {
    return _initArray;
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
