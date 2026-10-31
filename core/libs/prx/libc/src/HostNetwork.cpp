#include "prx/libc/include/HostNetwork.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Address = std::array<std::uint8_t, 16>;

struct Policy {
    bool enabled = true;
    std::vector<std::string> allowed;
    std::vector<std::string> denied;
};

struct Resolved {
    std::mutex lock;
    std::set<Address> addresses;
};

std::string Lowered(std::string_view name) {
    while (!name.empty() && name.back() == '.') name.remove_suffix(1);
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lowered;
}

std::vector<std::string> Domains(const char* variable, const char* value) {
    std::vector<std::string> domains;
    if (value == nullptr) return domains;
    std::string_view rest(value);
    while (true) {
        const auto comma = rest.find(',');
        const auto domain = Lowered(rest.substr(0, comma));
        if (domain.empty() || domain.find_first_of(" \t/:") != std::string::npos) throw std::runtime_error(std::string(variable) + " must list domain names separated by commas, not " + value);
        domains.push_back(domain);
        if (comma == std::string_view::npos) return domains;
        rest.remove_prefix(comma + 1);
    }
}

const Policy& Configured() {
    static const Policy policy = [] {
        Policy read;
        const char* network = std::getenv("APS5_NETWORK");
        if (network != nullptr && std::string_view(network) != "0" && std::string_view(network) != "1") throw std::runtime_error(std::string("APS5_NETWORK must be 0 or 1, not ") + network);
        read.enabled = network == nullptr || std::string_view(network) == "1";
        read.allowed = Domains("APS5_NETWORK_ALLOW", std::getenv("APS5_NETWORK_ALLOW"));
        read.denied = Domains("APS5_NETWORK_DENY", std::getenv("APS5_NETWORK_DENY"));
        if (!read.enabled && (!read.allowed.empty() || !read.denied.empty())) throw std::runtime_error("APS5_NETWORK_ALLOW and APS5_NETWORK_DENY need networking, which APS5_NETWORK=0 turns off");
        return read;
    }();
    return policy;
}

Resolved& Remembered() {
    static Resolved resolved;
    return resolved;
}

bool Matches(const std::string& name, const std::vector<std::string>& domains) {
    return std::any_of(domains.begin(), domains.end(), [&](const std::string& domain) {
        return name == domain || (name.size() > domain.size() && name.ends_with(domain) && name[name.size() - domain.size() - 1] == '.');
    });
}

bool Permits(const char* name) {
    const auto& policy = Configured();
    const auto lowered = Lowered(name);
    return !Matches(lowered, policy.denied) && (policy.allowed.empty() || Matches(lowered, policy.allowed));
}

bool Restricted() {
    return !Configured().allowed.empty();
}

Address Key(const sockaddr* address) {
    Address key{};
    if (address->sa_family == AF_INET) {
        key[10] = key[11] = 0xff;
        std::memcpy(key.data() + 12, &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, 4);
    } else {
        std::memcpy(key.data(), &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr, 16);
    }
    return key;
}

void Remember(const addrinfo* list) {
    auto& resolved = Remembered();
    std::lock_guard guard(resolved.lock);
    for (auto* item = list; item; item = item->ai_next) {
        if (item->ai_addr && (item->ai_family == AF_INET || item->ai_family == AF_INET6)) resolved.addresses.insert(Key(item->ai_addr));
    }
}

void Remember(const hostent* entry) {
    if (entry->h_addrtype != AF_INET || entry->h_length != 4) return;
    auto& resolved = Remembered();
    std::lock_guard guard(resolved.lock);
    for (auto** address = entry->h_addr_list; *address; ++address) {
        Address key{};
        key[10] = key[11] = 0xff;
        std::memcpy(key.data() + 12, *address, 4);
        resolved.addresses.insert(key);
    }
}

bool Numeric(const char* name) {
    in_addr address{};
    return inet_pton(AF_INET, name, &address) == 1;
}

void SetHostError(const int error) {
#ifdef _WIN32
    WSASetLastError(error == TRY_AGAIN ? WSATRY_AGAIN : WSAHOST_NOT_FOUND);
#else
    h_errno = error;
#endif
}

}

bool HostNetworkEnabled_nid_no_patch() {
    return Configured().enabled;
}

bool HostNetworkOpen_nid_no_patch() {
    return Configured().enabled && !Restricted();
}

bool HostNetworkReaches_nid_no_patch(const sockaddr* address) {
    if ((address->sa_family != AF_INET && address->sa_family != AF_INET6) || HostNetwork::Loopback(address) || HostNetwork::Unspecified(address)) return true;
    if (!Configured().enabled) return false;
    if (!Restricted()) return true;
    auto& resolved = Remembered();
    std::lock_guard guard(resolved.lock);
    return resolved.addresses.contains(Key(address));
}

int HostNetworkGetAddrInfo_nid_no_patch(const char* node, const char* service, const addrinfo* hints, addrinfo** result) {
    if (node == nullptr || std::string_view(node) == "localhost" || (hints != nullptr && (hints->ai_flags & AI_NUMERICHOST))) return ::getaddrinfo(node, service, hints, result);
    addrinfo numeric = hints != nullptr ? *hints : addrinfo{};
    numeric.ai_flags |= AI_NUMERICHOST;
    const int error = ::getaddrinfo(node, service, &numeric, result);
    if (error != EAI_NONAME) return error;
    if (!Configured().enabled) return EAI_AGAIN;
    if (!Permits(node)) return EAI_NONAME;
    const int found = ::getaddrinfo(node, service, hints, result);
    if (found == 0 && Restricted()) Remember(*result);
    return found;
}

hostent* HostNetworkGetHostByName_nid_no_patch(const char* name) {
    if (std::string_view(name) == "localhost" || Numeric(name)) return ::gethostbyname(name);
    if (!Configured().enabled || !Permits(name)) {
        SetHostError(Configured().enabled ? HOST_NOT_FOUND : TRY_AGAIN);
        return nullptr;
    }
    auto* entry = ::gethostbyname(name);
    if (entry != nullptr && Restricted()) Remember(entry);
    return entry;
}

#ifdef __linux__
int HostNetworkGetHostByNameR_nid_no_patch(const char* name, hostent* entry, char* buffer, const std::size_t size, hostent** result, int* hostError) {
    if (std::string_view(name) == "localhost" || Numeric(name)) return ::gethostbyname_r(name, entry, buffer, size, result, hostError);
    if (!Configured().enabled || !Permits(name)) {
        *result = nullptr;
        *hostError = Configured().enabled ? HOST_NOT_FOUND : TRY_AGAIN;
        return Configured().enabled ? 0 : EAGAIN;
    }
    const int error = ::gethostbyname_r(name, entry, buffer, size, result, hostError);
    if (error == 0 && *result != nullptr && Restricted()) Remember(*result);
    return error;
}
#endif
