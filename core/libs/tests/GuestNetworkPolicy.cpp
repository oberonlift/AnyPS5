#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/Api.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libSceNetCtl/include/NetCtl.hpp"
#include "prx/libScePosixForWebKit/include/Api.hpp"
#include "prx/libScePosixForWebKit/GuestResolver.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

constexpr int GuestInet = 2;
constexpr int GuestStream = 1;
constexpr int GuestDatagram = 2;
constexpr int GuestNetworkUnreachable = 51;
constexpr int GuestAddressNotAvailable = 49;
constexpr int GuestLookupAgain = 2;
constexpr int GuestNoName = 8;
constexpr std::uint32_t GuestLoopbackFlag = 0x8;
constexpr int NetCtlDisconnected = 0;
constexpr int NetCtlInfoIpAddress = 14;
constexpr int NetCtlNotConnected = static_cast<int>(0x80412108);

void Require(const bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "network policy check failed: %s\n", what);
    std::abort();
}

std::array<std::uint8_t, 16> Ipv4(const std::uint8_t a, const std::uint8_t b, const std::uint8_t c, const std::uint8_t d, const std::uint16_t port) {
    return {16, GuestInet, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port), a, b, c, d};
}

void ConsoleHasNoConnection() {
    Require(sceNetCtlInit() == 0, "sceNetCtlInit");
    int state = -1;
    Require(sceNetCtlGetState(&state) == 0 && state == NetCtlDisconnected, "the state is not disconnected");
    NetCtlInfo info{};
    Require(sceNetCtlGetInfo(NetCtlInfoIpAddress, &info) == NetCtlNotConnected, "an IP address is reported");
}

void OnlyLoopbackInterfacesAreListed() {
    GuestInterfaceAddress* list = nullptr;
    Require(getifaddrs_nid_postfix(&list) == 0 && list != nullptr, "getifaddrs lists nothing");
    for (auto* entry = list; entry; entry = entry->next) Require((entry->flags & GuestLoopbackFlag) != 0, "an interface other than loopback is listed");
    freeifaddrs_nid_postfix(list);
}

void OtherAddressesAreUnreachable() {
    const auto documentation = Ipv4(192, 0, 2, 1, 80);
    const int stream = socket_nid_postfix(GuestInet, GuestStream, 0);
    Require(stream >= 0, "stream socket");
    Require(connect_nid_postfix(stream, documentation.data(), documentation.size()) == -1 && *__error_nid_postfix() == GuestNetworkUnreachable,
        "a connection to another address did not fail with ENETUNREACH");
    Require(close_nid_postfix(stream) == 0, "close");
    const int datagram = socket_nid_postfix(GuestInet, GuestDatagram, 0);
    Require(datagram >= 0, "datagram socket");
    const char byte = 0;
    Require(sendto_nid_postfix(datagram, &byte, 1, 0, documentation.data(), documentation.size()) == -1 && *__error_nid_postfix() == GuestNetworkUnreachable,
        "a datagram to another address did not fail with ENETUNREACH");
    Require(close_nid_postfix(datagram) == 0, "close");
}

void WildcardBindsToLoopback() {
    const int listener = socket_nid_postfix(GuestInet, GuestStream, 0);
    Require(listener >= 0, "listener socket");
    auto any = Ipv4(0, 0, 0, 0, 0);
    Require(bind_nid_postfix(listener, any.data(), any.size()) == 0 && listen_nid_postfix(listener, 1) == 0, "a wildcard bind failed");
    std::uint32_t size = any.size();
    Require(getsockname_nid_postfix(listener, any.data(), &size) == 0 && any[4] == 127 && any[5] == 0 && any[6] == 0 && any[7] == 1,
        "a wildcard bind is not bound to loopback");
    const int client = socket_nid_postfix(GuestInet, GuestStream, 0);
    Require(client >= 0 && connect_nid_postfix(client, any.data(), any.size()) == 0, "loopback is unreachable");
    const int accepted = accept_nid_postfix(listener, nullptr, nullptr);
    Require(accepted >= 0, "accept");
    for (const int descriptor : {client, accepted, listener}) Require(close_nid_postfix(descriptor) == 0, "close");

    const int other = socket_nid_postfix(GuestInet, GuestStream, 0);
    const auto documentation = Ipv4(192, 0, 2, 1, 0);
    Require(other >= 0 && bind_nid_postfix(other, documentation.data(), documentation.size()) == -1 && *__error_nid_postfix() == GuestAddressNotAvailable,
        "a bind to another address did not fail with EADDRNOTAVAIL");
    Require(close_nid_postfix(other) == 0, "close");
}

