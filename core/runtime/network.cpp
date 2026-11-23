// Offline network policy.
//
// With the offline switch on (the default) the title process behaves like a
// PC whose network is disconnected: host names other than the local host do
// not resolve, connections and datagrams to anything but the loopback
// interface fail with "network unreachable", and HTTP requests fail with
// "name not resolved". These are the documented errors the host's own
// networking APIs report in that situation, so title code meets an ordinary
// offline machine, not an invented one. Loopback traffic is left alone.
//
// The interposers apply to the title image and to every DLL the package
// ships (see binder.cpp), which covers all code that came with the package.
#include "core/runtime/network.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstring>

#include "core/common/log.h"
#include "core/common/strings.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "network";
constexpr DWORD kWinHttpNameNotResolved = 12007;  // ERROR_WINHTTP_NAME_NOT_RESOLVED

FARPROC host_function(const wchar_t* library, const char* name) {
  HMODULE module = GetModuleHandleW(library);
  if (!module) module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module ? GetProcAddress(module, name) : nullptr;
}

template <typename Fn>
Fn ws2(const char* name) {
  return reinterpret_cast<Fn>(host_function(L"ws2_32.dll", name));
}

void set_wsa_error(int error) {
  using Fn = void(WSAAPI*)(int);
  if (const auto fn = ws2<Fn>("WSASetLastError")) fn(error);
  SetLastError(static_cast<DWORD>(error));
}

bool offline() {
  return state().offline;
}

std::atomic<int> g_log_budget{32};

void note_refusal(const char* what, const char* detail) {
  if (g_log_budget.fetch_sub(1) > 0) LOG_INFO(kComponent, "offline: refused %s%s%s", what, detail ? " " : "", detail ? detail : "");
}

bool is_loopback_name(const char* name) {
  if (!name) return true;  // a null node name means the local host
  return iequals(std::string_view(name), "localhost") || std::strncmp(name, "127.", 4) == 0 ||
         std::strcmp(name, "::1") == 0 || name[0] == '\0';
}

bool is_loopback_name(const wchar_t* name) {
  if (!name) return true;
  return is_loopback_name(narrow(name).c_str());
}

bool is_loopback_address(const sockaddr* address, int length) {
  if (!address) return false;
  if (address->sa_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in))) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
    // First octet of the address in network byte order.
    return (v4->sin_addr.s_addr & 0xFF) == 127;
  }
  if (address->sa_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6))) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
    static const uint8_t kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t kMappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(v6->sin6_addr.s6_addr, kLoopback, 16) == 0) return true;
    return std::memcmp(v6->sin6_addr.s6_addr, kMappedPrefix, 12) == 0 && v6->sin6_addr.s6_addr[12] == 127;
  }
  return false;
}

// --- name resolution ---------------------------------------------------------

INT WSAAPI hook_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
  if (offline() && !is_loopback_name(node)) {
    note_refusal("name resolution of", node);
    if (result) *result = nullptr;
    set_wsa_error(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
  }
  using Fn = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
  const auto fn = ws2<Fn>("getaddrinfo");
  return fn ? fn(node, service, hints, result) : WSANOTINITIALISED;
}

INT WSAAPI hook_GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
  if (offline() && !is_loopback_name(node)) {
    note_refusal("name resolution of", node ? narrow(node).c_str() : nullptr);
    if (result) *result = nullptr;
    set_wsa_error(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
  }
  using Fn = INT(WSAAPI*)(PCWSTR, PCWSTR, const ADDRINFOW*, PADDRINFOW*);
  const auto fn = ws2<Fn>("GetAddrInfoW");
  return fn ? fn(node, service, hints, result) : WSANOTINITIALISED;
}

hostent* WSAAPI hook_gethostbyname(const char* name) {
  if (offline() && !is_loopback_name(name)) {
    note_refusal("name resolution of", name);
    set_wsa_error(WSAHOST_NOT_FOUND);
    return nullptr;
  }
  using Fn = hostent*(WSAAPI*)(const char*);
  const auto fn = ws2<Fn>("gethostbyname");
  return fn ? fn(name) : nullptr;
}

