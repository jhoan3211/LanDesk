#pragma once

// Conversión de frecuencia de muestreo (interpolación lineal con estado entre bloques) y de canales.
// Suficiente para voz/música de escritorio y con latencia cero (no usa ventanas de filtro).

#include <cstdint>
#include <span>
#include <vector>

namespace lancast::audio {

class Resampler {
public:
    // Entrada siempre estéreo intercalado int16; salida con `out_channels` (1 o 2).
    void configure(std::uint32_t in_rate, std::uint32_t out_rate, std::uint8_t out_channels);
    void process(std::span<const std::int16_t> stereo_in, std::vector<std::int16_t>& out);

    [[nodiscard]] std::uint32_t in_rate() const noexcept { return in_rate_; }
    [[nodiscard]] std::uint32_t out_rate() const noexcept { return out_rate_; }
    [[nodiscard]] std::uint8_t out_channels() const noexcept { return out_channels_; }

private:
    std::uint32_t in_rate_ = 48000;
    std::uint32_t out_rate_ = 48000;
    std::uint8_t out_channels_ = 2;
    double pos_ = 0.0;               // Posición fraccional relativa al primer frame del bloque actual.
    std::int16_t prev_[2] = {0, 0};  // Último frame del bloque anterior (para interpolar en el borde).
    bool has_prev_ = false;
};

}  // namespace lancast::audio
