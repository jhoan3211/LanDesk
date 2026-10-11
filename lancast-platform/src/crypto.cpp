#include "lancast/platform/crypto.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <stdexcept>

#include "win.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace lancast::crypto {
namespace {

void check(NTSTATUS status, const char* what) {
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(std::string{"CNG: "} + what + " falló");
    }
}

// Proveedores reutilizables (los pseudo-handles de Windows 10 son thread-safe y no requieren cierre).
BCRYPT_ALG_HANDLE hmac_alg() {
    static BCRYPT_ALG_HANDLE h = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        check(BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
              "abrir HMAC-SHA256");
        return a;
    }();
    return h;
}

}  // namespace

void random_bytes(std::span<std::uint8_t> out) {
    check(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
          "BCryptGenRandom");
}

std::uint32_t random_u32() {
    std::uint32_t v = 0;
    random_bytes({reinterpret_cast<std::uint8_t*>(&v), sizeof(v)});
    return v;
}

Digest sha256(std::span<const std::uint8_t> data) {
    Digest out{};
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), static_cast<ULONG>(out.size())),
          "SHA-256");
    return out;
}

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 20> out{};
    check(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), static_cast<ULONG>(out.size())),
          "SHA-1");
    return out;
}

Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    Digest out{};
    check(BCryptHash(hmac_alg(), const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                     const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), out.data(),
                     static_cast<ULONG>(out.size())),
          "HMAC-SHA256");
    return out;
}

std::vector<std::uint8_t> hkdf_sha256(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt,
                                      std::string_view info, std::size_t length) {
    // RFC 5869.
    const std::array<std::uint8_t, 32> zero_salt{};
    const Digest prk = hmac_sha256(salt.empty() ? std::span<const std::uint8_t>{zero_salt} : salt, ikm);

    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> block;
    Digest t{};
    std::size_t t_len = 0;
    for (std::uint8_t counter = 1; out.size() < length; ++counter) {
        block.assign(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(t_len));
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(counter);
        t = hmac_sha256(prk, block);
        t_len = t.size();
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

Key256 pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt, std::uint32_t iterations) {
    Key256 out{};
    check(BCryptDeriveKeyPBKDF2(hmac_alg(), reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
                                static_cast<ULONG>(salt.size()), iterations, out.data(), static_cast<ULONG>(out.size()), 0),
          "PBKDF2");
    return out;
}

bool equal_ct(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

Sha256::Sha256() {
    BCRYPT_HASH_HANDLE h = nullptr;
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0), "crear SHA-256");
    handle_ = h;
}

Sha256::~Sha256() {
    if (handle_ != nullptr) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle_));
}

void Sha256::update(std::span<const std::uint8_t> data) {
    check(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(handle_), const_cast<PUCHAR>(data.data()),
                         static_cast<ULONG>(data.size()), 0),
          "SHA-256 update");
}

Digest Sha256::finish() {
    Digest out{};
    check(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(handle_), out.data(), static_cast<ULONG>(out.size()), 0),
          "SHA-256 finish");
    return out;
}

AesGcm::AesGcm(std::span<const std::uint8_t> key) {
    BCRYPT_KEY_HANDLE k = nullptr;
    check(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &k, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                     static_cast<ULONG>(key.size()), 0),
          "clave AES-GCM");
    key_ = k;
}

AesGcm::~AesGcm() {
    if (key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool AesGcm::encrypt(std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> aad,
                     std::span<const std::uint8_t> plaintext, std::uint8_t* out) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = out + plaintext.size();
    info.cbTag = kTagSize;
    ULONG written = 0;
    const NTSTATUS st =
        BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(plaintext.data()),
                      static_cast<ULONG>(plaintext.size()), &info, nullptr, 0, out, static_cast<ULONG>(plaintext.size()),
                      &written, 0);
    return BCRYPT_SUCCESS(st);
}

bool AesGcm::decrypt(std::span<const std::uint8_t> nonce, std::span<const std::uint8_t> aad,
                     std::span<const std::uint8_t> ciphertext, std::uint8_t* out) {
    if (ciphertext.size() < kTagSize) return false;
    const std::size_t len = ciphertext.size() - kTagSize;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<PUCHAR>(ciphertext.data() + len);
    info.cbTag = kTagSize;
    ULONG written = 0;
    const NTSTATUS st = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(ciphertext.data()),
                                      static_cast<ULONG>(len), &info, nullptr, 0, out, static_cast<ULONG>(len), &written, 0);
    return BCRYPT_SUCCESS(st);
}

Ecdh::Ecdh() {
    BCRYPT_KEY_HANDLE k = nullptr;
    check(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, &k, 256, 0), "generar ECDH");
    check(BCryptFinalizeKeyPair(k, 0), "finalizar ECDH");
    key_ = k;

    std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    ULONG size = 0;
    check(BCryptExportKey(k, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), static_cast<ULONG>(blob.size()), &size, 0),
          "exportar ECDH");
    std::copy_n(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), 64, public_.begin());
}

Ecdh::~Ecdh() {
    if (key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

platform::Result<Key256> Ecdh::derive(std::span<const std::uint8_t, 64> remote) const {
    std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(remote.begin(), remote.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));

    BCRYPT_KEY_HANDLE peer = nullptr;
    NTSTATUS st = BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob.data(),
                                      static_cast<ULONG>(blob.size()), 0);
    if (!BCRYPT_SUCCESS(st)) return std::unexpected(platform::Error{"Clave pública ECDH inválida", st});

    BCRYPT_SECRET_HANDLE secret = nullptr;
    st = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peer, &secret, 0);
    BCryptDestroyKey(peer);
    if (!BCRYPT_SUCCESS(st)) return std::unexpected(platform::Error{"Acuerdo ECDH", st});

    Key256 out{};
    ULONG size = 0;
    st = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, out.data(), static_cast<ULONG>(out.size()), &size, 0);
    BCryptDestroySecret(secret);
    if (!BCRYPT_SUCCESS(st) || size != out.size()) return std::unexpected(platform::Error{"Derivar secreto ECDH", st});
    std::reverse(out.begin(), out.end());  // CNG lo entrega en little-endian.
    return out;
}

std::vector<std::uint8_t> protect(std::span<const std::uint8_t> data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"LanCast", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::vector<std::uint8_t> result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

platform::Result<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t> data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return platform::detail::hr_error("CryptUnprotectData", HRESULT_FROM_WIN32(GetLastError()));
    }
    std::vector<std::uint8_t> result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

}  // namespace lancast::crypto
