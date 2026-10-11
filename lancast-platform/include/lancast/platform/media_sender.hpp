#pragma once

// Envío de datagramas de medios cifrados (video/audio) a un endpoint UDP. Thread-safe: cada hilo
// usa su propio cifrador y la secuencia (que es el nonce) es un contador atómico compartido.

#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "lancast/core/media.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/secure_channel.hpp"

namespace lancast::net {

class MediaSender {
public:
    MediaSender(UdpSocket& socket, std::span<const std::uint8_t> key) : socket_(socket), key_(key.begin(), key.end()) {}

    void set_endpoint(const Endpoint& e) {
        std::scoped_lock lock(mutex_);
        endpoint_ = e;
    }
    [[nodiscard]] std::optional<Endpoint> endpoint() const {
        std::scoped_lock lock(mutex_);
        return endpoint_;
    }

    // Cifrador por hilo (crear uno en cada hilo emisor).
    [[nodiscard]] std::unique_ptr<DatagramCipher> make_cipher() const { return std::make_unique<DatagramCipher>(key_); }

    bool send(DatagramCipher& cipher, media::PacketType type, std::span<const std::uint8_t> payload,
              std::vector<std::uint8_t>& scratch) {
        const auto ep = endpoint();
        if (!ep) return false;
        std::uint8_t header[media::kPacketHeaderSize];
        media::PacketHeader{type, seq_.fetch_add(1)}.write(header);
        if (!cipher.seal(header, payload, scratch)) return false;
        bytes_sent_.fetch_add(scratch.size());
        return socket_.send_to(*ep, scratch);
    }

    // Lote de datagramas del mismo tamaño enviados con una sola llamada (ver UdpSocket::send_batch).
    class Batch {
    public:
        explicit Batch(MediaSender& sender) : sender_(sender) {}
        ~Batch() { flush(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        void add(DatagramCipher& cipher, media::PacketType type, std::span<const std::uint8_t> payload) {
            std::uint8_t header[media::kPacketHeaderSize];
            media::PacketHeader{type, sender_.seq_.fetch_add(1)}.write(header);
            if (!cipher.seal(header, payload, scratch_)) return;
            if (!buffer_.empty() && (scratch_.size() != segment_ || buffer_.size() + scratch_.size() > kMaxBatch)) flush();
            segment_ = static_cast<std::uint32_t>(scratch_.size());
            buffer_.insert(buffer_.end(), scratch_.begin(), scratch_.end());
        }

        void flush() {
            if (buffer_.empty()) return;
            if (const auto ep = sender_.endpoint()) {
                sender_.bytes_sent_.fetch_add(buffer_.size());
                sender_.socket_.send_batch(*ep, buffer_, segment_);
            }
            buffer_.clear();
        }

    private:
        static constexpr std::size_t kMaxBatch = 60'000;  // USO admite hasta 64 KB por llamada.
        MediaSender& sender_;
        std::vector<std::uint8_t> buffer_;
        std::vector<std::uint8_t> scratch_;
        std::uint32_t segment_ = 0;
    };

    [[nodiscard]] std::uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }

private:
    UdpSocket& socket_;
    std::vector<std::uint8_t> key_;
    mutable std::mutex mutex_;
    std::optional<Endpoint> endpoint_;
    std::atomic<std::uint64_t> seq_{1};
    std::atomic<std::uint64_t> bytes_sent_{0};
};

}  // namespace lancast::net
