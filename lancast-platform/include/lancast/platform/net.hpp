#pragma once

// Sockets IPv4 (Winsock) para LAN: TCP con tramas, UDP y utilidades de direcciones.

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lancast/platform/error.hpp"

namespace lancast::net {

struct Endpoint {
    std::uint32_t ip = 0;  // Orden de host (192.168.1.2 = 0xC0A80102).
    std::uint16_t port = 0;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string ip_string() const;
    // "192.168.1.2", "192.168.1.2:47984" o nombre de equipo (se resuelve por DNS/NetBIOS).
    static std::optional<Endpoint> parse(std::string_view text, std::uint16_t default_port);
    [[nodiscard]] bool is_private() const noexcept;  // RFC1918, loopback o link-local.
    [[nodiscard]] bool is_loopback() const noexcept { return (ip >> 24) == 127; }
    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

// Inicializa Winsock 2.2 durante la vida del objeto (crear uno en main).
class WinsockScope {
public:
    WinsockScope();
    ~WinsockScope();
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

class Socket {
public:
    Socket() = default;
    explicit Socket(std::uintptr_t s) noexcept : s_(s) {}
    ~Socket() { close(); }
    Socket(Socket&& o) noexcept : s_(o.release()) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            s_ = o.release();
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uintptr_t handle() const noexcept { return s_; }
    std::uintptr_t release() noexcept;
    void close() noexcept;
    // Desbloquea operaciones pendientes en otros hilos (recv/accept devuelven error).
    void shutdown() noexcept;

private:
    std::uintptr_t s_ = ~std::uintptr_t{0};
};

class TcpStream {
public:
    TcpStream() = default;
    explicit TcpStream(Socket s) : socket_(std::move(s)) {}

    static platform::Result<TcpStream> connect(const Endpoint& to, std::uint32_t timeout_ms);

    bool send_all(std::span<const std::uint8_t> data);
    bool recv_exact(std::span<std::uint8_t> out);
    // Lo que haya disponible (bloquea hasta al menos 1 byte). 0 = cerrado o error.
    std::size_t recv_some(std::span<std::uint8_t> out);
    // Trama [u32 len][datos]. recv_frame devuelve false al cerrar o si len > max.
    bool send_frame(std::span<const std::uint8_t> data);
    bool recv_frame(std::vector<std::uint8_t>& out, std::size_t max_size);

    void set_nodelay(bool on);
    void set_buffers(int bytes);
    void set_recv_timeout(std::uint32_t ms);
    [[nodiscard]] std::optional<Endpoint> peer() const;
    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
    void shutdown() noexcept { socket_.shutdown(); }
    void close() noexcept { socket_.close(); }

private:
    Socket socket_;
};

class TcpListener {
public:
    static platform::Result<TcpListener> bind(std::uint16_t port);
    // Espera hasta timeout_ms; nullopt si no hubo conexión.
    std::optional<TcpStream> accept(std::uint32_t timeout_ms, Endpoint* peer = nullptr);
    void close() noexcept { socket_.close(); }
    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_;
};

class UdpSocket {
public:
    static platform::Result<UdpSocket> bind(std::uint16_t port, bool reuse = false);

    bool send_to(const Endpoint& to, std::span<const std::uint8_t> data);
    // Varios datagramas de `segment` bytes concatenados en una sola llamada (UDP Send Offload de Windows:
    // el kernel/la tarjeta los separa). Si el sistema no lo soporta, se envían uno a uno.
    bool send_batch(const Endpoint& to, std::span<const std::uint8_t> data, std::uint32_t segment);
    // Devuelve bytes recibidos, 0 si timeout, nullopt si error/cerrado.
    std::optional<std::size_t> recv_from(std::span<std::uint8_t> buffer, Endpoint& from, std::uint32_t timeout_ms);
    void set_broadcast(bool on);
    void set_buffers(int bytes);
    [[nodiscard]] std::uint16_t local_port() const;
    void shutdown() noexcept { socket_.shutdown(); }
    void close() noexcept { socket_.close(); }
    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_;
    bool uso_supported_ = true;
};

struct LocalAddress {
    std::uint32_t ip = 0;
    std::uint32_t broadcast = 0;
    std::string adapter;
    bool has_gateway = false;  // Adaptador de la LAN real (los virtuales de Hyper-V/WSL no tienen).
};
// IPv4 de los adaptadores activos (excluye loopback), primero los que tienen puerta de enlace.
[[nodiscard]] std::vector<LocalAddress> local_addresses();

[[nodiscard]] std::string computer_name();

}  // namespace lancast::net
