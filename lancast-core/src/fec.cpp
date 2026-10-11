#include "lancast/core/fec.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define LANCAST_X86 1
#endif

namespace lancast::fec {
namespace {

// Polinomio primitivo x^8 + x^4 + x^3 + x^2 + 1 (0x11D).
struct Tables {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
    std::array<std::array<std::uint8_t, 256>, 256> mul{};

    Tables() {
        unsigned x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            log[x] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
        for (int a = 0; a < 256; ++a) {
            for (int b = 0; b < 256; ++b) {
                mul[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] =
                    (a == 0 || b == 0) ? 0
                                       : exp[static_cast<std::size_t>(log[static_cast<std::size_t>(a)]) +
                                             log[static_cast<std::size_t>(b)]];
            }
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

#ifdef LANCAST_X86
enum class SimdLevel { None, Ssse3, Avx2 };

SimdLevel detect_simd() noexcept {
    int info[4]{};
    __cpuid(info, 0);
    const int max_leaf = info[0];
    __cpuid(info, 1);
    const bool ssse3 = (info[2] & (1 << 9)) != 0;
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    bool avx2 = false;
    if (max_leaf >= 7 && osxsave && avx) {
        const unsigned long long xcr0 = _xgetbv(0);
        if ((xcr0 & 6) == 6) {
            __cpuidex(info, 7, 0);
            avx2 = (info[1] & (1 << 5)) != 0;
        }
    }
    if (avx2) return SimdLevel::Avx2;
    if (ssse3) return SimdLevel::Ssse3;
    return SimdLevel::None;
}

SimdLevel simd_level() noexcept {
    static const SimdLevel level = detect_simd();
    return level;
}

void mul_add_ssse3(std::uint8_t* dst, const std::uint8_t* src, const std::uint8_t* lo_tbl, const std::uint8_t* hi_tbl,
                   std::size_t& i, std::size_t size) noexcept {
    const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lo_tbl));
    const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(hi_tbl));
    const __m128i mask = _mm_set1_epi8(0x0F);
    for (; i + 16 <= size; i += 16) {
        const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i l = _mm_shuffle_epi8(lo, _mm_and_si128(s, mask));
        const __m128i h = _mm_shuffle_epi8(hi, _mm_and_si128(_mm_srli_epi64(s, 4), mask));
        const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_xor_si128(d, _mm_xor_si128(l, h)));
    }
}

void mul_add_avx2(std::uint8_t* dst, const std::uint8_t* src, const std::uint8_t* lo_tbl, const std::uint8_t* hi_tbl,
                  std::size_t& i, std::size_t size) noexcept {
    const __m256i lo = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lo_tbl)));
    const __m256i hi = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(hi_tbl)));
    const __m256i mask = _mm256_set1_epi8(0x0F);
    for (; i + 32 <= size; i += 32) {
        const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i l = _mm256_shuffle_epi8(lo, _mm256_and_si256(s, mask));
        const __m256i h = _mm256_shuffle_epi8(hi, _mm256_and_si256(_mm256_srli_epi64(s, 4), mask));
        const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_xor_si256(d, _mm256_xor_si256(l, h)));
    }
}
#endif

// Invierte una matriz n x n en GF(2^8) por Gauss-Jordan. Devuelve false si es singular.
bool invert_matrix(std::vector<std::uint8_t>& a, int n) {
    const auto& t = tables();
    const auto N = static_cast<std::size_t>(n);
    std::vector<std::uint8_t> inv(N * N, 0);
    for (std::size_t i = 0; i < N; ++i) inv[i * N + i] = 1;

    for (std::size_t col = 0; col < N; ++col) {
        std::size_t pivot = col;
        while (pivot < N && a[pivot * N + col] == 0) ++pivot;
        if (pivot == N) return false;
        if (pivot != col) {
            for (std::size_t j = 0; j < N; ++j) {
                std::swap(a[pivot * N + j], a[col * N + j]);
                std::swap(inv[pivot * N + j], inv[col * N + j]);
            }
        }
        const std::uint8_t pinv = gf_inv(a[col * N + col]);
        for (std::size_t j = 0; j < N; ++j) {
            a[col * N + j] = t.mul[pinv][a[col * N + j]];
            inv[col * N + j] = t.mul[pinv][inv[col * N + j]];
        }
        for (std::size_t r = 0; r < N; ++r) {
            if (r == col) continue;
            const std::uint8_t f = a[r * N + col];
            if (f == 0) continue;
            for (std::size_t j = 0; j < N; ++j) {
                a[r * N + j] ^= t.mul[f][a[col * N + j]];
                inv[r * N + j] ^= t.mul[f][inv[col * N + j]];
            }
        }
    }
    a = std::move(inv);
    return true;
}

}  // namespace

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) noexcept { return tables().mul[a][b]; }

