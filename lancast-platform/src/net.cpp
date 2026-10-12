#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <ws2ipdef.h>

#include "lancast/platform/net.hpp"

#include <algorithm>
#include <charconv>
#include <format>

#include "lancast/core/log.hpp"
#include "win.hpp"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace lancast::net {
namespace {

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = htonl(e.ip);
    return a;
}

Endpoint from_sockaddr(const sockaddr_in& a) { return Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)}; }

std::unexpected<platform::Error> wsa_error(const char* what) {
    return std::unexpected(platform::Error{what, HRESULT_FROM_WIN32(static_cast<DWORD>(WSAGetLastError()))});
}

SOCKET as_socket(std::uintptr_t s) { return static_cast<SOCKET>(s); }

bool wait_readable(SOCKET s, std::uint32_t timeout_ms) {
    WSAPOLLFD fd{};
    fd.fd = s;
    fd.events = POLLRDNORM;
    const int r = WSAPoll(&fd, 1, static_cast<INT>(timeout_ms));
    return r > 0;
}

}  // namespace

std::string Endpoint::ip_string() const {
    return std::format("{}.{}.{}.{}", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

std::string Endpoint::to_string() const { return std::format("{}:{}", ip_string(), port); }

std::optional<Endpoint> Endpoint::parse(std::string_view text, std::uint16_t default_port) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (text.empty()) return std::nullopt;

    std::uint16_t port = default_port;
    std::string_view host = text;
    if (const auto colon = text.rfind(':'); colon != std::string_view::npos) {
        host = text.substr(0, colon);
        const auto p = text.substr(colon + 1);
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), value);
        if (ec != std::errc{} || ptr != p.data() + p.size() || value == 0 || value > 65535) return std::nullopt;
        port = static_cast<std::uint16_t>(value);
    }

    const std::string host_str{host};
    in_addr addr{};
    if (inet_pton(AF_INET, host_str.c_str(), &addr) == 1) return Endpoint{ntohl(addr.s_addr), port};

    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(host_str.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) return std::nullopt;
    const Endpoint e{ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr), port};
    freeaddrinfo(result);
    return e;
}

bool Endpoint::is_private() const noexcept {
    const std::uint32_t a = ip >> 24;
    const std::uint32_t b = (ip >> 16) & 0xFF;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || a == 127 ||
           (a == 169 && b == 254);
}

WinsockScope::WinsockScope() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
}

WinsockScope::~WinsockScope() { WSACleanup(); }

bool Socket::valid() const noexcept { return as_socket(s_.load()) != INVALID_SOCKET; }

std::uintptr_t Socket::release() noexcept { return s_.exchange(static_cast<std::uintptr_t>(INVALID_SOCKET)); }

void Socket::close() noexcept {
    const std::uintptr_t s = s_.exchange(static_cast<std::uintptr_t>(INVALID_SOCKET));
    if (as_socket(s) != INVALID_SOCKET) closesocket(as_socket(s));
}

void Socket::shutdown() noexcept {
    const std::uintptr_t s = s_.exchange(static_cast<std::uintptr_t>(INVALID_SOCKET));
    if (as_socket(s) == INVALID_SOCKET) return;
    ::shutdown(as_socket(s), SD_BOTH);  // Avisa al otro extremo (FIN)...
    closesocket(as_socket(s));          // ...y despierta a los hilos bloqueados en este socket.
}

