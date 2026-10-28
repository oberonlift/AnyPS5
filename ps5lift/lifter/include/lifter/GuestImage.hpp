#ifndef PS5LIFT_GUESTIMAGE_HPP
#define PS5LIFT_GUESTIMAGE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Ps5Lift {

struct GuestTls {
    std::uint64_t Address = 0;
    std::uint64_t FileSize = 0;
    std::uint64_t MemorySize = 0;
    std::uint64_t Alignment = 1;
    std::uint64_t Module = 0;
    std::uint64_t Offset = 0;
};

struct GuestDataImport {
    std::uint64_t Slot;
    std::string Name;
    std::int64_t Addend;
};

struct GuestSegment {
    std::uint64_t Address;
    std::uint64_t MemorySize;
    std::vector<std::uint8_t> Bytes;
    std::uint32_t Flags;

    bool IsExecutable() const;
};

struct GuestArray {
    std::uint64_t Address = 0;
    std::uint64_t Count = 0;
};

struct GuestRelocation {
    std::uint64_t Offset;
    std::uint32_t Type;
    std::int64_t Addend;
    std::string Symbol;
    bool Defined;
    bool Object;
    std::uint64_t Value;
};

class GuestImage {
public:
    using Resolver = std::function<std::optional<std::uint64_t>(const std::string& name)>;

    static GuestImage Read(const std::string& path, std::optional<std::uint64_t> base);
    void PlaceTls(std::uint64_t module, std::uint64_t offset);
    void Relocate(const Resolver& resolve);

    const std::string& Path() const;
    const std::vector<std::string>& Needed() const;
    std::uint64_t End() const;
    std::optional<std::uint64_t> Export(const std::string& name) const;
    const std::map<std::string, std::uint64_t>& Exports() const;
    std::uint64_t Init() const;
    const GuestArray& InitArray() const;
    std::uint64_t Entry() const;
    const std::vector<GuestSegment>& Segments() const;
    std::span<const std::uint8_t> CodeAt(std::uint64_t address) const;
    std::optional<std::uint64_t> ReadData(std::uint64_t address, unsigned size) const;
    bool IsExecutable(std::uint64_t address) const;
    const std::map<std::uint64_t, std::string>& FunctionSymbols() const;
    const std::map<std::uint64_t, std::string>& Imports() const;
    std::optional<std::uint64_t> ImportAt(std::uint64_t slot) const;
    const std::set<std::uint64_t>& CodePointers() const;
    const std::set<std::uint64_t>& ExceptionFunctionStarts() const;
    const std::map<std::uint64_t, std::set<std::uint64_t>>& LandingPads() const;
    const std::vector<GuestDataImport>& DataImports() const;
    std::uint64_t EhFrameHeader() const;
    const GuestTls& Tls() const;

private:
    std::string _path;
    std::vector<std::string> _needed;
    std::map<std::string, std::uint64_t> _exports;
    std::vector<GuestRelocation> _relocations;
    std::uint64_t _mappedEnd = 0;
    std::uint64_t _importCount = 0;
    std::uint64_t _init = 0;
    GuestArray _initArray;
    std::uint64_t _entry = 0;
    std::vector<GuestSegment> _segments;
    std::map<std::uint64_t, std::string> _functionSymbols;
    std::map<std::uint64_t, std::string> _imports;
    std::map<std::uint64_t, std::uint64_t> _importSlots;
    std::set<std::uint64_t> _codePointers;
    std::set<std::uint64_t> _exceptionStarts;
    std::map<std::uint64_t, std::set<std::uint64_t>> _landingPads;
    std::vector<GuestDataImport> _dataImports;
    std::uint64_t _ehFrameHeader = 0;
    GuestTls _tls;

    void Write64(std::uint64_t address, std::uint64_t value);
};

}

#endif