std::uint8_t gf_inv(std::uint8_t a) noexcept {
    if (a == 0) return 0;
    const auto& t = tables();
    return t.exp[255 - t.log[a]];
}

void mul_add_region(std::uint8_t* dst, const std::uint8_t* src, std::uint8_t c, std::size_t size) noexcept {
    if (c == 0) return;
    std::size_t i = 0;
    if (c == 1) {
        for (; i < size; ++i) dst[i] ^= src[i];
        return;
    }
    const auto& row = tables().mul[c];
#ifdef LANCAST_X86
    const SimdLevel level = simd_level();
    if (level != SimdLevel::None) {
        alignas(16) std::uint8_t lo[16];
        alignas(16) std::uint8_t hi[16];
        for (int n = 0; n < 16; ++n) {
            lo[n] = row[static_cast<std::size_t>(n)];
            hi[n] = row[static_cast<std::size_t>(n << 4)];
        }
        if (level == SimdLevel::Avx2) mul_add_avx2(dst, src, lo, hi, i, size);
        mul_add_ssse3(dst, src, lo, hi, i, size);
    }
#endif
    for (; i < size; ++i) dst[i] ^= row[src[i]];
}

ReedSolomon::ReedSolomon(int data_shards, int parity_shards) : k_(data_shards), m_(parity_shards) {
    if (k_ <= 0 || m_ < 0 || k_ + m_ > kMaxTotalShards) throw std::invalid_argument("ReedSolomon: k/m fuera de rango");
    // Cauchy: C[i][j] = 1 / (x_i ^ y_j), x_i = k + i, y_j = j (todos distintos, así que x_i ^ y_j != 0).
    matrix_.resize(static_cast<std::size_t>(m_) * static_cast<std::size_t>(k_));
    for (int i = 0; i < m_; ++i) {
        for (int j = 0; j < k_; ++j) {
            matrix_[static_cast<std::size_t>(i) * static_cast<std::size_t>(k_) + static_cast<std::size_t>(j)] =
                gf_inv(static_cast<std::uint8_t>((k_ + i) ^ j));
        }
    }
}

void ReedSolomon::encode(std::span<const std::uint8_t* const> data, std::span<std::uint8_t* const> parity,
                         std::size_t shard_size) const {
    for (int i = 0; i < m_; ++i) {
        std::uint8_t* out = parity[static_cast<std::size_t>(i)];
        std::fill_n(out, shard_size, std::uint8_t{0});
        for (int j = 0; j < k_; ++j) mul_add_region(out, data[static_cast<std::size_t>(j)], coef(i, j), shard_size);
    }
}

bool ReedSolomon::reconstruct(std::span<std::uint8_t* const> shards, std::span<const bool> present,
                              std::size_t shard_size) const {
    std::vector<int> missing_data;
    for (int j = 0; j < k_; ++j) {
        if (!present[static_cast<std::size_t>(j)]) missing_data.push_back(j);
    }
    if (missing_data.empty()) return true;

    // Elegir k filas presentes: primero datos, luego paridad.
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(k_));
    for (int s = 0; s < k_ + m_ && static_cast<int>(rows.size()) < k_; ++s) {
        if (present[static_cast<std::size_t>(s)]) rows.push_back(s);
    }
    if (static_cast<int>(rows.size()) < k_) return false;

    const auto K = static_cast<std::size_t>(k_);
    std::vector<std::uint8_t> a(K * K, 0);
    for (std::size_t r = 0; r < K; ++r) {
        const int s = rows[r];
        if (s < k_) {
            a[r * K + static_cast<std::size_t>(s)] = 1;
        } else {
            for (int j = 0; j < k_; ++j) a[r * K + static_cast<std::size_t>(j)] = coef(s - k_, j);
        }
    }
    if (!invert_matrix(a, k_)) return false;

    // dato_j = sum_r inv[j][r] * fila_r. Se calculan en buffers temporales para no pisar fuentes.
    std::vector<std::vector<std::uint8_t>> recovered(missing_data.size(), std::vector<std::uint8_t>(shard_size, 0));
    for (std::size_t idx = 0; idx < missing_data.size(); ++idx) {
        const auto j = static_cast<std::size_t>(missing_data[idx]);
        for (std::size_t r = 0; r < K; ++r) {
            mul_add_region(recovered[idx].data(), shards[static_cast<std::size_t>(rows[r])], a[j * K + r], shard_size);
        }
    }
    for (std::size_t idx = 0; idx < missing_data.size(); ++idx) {
        std::copy(recovered[idx].begin(), recovered[idx].end(), shards[static_cast<std::size_t>(missing_data[idx])]);
    }
    return true;
}

int parity_for(int data_shards, int fec_percent) noexcept {
    if (fec_percent <= 0 || data_shards <= 0) return 0;
    int m = (data_shards * fec_percent + 99) / 100;
    m = std::max(m, 1);
    return std::min(m, kMaxTotalShards - data_shards);
}

}  // namespace lancast::fec
