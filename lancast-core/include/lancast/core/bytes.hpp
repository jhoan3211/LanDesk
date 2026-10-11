#pragma once

// Serialización binaria little-endian sin asignaciones ocultas: es el formato del protocolo de LanCast.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lancast {

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buf_.reserve(reserve); }

    ByteWriter& u8(std::uint8_t v) {
        buf_.push_back(v);
        return *this;
    }
    ByteWriter& boolean(bool v) { return u8(v ? 1 : 0); }
    ByteWriter& u16(std::uint16_t v) { return put(v); }
    ByteWriter& u32(std::uint32_t v) { return put(v); }
    ByteWriter& u64(std::uint64_t v) { return put(v); }
    ByteWriter& i32(std::int32_t v) { return put(static_cast<std::uint32_t>(v)); }
    ByteWriter& i64(std::int64_t v) { return put(static_cast<std::uint64_t>(v)); }
    ByteWriter& f32(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return put(bits);
    }

    // Bytes sin prefijo de longitud.
    ByteWriter& raw(std::span<const std::uint8_t> data) {
        buf_.insert(buf_.end(), data.begin(), data.end());
        return *this;
    }
    // Bloque con prefijo u32 de longitud.
    ByteWriter& blob(std::span<const std::uint8_t> data) {
        u32(static_cast<std::uint32_t>(data.size()));
        return raw(data);
    }
    // Texto UTF-8 con prefijo u32 de longitud.
    ByteWriter& str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return buf_; }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buf_); }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

private:
    template <typename T>
    ByteWriter& put(T v) {
        std::uint8_t tmp[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i) tmp[i] = static_cast<std::uint8_t>(v >> (8 * i));
        buf_.insert(buf_.end(), tmp, tmp + sizeof(T));
        return *this;
    }

    std::vector<std::uint8_t> buf_;
};

// Lector tolerante: ante datos insuficientes devuelve ceros y marca `ok() == false`.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    std::uint8_t u8() noexcept { return get<std::uint8_t>(); }
    bool boolean() noexcept { return u8() != 0; }
    std::uint16_t u16() noexcept { return get<std::uint16_t>(); }
    std::uint32_t u32() noexcept { return get<std::uint32_t>(); }
    std::uint64_t u64() noexcept { return get<std::uint64_t>(); }
    std::int32_t i32() noexcept { return static_cast<std::int32_t>(get<std::uint32_t>()); }
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(get<std::uint64_t>()); }
    float f32() noexcept {
        const std::uint32_t bits = get<std::uint32_t>();
        float v = 0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::span<const std::uint8_t> raw(std::size_t n) noexcept {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return {};
        }
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    std::vector<std::uint8_t> blob(std::size_t max = 64u * 1024 * 1024) {
        const std::uint32_t n = u32();
        if (n > max) {
            ok_ = false;
            return {};
        }
        auto s = raw(n);
        return {s.begin(), s.end()};
    }
    std::string str(std::size_t max = 1024u * 1024) {
        const std::uint32_t n = u32();
        if (n > max) {
            ok_ = false;
            return {};
        }
        auto s = raw(n);
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    }
    template <std::size_t N>
    std::array<std::uint8_t, N> fixed() noexcept {
        std::array<std::uint8_t, N> out{};
        auto s = raw(N);
        if (s.size() == N) std::memcpy(out.data(), s.data(), N);
        return out;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] std::span<const std::uint8_t> rest() const noexcept { return data_.subspan(pos_); }

private:
    template <typename T>
    T get() noexcept {
        if (!ok_ || remaining() < sizeof(T)) {
            ok_ = false;
            return T{};
        }
        T v{};
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(data_[pos_ + i]) << (8 * i));
        pos_ += sizeof(T);
        return v;
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace lancast
