#pragma once

// Envío de datagramas de medios cifrados (video/audio) a un endpoint UDP. Thread-safe: cada hilo
// usa su propio cifrador y la secuencia (que es el nonce) es un contador atómico compartido.
// Espectadores: un MediaSender puede tener "espejos" (otros clientes que solo ven) que reciben una copia de cada
// paquete cifrada con su propia clave: el video se codifica una sola vez.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
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

    void add_mirror(std::shared_ptr<MediaSender> m) {
        std::scoped_lock lock(mirrors_mutex_);
        mirrors_.push_back(std::move(m));
    }
    void remove_mirror(const MediaSender* m) {
        std::scoped_lock lock(mirrors_mutex_);
        std::erase_if(mirrors_, [m](const auto& x) { return x.get() == m; });
    }
    [[nodiscard]] std::vector<std::shared_ptr<MediaSender>> mirrors() const {
        std::scoped_lock lock(mirrors_mutex_);
        return mirrors_;
    }

    // Como espejo: cifra `payload` con la clave propia (desde cualquier hilo) y lo agrega a `out` (o lo envía si
    // out es null). Devuelve el tamaño del datagrama, 0 si no hay endpoint todavía.
    std::size_t seal_copy(media::PacketType type, std::span<const std::uint8_t> payload, std::vector<std::uint8_t>* out) {
        const auto ep = endpoint();
        if (!ep) return 0;
        std::scoped_lock lock(copy_mutex_);
        if (!copy_cipher_) copy_cipher_ = make_cipher();
        std::uint8_t header[media::kPacketHeaderSize];
        media::PacketHeader{type, seq_.fetch_add(1)}.write(header);
        if (!copy_cipher_->seal(header, payload, copy_scratch_)) return 0;
        bytes_sent_.fetch_add(copy_scratch_.size());
        if (out != nullptr) {
            out->insert(out->end(), copy_scratch_.begin(), copy_scratch_.end());
        } else {
            socket_.send_to(*ep, copy_scratch_);
        }
        return copy_scratch_.size();
    }

    bool send(DatagramCipher& cipher, media::PacketType type, std::span<const std::uint8_t> payload,
              std::vector<std::uint8_t>& scratch) {
        for (const auto& m : mirrors()) m->seal_copy(type, payload, nullptr);
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
        explicit Batch(MediaSender& sender, Pacer* pacer = nullptr) : sender_(sender), pacer_(pacer) {
            for (auto& m : sender.mirrors()) mirrors_.push_back({std::move(m), {}, 0});
        }
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
            // Espectadores: la misma tanda, con su clave (mismo tamaño de datagrama: se vacían juntos).
            for (auto& m : mirrors_) {
                const std::size_t n = m.sender->seal_copy(type, payload, &m.buffer);
                if (n != 0) m.segment = static_cast<std::uint32_t>(n);
            }
        }

        void flush() {
            if (buffer_.empty()) return;
            if (pacer_ != nullptr) pacer_->wait(buffer_.size());
            if (const auto ep = sender_.endpoint()) {
                sender_.bytes_sent_.fetch_add(buffer_.size());
                sender_.socket_.send_batch(*ep, buffer_, segment_);
            }
            buffer_.clear();
            for (auto& m : mirrors_) {
                if (m.buffer.empty()) continue;
                if (const auto ep = m.sender->endpoint()) m.sender->socket_.send_batch(*ep, m.buffer, m.segment);
                m.buffer.clear();
            }
        }

    private:
        static constexpr std::size_t kMaxBatch = 60'000;    // USO admite hasta 64 KB por llamada.
        static constexpr std::size_t kPacedBatch = 22'000;  // ~16 paquetes por tanda con ritmo.
        struct Mirror {
            std::shared_ptr<MediaSender> sender;
            std::vector<std::uint8_t> buffer;
            std::uint32_t segment = 0;
        };
        MediaSender& sender_;
        Pacer* pacer_ = nullptr;
        std::vector<Mirror> mirrors_;
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
    mutable std::mutex mirrors_mutex_;
    std::vector<std::shared_ptr<MediaSender>> mirrors_;
    std::mutex copy_mutex_;  // Cifrado como espejo (lo usan los hilos de video y de audio del host).
    std::unique_ptr<DatagramCipher> copy_cipher_;
    std::vector<std::uint8_t> copy_scratch_;
};

}  // namespace lancast::net
