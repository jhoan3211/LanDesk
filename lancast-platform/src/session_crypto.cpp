#include "lancast/platform/session_crypto.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace lancast::session {
namespace {

crypto::Key256 to_key(const std::vector<std::uint8_t>& v) {
    crypto::Key256 k{};
    std::copy_n(v.begin(), std::min(v.size(), k.size()), k.begin());
    return k;
}

}  // namespace

crypto::Digest transcript(std::span<const std::uint8_t> hello, std::span<const std::uint8_t> hello_ack) {
    crypto::Sha256 h;
    const std::string_view label = "lancast-v1-transcript";
    h.update({reinterpret_cast<const std::uint8_t*>(label.data()), label.size()});
    h.update(hello);
    h.update(hello_ack);
    return h.finish();
}

crypto::Key256 auth_key_from_pin(std::string_view pin, const crypto::Digest& th) {
    return crypto::pbkdf2_sha256(pin, th, kPinIterations);
}

crypto::Key256 auth_key_from_secret(std::span<const std::uint8_t> secret, const crypto::Digest& th) {
    return to_key(crypto::hkdf_sha256(secret, th, "lancast auth", 32));
}

crypto::Digest proof(const crypto::Key256& auth_key, std::string_view role, const crypto::Digest& th,
                     const crypto::Key256& z) {
    std::vector<std::uint8_t> msg(role.begin(), role.end());
    msg.insert(msg.end(), th.begin(), th.end());
    msg.insert(msg.end(), z.begin(), z.end());
    return crypto::hmac_sha256(auth_key, msg);
}

Keys derive_keys(const crypto::Key256& z, const crypto::Key256& auth_key, const crypto::Digest& th) {
    std::vector<std::uint8_t> ikm(z.begin(), z.end());
    ikm.insert(ikm.end(), auth_key.begin(), auth_key.end());
    const auto master = crypto::hkdf_sha256(ikm, th, "lancast session", 32);
    Keys k;
    k.control_c2h = to_key(crypto::hkdf_sha256(master, {}, "control c2h", 32));
    k.control_h2c = to_key(crypto::hkdf_sha256(master, {}, "control h2c", 32));
    k.media_c2h = to_key(crypto::hkdf_sha256(master, {}, "media c2h", 32));
    k.media_h2c = to_key(crypto::hkdf_sha256(master, {}, "media h2c", 32));
    k.file_c2h = to_key(crypto::hkdf_sha256(master, {}, "file c2h", 32));
    k.file_h2c = to_key(crypto::hkdf_sha256(master, {}, "file h2c", 32));
    k.pairing_secret = to_key(crypto::hkdf_sha256(master, {}, "pairing secret", 32));
    return k;
}

std::string random_pin() {
    // Rechazo de muestras para evitar sesgo de módulo.
    std::uint32_t v = 0;
    do {
        v = crypto::random_u32();
    } while (v >= 4'294'000'000u);
    return std::format("{:06}", v % 1'000'000);
}

}  // namespace lancast::session