// --- connections and datagrams ------------------------------------------------

int WSAAPI hook_connect(SOCKET socket, const sockaddr* address, int length) {
  if (offline() && !is_loopback_address(address, length)) {
    note_refusal("a connection", nullptr);
    set_wsa_error(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  using Fn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
  const auto fn = ws2<Fn>("connect");
  return fn ? fn(socket, address, length) : SOCKET_ERROR;
}

int WSAAPI hook_WSAConnect(SOCKET socket, const sockaddr* address, int length, LPWSABUF caller_data,
                           LPWSABUF callee_data, LPQOS sqos, LPQOS gqos) {
  if (offline() && !is_loopback_address(address, length)) {
    note_refusal("a connection", nullptr);
    set_wsa_error(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  using Fn = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
  const auto fn = ws2<Fn>("WSAConnect");
  return fn ? fn(socket, address, length, caller_data, callee_data, sqos, gqos) : SOCKET_ERROR;
}

int WSAAPI hook_sendto(SOCKET socket, const char* buffer, int length, int flags, const sockaddr* to, int to_length) {
  if (offline() && to && !is_loopback_address(to, to_length)) {
    note_refusal("a datagram", nullptr);
    set_wsa_error(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  using Fn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
  const auto fn = ws2<Fn>("sendto");
  return fn ? fn(socket, buffer, length, flags, to, to_length) : SOCKET_ERROR;
}

int WSAAPI hook_WSASendTo(SOCKET socket, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to,
                          int to_length, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
  if (offline() && to && !is_loopback_address(to, to_length)) {
    note_refusal("a datagram", nullptr);
    set_wsa_error(WSAENETUNREACH);
    return SOCKET_ERROR;
  }
  using Fn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE);
  const auto fn = ws2<Fn>("WSASendTo");
  return fn ? fn(socket, buffers, count, sent, flags, to, to_length, overlapped, routine) : SOCKET_ERROR;
}

// --- HTTP ----------------------------------------------------------------------
//
// A request is the point where the host's HTTP stack first needs the
// network; offline it fails there, synchronously, with the error the stack
// reports when the server name cannot be resolved. Requests to the local
// host are not distinguished: HTTP from a title is never loopback traffic.

BOOL WINAPI hook_WinHttpSendRequest(void* request, LPCWSTR headers, DWORD headers_length, LPVOID optional,
                                    DWORD optional_length, DWORD total_length, DWORD_PTR context) {
  if (offline()) {
    note_refusal("an HTTP request", nullptr);
    SetLastError(kWinHttpNameNotResolved);
    return FALSE;
  }
  using Fn = BOOL(WINAPI*)(void*, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
  const auto fn = reinterpret_cast<Fn>(host_function(L"winhttp.dll", "WinHttpSendRequest"));
  return fn ? fn(request, headers, headers_length, optional, optional_length, total_length, context) : FALSE;
}

#define HOLY_NETWORK_OVERRIDE(library, name)                         \
  Override {                                                         \
    #name, reinterpret_cast<void*>(&hook_##name), library            \
  }

const Override kNetworkOverrides[] = {
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", getaddrinfo),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", GetAddrInfoW),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", gethostbyname),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", connect),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", WSAConnect),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", sendto),
    HOLY_NETWORK_OVERRIDE("ws2_32.dll", WSASendTo),
    HOLY_NETWORK_OVERRIDE("winhttp.dll", WinHttpSendRequest),
};

#undef HOLY_NETWORK_OVERRIDE

}  // namespace

const Override* find_network_override(std::string_view library, std::string_view symbol) {
  for (const Override& entry : kNetworkOverrides) {
    if (symbol == entry.symbol && iequals(library, entry.library)) return &entry;
  }
  return nullptr;
}

}  // namespace holy::rt
