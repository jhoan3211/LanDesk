#include "lancast/platform/secure_channel.hpp"

#include <array>
#include <cstring>

namespace lancast::net {
namespace {

std::array<std::uint8_t, 12> counter_nonce(std::uint64_t counter) {
    std::array<std::uint8_t, 12> n{};
    for (int i = 0; i < 8; ++i) n[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(counter >> (8 * i));
    return n;
}

}  // namespace

SecureChannel::SecureChannel(TcpStream stream, std::span<const std::uint8_t> send_key,
                             std::span<const std::uint8_t> recv_key)
    : stream_(std::move(stream)), send_cipher_(send_key), recv_cipher_(recv_key) {}

bool SecureChannel::send(std::span<const std::uint8_t> plaintext) {
    std::scoped_lock lock(send_mutex_);
    const auto nonce = counter_nonce(send_counter_++);
    send_buf_.resize(plaintext.size() + crypto::AesGcm::kTagSize);
    if (!send_cipher_.encrypt(nonce, {}, plaintext, send_buf_.data())) return false;
    return stream_.send_frame(send_buf_);
}

bool SecureChannel::recv(std::vector<std::uint8_t>& plaintext, std::size_t max_size) {
    if (!stream_.recv_frame(recv_buf_, max_size + crypto::AesGcm::kTagSize)) return false;
    if (recv_buf_.size() < crypto::AesGcm::kTagSize) return false;
    const auto nonce = counter_nonce(recv_counter_++);
    plaintext.resize(recv_buf_.size() - crypto::AesGcm::kTagSize);
    return recv_cipher_.decrypt(nonce, {}, recv_buf_, plaintext.data());
}

bool DatagramCipher::seal(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload,
                          std::vector<std::uint8_t>& out) {
    out.resize(header.size() + payload.size() + crypto::AesGcm::kTagSize);
    std::memcpy(out.data(), header.data(), header.size());
    return cipher_.encrypt(header, header, payload, out.data() + header.size());
}

bool DatagramCipher::open(std::span<const std::uint8_t> datagram, std::vector<std::uint8_t>& payload) {
    constexpr std::size_t kHeader = 12;
    if (datagram.size() < kHeader + crypto::AesGcm::kTagSize) return false;
    const auto header = datagram.first(kHeader);
    payload.resize(datagram.size() - kHeader - crypto::AesGcm::kTagSize);
    return cipher_.decrypt(header, header, datagram.subspan(kHeader), payload.data());
}

}  // namespace lancast::net
