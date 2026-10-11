#pragma once

// Criptografía con Windows CNG (bcrypt.dll): sin dependencias externas y con aceleración AES-NI / SHA-NI.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "lancast/platform/error.hpp"

namespace lancast::crypto {

using Key256 = std::array<std::uint8_t, 32>;
using Digest = std::array<std::uint8_t, 32>;

void random_bytes(std::span<std::uint8_t> out);
[[nodiscard]] std::uint32_t random_u32();

[[nodiscard]] Digest sha256(std::span<const std::uint8_t> data);
[[nodiscard]] Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
[[nodiscard]] std::vector<std::uint8_t> hkdf_sha256(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt,
                                                    std::string_view info, std::size_t length);
[[nodiscard]] Key256 pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt,
                                   std::uint32_t iterations);
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data);  // Solo WebSocket.

// Comparación en tiempo constante.
[[nodiscard]] bool equal_ct(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept;

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(std::span<const std::uint8_t> data);
    [[nodiscard]] Digest finish();

private:
    void* handle_ = nullptr;
};

// AES-256-GCM. Nonce de 12 bytes, tag de 16. Cada instancia debe usarse desde un solo hilo a la vez.
class AesGcm {
public:
    static constexpr std::size_t kNonceSize = 12;
    static constexpr std::size_t kTagSize = 16;

    explicit AesGcm(std::span<const std::uint8_t> key);
    ~AesGcm();
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    // Cifra `plaintext` en `out` (tamaño plaintext + 16).
    bool encrypt(std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> aad,
                 std::span<const std::uint8_t> plaintext, std::uint8_t* out);
    // Descifra `ciphertext` (incluye tag al final) en `out` (tamaño ciphertext - 16).
    bool decrypt(std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> aad,
                 std::span<const std::uint8_t> ciphertext, std::uint8_t* out);

private:
    void* key_ = nullptr;
};

// ECDH P-256 efímero.
class Ecdh {
public:
    Ecdh();
    ~Ecdh();
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;

    [[nodiscard]] const std::array<std::uint8_t, 64>& public_key() const noexcept { return public_; }
    // Secreto compartido (coordenada X, 32 bytes big-endian). nullopt si la clave remota es inválida.
    [[nodiscard]] platform::Result<Key256> derive(std::span<const std::uint8_t, 64> remote) const;

private:
    void* key_ = nullptr;
    std::array<std::uint8_t, 64> public_{};
};

// DPAPI (ligado al usuario de Windows) para guardar secretos de emparejamiento en disco.
[[nodiscard]] std::vector<std::uint8_t> protect(std::span<const std::uint8_t> data);
[[nodiscard]] platform::Result<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t> data);

}  // namespace lancast::crypto
