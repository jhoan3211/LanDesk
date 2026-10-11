// Pruebas de integración del camino de video y de la criptografía contra el hardware real.

#include <gtest/gtest.h>

#include <format>

#include "lancast/core/encoding.hpp"
#include "lancast/platform/capture.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/video_processor.hpp"

using namespace lancast;

TEST(Crypto, HkdfRfc5869TestCase1) {
    const auto ikm = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b").value();
    const auto salt = from_hex("000102030405060708090a0b0c").value();
    const auto info_bytes = from_hex("f0f1f2f3f4f5f6f7f8f9").value();
    const std::string info{info_bytes.begin(), info_bytes.end()};
    const auto okm = crypto::hkdf_sha256(ikm, salt, info, 42);
    EXPECT_EQ(to_hex(okm),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

TEST(Crypto, Sha256AndIncremental) {
    const std::string_view abc = "abc";
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()};
    EXPECT_EQ(to_hex(crypto::sha256(bytes)), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    crypto::Sha256 h;
    h.update(bytes.first(1));
    h.update(bytes.subspan(1));
    EXPECT_EQ(h.finish(), crypto::sha256(bytes));
}

TEST(Crypto, AesGcmRoundtripAndTamperDetection) {
    std::array<std::uint8_t, 32> key{};
    crypto::random_bytes(key);
    crypto::AesGcm gcm(key);
    const std::array<std::uint8_t, 12> nonce{1, 2, 3};
    const std::array<std::uint8_t, 4> aad{9, 9, 9, 9};
    std::vector<std::uint8_t> plain(1000, 0x42);
    std::vector<std::uint8_t> sealed(plain.size() + 16);
    ASSERT_TRUE(gcm.encrypt(nonce, aad, plain, sealed.data()));
    std::vector<std::uint8_t> out(plain.size());
    ASSERT_TRUE(gcm.decrypt(nonce, aad, sealed, out.data()));
    EXPECT_EQ(out, plain);
    sealed[10] ^= 1;
    EXPECT_FALSE(gcm.decrypt(nonce, aad, sealed, out.data()));
}

TEST(Crypto, EcdhAgreement) {
    crypto::Ecdh a;
    crypto::Ecdh b;
    const auto sa = a.derive(b.public_key());
    const auto sb = b.derive(a.public_key());
    ASSERT_TRUE(sa.has_value());
    ASSERT_TRUE(sb.has_value());
    EXPECT_EQ(*sa, *sb);
    std::array<std::uint8_t, 64> bogus{};
    EXPECT_FALSE(a.derive(bogus).has_value());
}

TEST(Crypto, DpapiRoundtrip) {
    const std::vector<std::uint8_t> secret{1, 2, 3, 4, 5};
    const auto sealed = crypto::protect(secret);
    ASSERT_FALSE(sealed.empty());
    EXPECT_EQ(crypto::unprotect(sealed).value(), secret);
}

class VideoPipeline : public ::testing::Test {
protected:
    static void SetUpTestSuite() { platform::enable_per_monitor_dpi_awareness(); }
    mf::MfScope mf_;
};

TEST_F(VideoPipeline, CaptureConvertEncodeDecode) {
    auto device = gpu::create_device(std::nullopt);
    ASSERT_TRUE(device.has_value()) << device.error().message();

    const auto gpus = platform::enumerate_gpus().value();
    const auto monitors = platform::enumerate_monitors(gpus).value();
    const auto primary = std::ranges::find_if(monitors, [](const MonitorInfo& m) { return m.primary; });
    ASSERT_NE(primary, monitors.end());

    // Captura del monitor principal (si no está en esta GPU, la prueba se omite).
    auto cap = capture::DesktopCapture::create(*device, {primary->desktop}, primary->desktop);
    if (!cap) GTEST_SKIP() << cap.error().message();
    capture::Status st = capture::Status::Timeout;
    for (int i = 0; i < 20 && st != capture::Status::NewFrame; ++i) st = (*cap)->acquire(100);
    ASSERT_EQ(st, capture::Status::NewFrame);

    const Size out_size{1280, 720};
    auto vp = gpu::VideoProcessor::create(*device, (*cap)->texture_size(), out_size, gpu::ColorConversion::RgbToYuv709);
    ASSERT_TRUE(vp.has_value()) << vp.error().message();
    auto nv12 = gpu::create_texture(device->device.Get(), out_size.width, out_size.height, DXGI_FORMAT_NV12,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ASSERT_TRUE(nv12.has_value());
    ASSERT_TRUE((*vp)->process((*cap)->texture(), 0, nv12->Get(), nullptr, nullptr));

    const auto codecs = mf::available_encoders(device->luid);
    ASSERT_FALSE(codecs.empty()) << "Sin codificador por hardware";
    const auto decoders = mf::available_decoders();

    for (const Codec codec : codecs) {
        SCOPED_TRACE(std::string{to_string(codec)});
        auto enc = mf::create_encoder(*device, {codec, out_size, 60, 10000, RateControl::Cbr});
        ASSERT_TRUE(enc.has_value()) << enc.error().message();
        std::printf("  Backend: %s (%s)\n", (*enc)->backend().c_str(), (*enc)->name().c_str());

        std::unique_ptr<mf::Decoder> dec;
        if (std::ranges::find(decoders, codec) != decoders.end()) {
            auto d = mf::create_decoder(*device, codec, out_size);
            if (d) dec = std::move(*d);
        }

        int packets = 0;
        int keyframes = 0;
        int decoded = 0;
        for (int i = 0; i < 30; ++i) {
            const auto t0 = gpu::now_us();
            auto p = (*enc)->encode(nv12->Get(), static_cast<std::uint64_t>(i) * 16667, i == 0 || i == 20);
            ASSERT_TRUE(p.has_value()) << p.error().message();
            if (!p->has_value()) continue;
            ++packets;
            if ((*p)->keyframe) ++keyframes;
            if (i >= 3 && i <= 6) {
                std::printf("  %s: entrada %d -> salida ts %llu (frame %llu), %zu bytes, %.2f ms\n",
                            std::string{to_string(codec)}.c_str(), i, static_cast<unsigned long long>((*p)->timestamp_us),
                            static_cast<unsigned long long>(((*p)->timestamp_us + 100) / 16667), (*p)->data.size(),
                            (gpu::now_us() - t0) / 1000.0);
            }
            if (dec) {
                auto f = dec->decode((*p)->data, (*p)->timestamp_us);
                ASSERT_TRUE(f.has_value()) << f.error().message();
                if (f->has_value()) {
                    ++decoded;
                    EXPECT_EQ((*f)->size, out_size);
                }
            }
        }
        EXPECT_GE(packets, 28);
        EXPECT_GE(keyframes, 2);
        if (dec) {
            EXPECT_GE(decoded, 22) << dec->name();  // El decodificador de Windows a veces retiene algún frame suelto.
        } else {
            std::printf("  (sin decodificador %s en este equipo)\n", std::string{to_string(codec)}.c_str());
        }
    }
}
