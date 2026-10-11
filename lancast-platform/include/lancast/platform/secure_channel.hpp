#pragma once

// Canal TCP cifrado con AES-256-GCM: cada trama lleva [u32 len][cifrado + tag]; el nonce es un contador
// de 64 bits por dirección (claves distintas por dirección), así que nunca se repite.

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "lancast/platform/crypto.hpp"
#include "lancast/platform/net.hpp"

namespace lancast::net {

class SecureChannel {
public:
    SecureChannel(TcpStream stream, std::span<const std::uint8_t> send_key, std::span<const std::uint8_t> recv_key);

    // Thread-safe (varios hilos pueden enviar).
    bool send(std::span<const std::uint8_t> plaintext);
    // Un solo hilo lector. Devuelve false si la conexión se cerró o la autenticación falló.
    bool recv(std::vector<std::uint8_t>& plaintext, std::size_t max_size);

    void shutdown() noexcept { stream_.shutdown(); }
    [[nodiscard]] TcpStream& stream() noexcept { return stream_; }

private:
    TcpStream stream_;
    std::mutex send_mutex_;
    crypto::AesGcm send_cipher_;
    crypto::AesGcm recv_cipher_;
    std::uint64_t send_counter_ = 0;
    std::uint64_t recv_counter_ = 0;
    std::vector<std::uint8_t> send_buf_;
    std::vector<std::uint8_t> recv_buf_;
};

// Cifrado de datagramas de medios. El nonce es la cabecera de 12 bytes del paquete (tipo + secuencia).
class DatagramCipher {
public:
    explicit DatagramCipher(std::span<const std::uint8_t> key) : cipher_(key) {}
    // header: 12 bytes; out: header + cifrado + tag (se escribe completo).
    bool seal(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out);
    bool open(std::span<const std::uint8_t> datagram, std::vector<std::uint8_t>& payload);

private:
    crypto::AesGcm cipher_;
};

}  // namespace lancast::net
