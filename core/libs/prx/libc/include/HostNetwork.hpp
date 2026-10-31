#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <cerrno>
#include <cstddef>
#include <cstring>

extern "C" {
bool HostNetworkEnabled_nid_no_patch();
bool HostNetworkReaches_nid_no_patch(const sockaddr* address);
bool HostNetworkOpen_nid_no_patch();
int HostNetworkGetAddrInfo_nid_no_patch(const char* node, const char* service, const addrinfo* hints, addrinfo** result);
hostent* HostNetworkGetHostByName_nid_no_patch(const char* name);
#ifdef __linux__
int HostNetworkGetHostByNameR_nid_no_patch(const char* name, hostent* entry, char* buffer, std::size_t size, hostent** result, int* hostError);
#endif
}

namespace HostNetwork {

inline bool Enabled() {
    return HostNetworkEnabled_nid_no_patch();
}

inline bool Loopback(const sockaddr* address) {
    if (address->sa_family == AF_INET) return (ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) >> 24) == 127;
    if (address->sa_family != AF_INET6) return false;
    const auto& ip = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
    return IN6_IS_ADDR_LOOPBACK(&ip) || (IN6_IS_ADDR_V4MAPPED(&ip) && ip.s6_addr[12] == 127);
}

inline bool Unspecified(const sockaddr* address) {
    if (address->sa_family == AF_INET) return reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr == htonl(INADDR_ANY);
    return address->sa_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
}

inline void SetError(const int error) {
#ifdef _WIN32
    WSASetLastError(error == ENETUNREACH ? WSAENETUNREACH : WSAEADDRNOTAVAIL);
#else
    errno = error;
#endif
}

template <typename Socket, typename Length>
int Connect(const Socket socket, const sockaddr* address, const Length length) {
    if (!HostNetworkReaches_nid_no_patch(address)) {
        SetError(ENETUNREACH);
        return -1;
    }
    return ::connect(socket, address, length);
}

template <typename Socket, typename Buffer, typename Size, typename Length>
auto SendTo(const Socket socket, const Buffer buffer, const Size size, const int flags, const sockaddr* address, const Length length) -> decltype(::sendto(socket, buffer, size, flags, address, length)) {
    if (address != nullptr && !HostNetworkReaches_nid_no_patch(address)) {
        SetError(ENETUNREACH);
        return -1;
    }
    return ::sendto(socket, buffer, size, flags, address, length);
}

template <typename Socket, typename Length>
int Bind(const Socket socket, const sockaddr* address, const Length length) {
    if (HostNetworkOpen_nid_no_patch() || Loopback(address)) return ::bind(socket, address, length);
    sockaddr_storage local{};
    if (address->sa_family == AF_INET && static_cast<std::size_t>(length) >= sizeof(sockaddr_in)) {
        if (!Unspecified(address)) {
            SetError(EADDRNOTAVAIL);
            return -1;
        }
        std::memcpy(&local, address, sizeof(sockaddr_in));
        reinterpret_cast<sockaddr_in&>(local).sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::bind(socket, reinterpret_cast<const sockaddr*>(&local), length);
    }
    if (address->sa_family == AF_INET6 && static_cast<std::size_t>(length) >= sizeof(sockaddr_in6)) {
        if (!Unspecified(address)) {
            SetError(EADDRNOTAVAIL);
            return -1;
        }
        std::memcpy(&local, address, sizeof(sockaddr_in6));
        reinterpret_cast<sockaddr_in6&>(local).sin6_addr = in6addr_loopback;
        return ::bind(socket, reinterpret_cast<const sockaddr*>(&local), length);
    }
    return ::bind(socket, address, length);
}

inline int GetAddrInfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result) {
    return HostNetworkGetAddrInfo_nid_no_patch(node, service, hints, result);
}

inline hostent* GetHostByName(const char* name) {
    return HostNetworkGetHostByName_nid_no_patch(name);
}

#ifdef __linux__
inline int GetHostByNameR(const char* name, hostent* entry, char* buffer, const std::size_t size, hostent** result, int* hostError) {
    return HostNetworkGetHostByNameR_nid_no_patch(name, entry, buffer, size, result, hostError);
}
#endif

template <typename Length, typename HostLength, typename ServiceLength>
int GetNameInfo(const sockaddr* address, const Length length, char* host, const HostLength hostLength, char* service, const ServiceLength serviceLength, const int flags) {
    if (HostNetworkOpen_nid_no_patch() || host == nullptr || (flags & NI_NUMERICHOST) || Loopback(address)) return ::getnameinfo(address, length, host, hostLength, service, serviceLength, flags);
    if (flags & NI_NAMEREQD) return Enabled() ? EAI_NONAME : EAI_AGAIN;
    return ::getnameinfo(address, length, host, hostLength, service, serviceLength, flags | NI_NUMERICHOST);
}

}