platform::Result<TcpStream> TcpStream::connect(const Endpoint& to, std::uint32_t timeout_ms) {
    Socket s{static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))};
    if (!s.valid()) return wsa_error("socket");

    u_long nonblocking = 1;
    ioctlsocket(as_socket(s.handle()), FIONBIO, &nonblocking);
    const sockaddr_in addr = to_sockaddr(to);
    ::connect(as_socket(s.handle()), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    WSAPOLLFD fd{};
    fd.fd = as_socket(s.handle());
    fd.events = POLLWRNORM;
    const int r = WSAPoll(&fd, 1, static_cast<INT>(timeout_ms));
    if (r <= 0) return std::unexpected(platform::Error{"Tiempo de conexión agotado", r == 0 ? HRESULT_FROM_WIN32(WSAETIMEDOUT) : 0});
    if ((fd.revents & (POLLERR | POLLHUP)) != 0) {
        int err = 0;
        int len = sizeof(err);
        getsockopt(as_socket(s.handle()), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        return std::unexpected(platform::Error{"No se pudo conectar", HRESULT_FROM_WIN32(static_cast<DWORD>(err))});
    }
    nonblocking = 0;
    ioctlsocket(as_socket(s.handle()), FIONBIO, &nonblocking);

    TcpStream stream{std::move(s)};
    stream.set_nodelay(true);
    return stream;
}

bool TcpStream::send_all(std::span<const std::uint8_t> data) {
    while (!data.empty()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(data.size(), 1u << 30));
        const int n = ::send(as_socket(socket_.handle()), reinterpret_cast<const char*>(data.data()), chunk, 0);
        if (n <= 0) return false;
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

bool TcpStream::recv_exact(std::span<std::uint8_t> out) {
    while (!out.empty()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(out.size(), 1u << 30));
        const int n = ::recv(as_socket(socket_.handle()), reinterpret_cast<char*>(out.data()), chunk, 0);
        if (n <= 0) return false;
        out = out.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

std::size_t TcpStream::recv_some(std::span<std::uint8_t> out) {
    const int n = ::recv(as_socket(socket_.handle()), reinterpret_cast<char*>(out.data()),
                         static_cast<int>(std::min<std::size_t>(out.size(), 1u << 30)), 0);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

bool TcpStream::send_frame(std::span<const std::uint8_t> data) {
    const auto len = static_cast<std::uint32_t>(data.size());
    const std::uint8_t header[4] = {static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8),
                                    static_cast<std::uint8_t>(len >> 16), static_cast<std::uint8_t>(len >> 24)};
    // Una sola llamada para tramas pequeñas (evita dos segmentos TCP con NODELAY).
    if (data.size() <= 16 * 1024) {
        std::uint8_t buf[16 * 1024 + 4];
        std::memcpy(buf, header, 4);
        std::memcpy(buf + 4, data.data(), data.size());
        return send_all({buf, data.size() + 4});
    }
    return send_all(header) && send_all(data);
}

bool TcpStream::recv_frame(std::vector<std::uint8_t>& out, std::size_t max_size) {
    std::uint8_t header[4];
    if (!recv_exact(header)) return false;
    const std::uint32_t len = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
    if (len > max_size) return false;
    out.resize(len);
    return recv_exact(out);
}

void TcpStream::set_nodelay(bool on) {
    const BOOL v = on ? TRUE : FALSE;
    setsockopt(as_socket(socket_.handle()), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof(v));
}

void TcpStream::set_buffers(int bytes) {
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

void TcpStream::set_recv_timeout(std::uint32_t ms) {
    const DWORD v = ms;
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
}

std::optional<Endpoint> TcpStream::peer() const {
    sockaddr_in a{};
    int len = sizeof(a);
    if (getpeername(as_socket(socket_.handle()), reinterpret_cast<sockaddr*>(&a), &len) != 0) return std::nullopt;
    return from_sockaddr(a);
}

platform::Result<TcpListener> TcpListener::bind(std::uint16_t port) {
    TcpListener l;
    l.socket_ = Socket{static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))};
    if (!l.socket_.valid()) return wsa_error("socket");
    const BOOL exclusive = TRUE;
    setsockopt(as_socket(l.socket_.handle()), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    const sockaddr_in addr = to_sockaddr(Endpoint{INADDR_ANY, port});
    if (::bind(as_socket(l.socket_.handle()), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return wsa_error("No se pudo abrir el puerto TCP (¿otra instancia de LanCast?)");
    }
    if (::listen(as_socket(l.socket_.handle()), 8) != 0) return wsa_error("listen");
    return l;
}

std::optional<TcpStream> TcpListener::accept(std::uint32_t timeout_ms, Endpoint* peer) {
    if (!socket_.valid() || !wait_readable(as_socket(socket_.handle()), timeout_ms)) return std::nullopt;
    sockaddr_in a{};
    int len = sizeof(a);
    const SOCKET s = ::accept(as_socket(socket_.handle()), reinterpret_cast<sockaddr*>(&a), &len);
    if (s == INVALID_SOCKET) return std::nullopt;
    if (peer != nullptr) *peer = from_sockaddr(a);
    TcpStream stream{Socket{static_cast<std::uintptr_t>(s)}};
    stream.set_nodelay(true);
    return stream;
}

platform::Result<UdpSocket> UdpSocket::bind(std::uint16_t port, bool reuse) {
    UdpSocket u;
    u.socket_ = Socket{static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))};
    if (!u.socket_.valid()) return wsa_error("socket UDP");
    if (reuse) {
        const BOOL v = TRUE;
        setsockopt(as_socket(u.socket_.handle()), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&v), sizeof(v));
    }
    // Evita que un ICMP "puerto inalcanzable" haga fallar los recv posteriores (WSAECONNRESET).
    BOOL no_reset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(as_socket(u.socket_.handle()), SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), nullptr, 0, &bytes, nullptr,
             nullptr);
    const sockaddr_in addr = to_sockaddr(Endpoint{INADDR_ANY, port});
    if (::bind(as_socket(u.socket_.handle()), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return wsa_error("No se pudo abrir el puerto UDP");
    }
    return u;
}

bool UdpSocket::send_to(const Endpoint& to, std::span<const std::uint8_t> data) {
    const sockaddr_in addr = to_sockaddr(to);
    return ::sendto(as_socket(socket_.handle()), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                    0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == static_cast<int>(data.size());
}

std::optional<std::size_t> UdpSocket::recv_from(std::span<std::uint8_t> buffer, Endpoint& from, std::uint32_t timeout_ms) {
    if (!socket_.valid()) return std::nullopt;
    if (timeout_ms != INFINITE && !wait_readable(as_socket(socket_.handle()), timeout_ms)) {
        return socket_.valid() ? std::optional<std::size_t>{0} : std::nullopt;
    }
    sockaddr_in a{};
    int len = sizeof(a);
    const int n = ::recvfrom(as_socket(socket_.handle()), reinterpret_cast<char*>(buffer.data()),
                             static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n < 0) {
        const int err = WSAGetLastError();
        if (err == WSAEMSGSIZE || err == WSAECONNRESET) return 0;
        return std::nullopt;
    }
    from = from_sockaddr(a);
    return static_cast<std::size_t>(n);
}

bool UdpSocket::send_batch(const Endpoint& to, std::span<const std::uint8_t> data, std::uint32_t segment) {
    if (data.size() <= segment) return send_to(to, data);
#ifdef UDP_SEND_MSG_SIZE
    if (uso_supported_) {
        sockaddr_in addr = to_sockaddr(to);
        WSABUF buf{static_cast<ULONG>(data.size()), reinterpret_cast<CHAR*>(const_cast<std::uint8_t*>(data.data()))};
        alignas(WSACMSGHDR) char control[WSA_CMSG_SPACE(sizeof(DWORD))]{};
        WSAMSG msg{};
        msg.name = reinterpret_cast<sockaddr*>(&addr);
        msg.namelen = sizeof(addr);
        msg.lpBuffers = &buf;
        msg.dwBufferCount = 1;
        msg.Control.buf = control;
        msg.Control.len = sizeof(control);
        WSACMSGHDR* cm = WSA_CMSG_FIRSTHDR(&msg);
        cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        cm->cmsg_level = IPPROTO_UDP;
        cm->cmsg_type = UDP_SEND_MSG_SIZE;
        *reinterpret_cast<DWORD*>(WSA_CMSG_DATA(cm)) = segment;
        DWORD sent = 0;
        if (WSASendMsg(as_socket(socket_.handle()), &msg, 0, &sent, nullptr, nullptr) == 0) return true;
        const int err = WSAGetLastError();
        if (err != WSAEINVAL && err != WSAEOPNOTSUPP && err != WSAENOPROTOOPT) return false;
        uso_supported_ = false;  // Windows antiguo o adaptador sin USO: envío clásico.
        log::info("UDP Send Offload no disponible (error {}): envío paquete a paquete", err);
    }
#endif
    bool ok = true;
    for (std::size_t off = 0; off < data.size(); off += segment) {
        ok = send_to(to, data.subspan(off, std::min<std::size_t>(segment, data.size() - off))) && ok;
    }
    return ok;
}

void UdpSocket::set_broadcast(bool on) {
    const BOOL v = on ? TRUE : FALSE;
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v), sizeof(v));
}

void UdpSocket::set_buffers(int bytes) {
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    setsockopt(as_socket(socket_.handle()), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

std::uint16_t UdpSocket::local_port() const {
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(as_socket(socket_.handle()), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

std::vector<LocalAddress> local_addresses() {
    std::vector<LocalAddress> out;
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const std::uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const std::uint32_t mask = u->OnLinkPrefixLength == 0 ? 0 : (~0u << (32 - u->OnLinkPrefixLength));
            LocalAddress la{ip, ip | ~mask, platform::detail::to_utf8(a->FriendlyName), a->FirstGatewayAddress != nullptr, mask, {}};
            if (a->PhysicalAddressLength == 6) std::copy_n(a->PhysicalAddress, 6, la.mac.begin());
            out.push_back(std::move(la));
        }
    }
    std::ranges::stable_sort(out, [](const LocalAddress& x, const LocalAddress& y) { return x.has_gateway > y.has_gateway; });
    return out;
}

std::string computer_name() {
    wchar_t name[256]{};
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &size)) return "PC";
    return platform::detail::to_utf8(name);
}

}  // namespace lancast::net
