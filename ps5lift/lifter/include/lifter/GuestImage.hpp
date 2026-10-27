#ifndef PS5LIFT_GUESTIMAGE_HPP
#define PS5LIFT_GUESTIMAGE_HPP

#include <cstdint>
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
};

struct GuestSegment {
    std::uint64_t Address;
    std::uint64_t MemorySize;
    std::vector<std::uint8_t> Bytes;
    std::uint32_t Flags;
};

class GuestImage {
public:
    static GuestImage Load(const std::string& path);

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
    const GuestTls& Tls() const;

private:
    std::uint64_t _entry = 0;
    std::vector<GuestSegment> _segments;
    std::map<std::uint64_t, std::string> _functionSymbols;
    std::map<std::uint64_t, std::string> _imports;
    std::map<std::uint64_t, std::uint64_t> _importSlots;
    std::set<std::uint64_t> _codePointers;
    std::set<std::uint64_t> _exceptionStarts;
    GuestTls _tls;

    void Write64(std::uint64_t address, std::uint64_t value);
};

}

#endif
