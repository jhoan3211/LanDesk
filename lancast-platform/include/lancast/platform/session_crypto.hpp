#pragma once

// Derivación de claves del handshake de LanCast.
//
//   TH       = SHA-256(Hello || HelloAck)                      (transcript)
//   Z        = ECDH P-256 efímero                               (secreto compartido)
//   K_auth   = PBKDF2(PIN, TH, 100k)   o   HKDF(secreto_emparejamiento, TH, "lancast auth")
//   prueba   = HMAC(K_auth, rol || TH || Z)                     (cliente y host demuestran K_auth)
//   maestro  = HKDF(Z || K_auth, TH, "lancast session")         (claves por dirección y canal)
//
// Un atacante en la red no puede derivar las claves sin el PIN o el secreto de emparejamiento, y
// tras el primer emparejamiento con PIN ambos equipos guardan un secreto de 256 bits (TOFU).

#include <array>
#include <span>
#include <string_view>

#include "lancast/platform/crypto.hpp"

namespace lancast::session {

inline constexpr std::uint32_t kPinIterations = 100'000;

struct Keys {
    crypto::Key256 control_c2h{};
    crypto::Key256 control_h2c{};
    crypto::Key256 media_c2h{};
    crypto::Key256 media_h2c{};
    crypto::Key256 file_c2h{};
    crypto::Key256 file_h2c{};
    crypto::Key256 pairing_secret{};
};

[[nodiscard]] crypto::Digest transcript(std::span<const std::uint8_t> hello, std::span<const std::uint8_t> hello_ack);
[[nodiscard]] crypto::Key256 auth_key_from_pin(std::string_view pin, const crypto::Digest& th);
[[nodiscard]] crypto::Key256 auth_key_from_secret(std::span<const std::uint8_t> secret, const crypto::Digest& th);
[[nodiscard]] crypto::Digest proof(const crypto::Key256& auth_key, std::string_view role, const crypto::Digest& th,
                                   const crypto::Key256& z);
[[nodiscard]] Keys derive_keys(const crypto::Key256& z, const crypto::Key256& auth_key, const crypto::Digest& th);

// PIN de 6 dígitos uniforme.
[[nodiscard]] std::string random_pin();

}  // namespace lancast::session
