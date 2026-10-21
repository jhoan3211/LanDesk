// Micrófono del cliente: la captura del micrófono predeterminado entrega audio, y el reproductor puede abrir una
// salida concreta (la del cable de audio virtual del host).

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>

#include "lancast/platform/audio.hpp"

using namespace lancast;

TEST(Audio, MicrophoneCaptureDelivers) {
    std::atomic<std::uint32_t> frames{0};
    std::atomic<std::uint32_t> rate{0};
    audio::LoopbackCapture mic;
    mic.start(
        [&](std::span<const std::int16_t> pcm, std::uint32_t n, const audio::Format& fmt) {
            EXPECT_EQ(pcm.size(), static_cast<std::size_t>(n) * 2);  // Siempre estéreo intercalado.
            frames += n;
            rate = fmt.sample_rate;
        },
        audio::LoopbackCapture::Source::Microphone);
    for (int i = 0; i < 100 && frames < 4800; ++i) Sleep(20);
    mic.stop();
    if (frames == 0) GTEST_SKIP() << "Este equipo no tiene micrófono";
    EXPECT_GE(rate.load(), 8000u);
}

TEST(Audio, PlayerOpensVirtualCable) {
    const auto dev = audio::virtual_mic_device();
    if (!dev) GTEST_SKIP() << "Sin cable de audio virtual (VB-Cable)";
    audio::Player p;
    p.set_target_ms(20);
    p.start({48000, 1}, dev->id);
    std::vector<std::int16_t> tone(480);
    for (int i = 0; i < 50; ++i) {
        p.push(tone);
        Sleep(10);
    }
    EXPECT_GT(p.buffered_ms(), 0.0);
    p.stop();
}
