#pragma once

// Pérdida de paquetes a partir de la secuencia de los datagramas, tolerante al desorden. El host numera cada paquete
// al prepararlo y envía el video en tandas, así que un paquete de audio puede llegar antes que video con número menor.
// Un número se da por perdido solo cuando ya llegaron `grace` paquetes posteriores sin él.

#include <algorithm>
#include <array>
#include <cstdint>

namespace lancast::media {

class LossMeter {
public:
    explicit LossMeter(std::uint64_t grace = 64) : grace_(grace) {}

    void received(std::uint64_t seq) noexcept {
        if (!started_ || seq + kRestart < high_) {  // Primer paquete o el host reinició la secuencia.
            started_ = true;
            high_ = seq;
            next_ = seq;
            seen_.fill(kEmpty);
        }
        if (seq < next_) return;  // Llegó después del margen: ya se contó como perdido.
        seen_[seq % kSlots] = seq;
        high_ = std::max(high_, seq);
        while (high_ - next_ > grace_) {
            ++expected_;
            if (seen_[next_ % kSlots] != next_) ++lost_;
            ++next_;
        }
    }

    struct Counts {
        std::uint64_t expected = 0;
        std::uint64_t lost = 0;
        [[nodiscard]] double percent() const noexcept {
            return expected == 0 ? 0.0 : static_cast<double>(lost) * 100.0 / static_cast<double>(expected);
        }
    };
    // Lo contado desde la llamada anterior.
    Counts take() noexcept {
        const Counts c{expected_, lost_};
        expected_ = 0;
        lost_ = 0;
        return c;
    }

private:
    static constexpr std::size_t kSlots = 4096;
    static constexpr std::uint64_t kEmpty = ~0ull;
    static constexpr std::uint64_t kRestart = 100'000;
    std::uint64_t grace_;
    bool started_ = false;
    std::uint64_t high_ = 0;
    std::uint64_t next_ = 0;  // Siguiente número por decidir (los anteriores ya se contaron).
    std::uint64_t expected_ = 0;
    std::uint64_t lost_ = 0;
    std::array<std::uint64_t, kSlots> seen_{};
};

}  // namespace lancast::media
