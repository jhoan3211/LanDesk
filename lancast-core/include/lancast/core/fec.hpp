#pragma once

// Código de borrado Reed-Solomon sistemático sobre GF(2^8) con matriz de Cauchy.
// - k fragmentos de datos + m de paridad (k + m <= 255).
// - Cualquier subconjunto de k fragmentos recibidos reconstruye los datos.
// - El producto por constante usa SSSE3/AVX2 (técnica de tablas de nibbles) cuando la CPU lo soporta.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lancast::fec {

inline constexpr int kMaxTotalShards = 255;

// dst ^= c * src (en GF(2^8)), `size` bytes.
void mul_add_region(std::uint8_t* dst, const std::uint8_t* src, std::uint8_t c, std::size_t size) noexcept;
[[nodiscard]] std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) noexcept;
[[nodiscard]] std::uint8_t gf_inv(std::uint8_t a) noexcept;

class ReedSolomon {
public:
    ReedSolomon(int data_shards, int parity_shards);

    [[nodiscard]] int data_shards() const noexcept { return k_; }
    [[nodiscard]] int parity_shards() const noexcept { return m_; }

    // data: k punteros de `shard_size` bytes; parity: m punteros de salida.
    void encode(std::span<const std::uint8_t* const> data, std::span<std::uint8_t* const> parity,
                std::size_t shard_size) const;

    // shards: k + m punteros a buffers de `shard_size` bytes (los ausentes se sobrescriben con los datos
    // reconstruidos; solo se reconstruyen los de datos). present: k + m flags.
    // Devuelve false si hay menos de k fragmentos presentes.
    [[nodiscard]] bool reconstruct(std::span<std::uint8_t* const> shards, std::span<const bool> present,
                                   std::size_t shard_size) const;

private:
    [[nodiscard]] std::uint8_t coef(int parity_row, int data_col) const noexcept {
        return matrix_[static_cast<std::size_t>(parity_row) * static_cast<std::size_t>(k_) +
                       static_cast<std::size_t>(data_col)];
    }

    int k_;
    int m_;
    std::vector<std::uint8_t> matrix_;  // m x k (Cauchy)
};

// Número de fragmentos de paridad para k fragmentos de datos y un porcentaje de FEC.
[[nodiscard]] int parity_for(int data_shards, int fec_percent) noexcept;

}  // namespace lancast::fec