void WildcardBindStaysWildcard() {
    const int socket = socket_nid_postfix(GuestInet, GuestStream, 0);
    Require(socket >= 0, "socket");
    auto any = Ipv4(0, 0, 0, 0, 0);
    Require(bind_nid_postfix(socket, any.data(), any.size()) == 0, "a wildcard bind failed");
    std::uint32_t size = any.size();
    Require(getsockname_nid_postfix(socket, any.data(), &size) == 0 && any[4] == 0 && any[5] == 0 && any[6] == 0 && any[7] == 0,
        "a wildcard bind with networking is not bound to every address");
    Require(close_nid_postfix(socket) == 0, "close");
}

int Lookup(const char* name) {
    GuestResolver::AddressInfo hints{};
    hints.family = GuestInet;
    hints.socketType = GuestStream;
    GuestResolver::AddressInfo* result = nullptr;
    const int error = getaddrinfo_nid_postfix(name, "80", &hints, &result);
    Require((error == 0) == (result != nullptr), "a lookup's result does not match its error");
    if (result != nullptr) freeaddrinfo_nid_postfix(result);
    return error;
}

void LocalHostsResolve() {
    Require(Lookup("localhost") == 0 && Lookup("192.0.2.1") == 0, "localhost or a numeric host did not resolve");
}

void Set(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, name);
#else
    Require(setenv(name, value, 1) == 0, name);
#endif
}

void Unset(const char* name) {
#ifdef _WIN32
    Require(_putenv_s(name, "") == 0, name);
#else
    Require(unsetenv(name) == 0, name);
#endif
}

}

int main(const int count, char** arguments) {
    const std::string_view mode = count > 1 ? arguments[1] : "offline";
    Require(mode == "offline" || mode == "online" || mode == "allow" || mode == "deny", "the mode is offline, online, allow or deny");
    for (const char* name : {"APS5_NETWORK", "APS5_NETWORK_ALLOW", "APS5_NETWORK_DENY"}) Unset(name);
    if (mode == "offline") Set("APS5_NETWORK", "0");
    if (mode == "allow") Set("APS5_NETWORK_ALLOW", "example.com");
    if (mode == "deny") Set("APS5_NETWORK_DENY", "Example.COM.,example.net");
    std::array<void*, 10> heap{};
    ApplicationHeapRegister_nid_no_patch(heap.data());
    LocalHostsResolve();
    if (mode == "offline") {
        ConsoleHasNoConnection();
        OnlyLoopbackInterfacesAreListed();
        OtherAddressesAreUnreachable();
        WildcardBindsToLoopback();
        Require(Lookup("example.com") == GuestLookupAgain, "a name resolved without networking");
    } else if (mode == "online") {
        WildcardBindStaysWildcard();
    } else if (mode == "allow") {
        OtherAddressesAreUnreachable();
        WildcardBindsToLoopback();
        for (const char* name : {"example.org", "notexample.com", "example.com.evil.org"}) Require(Lookup(name) == GuestNoName, "a name outside the allowed domains resolved");
    } else {
        for (const char* name : {"example.com", "WWW.example.com", "mail.example.net."}) Require(Lookup(name) == GuestNoName, "a denied name resolved");
    }
    std::printf("network policy checks passed (%.*s)\n", static_cast<int>(mode.size()), mode.data());
    return 0;
}
