#pragma once

// Envío de datagramas de medios cifrados (video/audio) a un endpoint UDP. Thread-safe: cada hilo
// usa su propio cifrador y la secuencia (que es el nonce) es un contador atómico compartido.

#include <windows.h>

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
    // Ritmo de envío continuo (entre lotes y cuadros): no se envía más rápido que `mbps`. Las ráfagas de un cuadro
    // grande desbordan los búferes de un router de 1 Gb o del Wi-Fi y se pierden paquetes.
    class Pacer {
    public:
        void set_rate(std::uint32_t mbps) noexcept { mbps_ = mbps; }
        [[nodiscard]] bool active() const noexcept { return mbps_ > 0; }
        // Espera el turno de `bytes` y reserva su tiempo de salida.
        void wait(std::size_t bytes) noexcept {
            if (mbps_ == 0) return;
            const std::uint64_t now = now_us();
            if (next_us_ < now) next_us_ = now;  // Sin deuda acumulada: el ritmo cuenta desde ahora.
            while (now_us() < next_us_) YieldProcessor();
            next_us_ += bytes * 8 / mbps_;  // bits / Mbps = microsegundos
        }
        static std::uint64_t now_us() noexcept {
            static const std::int64_t freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return f.QuadPart;
            }();
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return static_cast<std::uint64_t>(c.QuadPart / freq * 1'000'000 + (c.QuadPart % freq) * 1'000'000 / freq);
        }

    private:
        std::uint32_t mbps_ = 0;
        std::uint64_t next_us_ = 0;
    };

    // Con un Pacer activo se envía en tandas de ~16 paquetes, cada una en su turno.
    class Batch {
    public:
        explicit Batch(MediaSender& sender, Pacer* pacer = nullptr) : sender_(sender), pacer_(pacer) {}
        ~Batch() { flush(); }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        void add(DatagramCipher& cipher, media::PacketType type, std::span<const std::uint8_t> payload) {
            std::uint8_t header[media::kPacketHeaderSize];
            media::PacketHeader{type, sender_.seq_.fetch_add(1)}.write(header);
            if (!cipher.seal(header, payload, scratch_)) return;
            const std::size_t limit = pacer_ != nullptr && pacer_->active() ? kPacedBatch : kMaxBatch;
            if (!buffer_.empty() && (scratch_.size() != segment_ || buffer_.size() + scratch_.size() > limit)) flush();
            segment_ = static_cast<std::uint32_t>(scratch_.size());
            buffer_.insert(buffer_.end(), scratch_.begin(), scratch_.end());
        }

        void flush() {
            if (buffer_.empty()) return;
            if (pacer_ != nullptr) pacer_->wait(buffer_.size());
            if (const auto ep = sender_.endpoint()) {
                sender_.bytes_sent_.fetch_add(buffer_.size());
                sender_.socket_.send_batch(*ep, buffer_, segment_);
            }
            buffer_.clear();
        }

    private:
        static constexpr std::size_t kMaxBatch = 60'000;    // USO admite hasta 64 KB por llamada.
        static constexpr std::size_t kPacedBatch = 22'000;  // ~16 paquetes por tanda con ritmo.
        MediaSender& sender_;
        Pacer* pacer_ = nullptr;
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
