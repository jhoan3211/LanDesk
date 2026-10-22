// Micrófono del cliente: la captura del micrófono predeterminado entrega audio, y el reproductor puede abrir una
// salida concreta (la del cable de audio virtual del host).

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

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

namespace {

// Pasa a 8 kHz mono (promedio de las muestras que caen en cada intervalo).
std::vector<float> to_8k(const std::vector<float>& in, std::uint32_t rate) {
    std::vector<float> out;
    const double step = rate / 8000.0;
    for (double pos = 0; pos + step <= in.size(); pos += step) {
        double acc = 0;
        int n = 0;
        for (auto i = static_cast<std::size_t>(pos); i < static_cast<std::size_t>(pos + step); ++i, ++n) acc += in[i];
        out.push_back(n ? static_cast<float>(acc / n) : 0.0f);
    }
    return out;
}

struct EchoResult {
    double correlation = 0;  // 0..1: cuánto de la grabación se parece al sonido reproducido.
    double level_db = -99;   // Nivel de lo grabado mientras sonaba, respecto del sonido enviado.
    bool aec = false;
    std::uint32_t frames = 0;
};

// Reproduce un barrido de 300 a 3400 Hz por la salida predeterminada (como el audio del host en el cliente) y graba
// el micrófono a la vez (como el micrófono del cliente).
EchoResult measure_echo(bool raw, bool loopback = false, bool lancast_aec = false) {
    constexpr std::uint32_t kRate = 48000;
    // Tres barridos: los dos primeros dejan que el cancelador se adapte; se mide el último.
    constexpr double kSecs = 1.2;
    constexpr int kSweeps = 3;
    const auto sweep_len = static_cast<std::size_t>(kRate * kSecs);
    std::vector<float> ref(sweep_len * kSweeps);
    double phase = 0;
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const double t = static_cast<double>(i % sweep_len) / kRate;
        phase += 2 * 3.14159265358979 * (300 + (3400 - 300) * t / kSecs) / kRate;
        ref[i] = static_cast<float>(0.3 * std::sin(phase));
    }

    std::mutex m;
    std::vector<float> rec;
    std::uint32_t rec_rate = 0;
    // Cancelador de LanCast: referencia = lo que entrega el reproductor (misma frecuencia que el micrófono, 48 kHz).
    std::unique_ptr<audio::EchoCanceller> aec;
    if (lancast_aec) aec = std::make_unique<audio::EchoCanceller>(kRate);
    std::vector<std::int16_t> mono;
    std::vector<std::int16_t> clean;
    audio::LoopbackCapture mic;
    mic.set_raw_microphone(raw);
    mic.start(
        [&](std::span<const std::int16_t> pcm, std::uint32_t n, const audio::Format& fmt) {
            std::scoped_lock lock(m);
            rec_rate = fmt.sample_rate;
            if (aec && fmt.sample_rate == kRate) {
                mono.resize(n);
                for (std::uint32_t f = 0; f < n; ++f) mono[f] = static_cast<std::int16_t>((pcm[2 * f] + pcm[2 * f + 1]) / 2);
                clean.clear();
                aec->capture(mono, clean);
                for (const std::int16_t v : clean) rec.push_back(v / 32768.0f);
                return;
            }
            for (std::uint32_t f = 0; f < n; ++f) rec.push_back((pcm[2 * f] + pcm[2 * f + 1]) / 65536.0f);
        },
        loopback ? audio::LoopbackCapture::Source::Loopback : audio::LoopbackCapture::Source::Microphone);
    Sleep(400);  // Que arranque (y que el cancelador de eco se acomode).
    {
        std::scoped_lock lock(m);
        rec.clear();
    }
    audio::Player out;
    out.set_target_ms(20);
    std::vector<std::int16_t> tap_mono;
    if (aec) {
        out.set_tap([&](std::span<const std::int16_t> pcm, const audio::Format& fmt) {
            tap_mono.resize(pcm.size() / fmt.channels);
            for (std::size_t f = 0; f < tap_mono.size(); ++f) tap_mono[f] = pcm[f * fmt.channels];
            aec->playback(tap_mono);
        });
    }
    out.start({kRate, 2});
    // Al ritmo del reloj (con 40 ms de adelanto), como llega el audio del host.
    std::vector<std::int16_t> chunk;
    const ULONGLONG t0 = GetTickCount64();
    std::size_t off = 0;
    while (off < ref.size()) {
        const auto due = std::min(ref.size(), static_cast<std::size_t>((GetTickCount64() - t0 + 40) * kRate / 1000));
        chunk.clear();
        for (; off < due; ++off) {
            const auto s = static_cast<std::int16_t>(ref[off] * 32767);
            chunk.push_back(s);
            chunk.push_back(s);
        }
        if (!chunk.empty()) out.push(chunk);
        Sleep(2);
    }
    Sleep(600);
    out.stop();
    EchoResult r;
    r.aec = mic.echo_cancellation();
    mic.stop();

    std::scoped_lock lock(m);
    r.frames = static_cast<std::uint32_t>(rec.size());
    if (rec.empty() || rec_rate == 0) return r;
    const auto all = to_8k(ref, kRate);
    const std::size_t len8 = all.size() / kSweeps;
    const std::vector<float> x(all.end() - static_cast<std::ptrdiff_t>(len8), all.end());  // Último barrido.
    const auto y = to_8k(rec, rec_rate);
    double ex = 0;
    for (float v : x) ex += static_cast<double>(v) * v;
    // Retraso de 0 a 500 ms (salida + entrada de audio + aire) respecto de donde empezó el último barrido.
    double best_gain = 0;
    for (std::size_t lag = len8 * (kSweeps - 1); lag < len8 * (kSweeps - 1) + 4000 && lag + x.size() <= y.size(); ++lag) {
        double xy = 0;
        double ey = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            xy += static_cast<double>(x[i]) * y[lag + i];
            ey += static_cast<double>(y[lag + i]) * y[lag + i];
        }
        if (ey <= 0) continue;
        const double c = std::abs(xy) / std::sqrt(ex * ey);
        if (c > r.correlation) {
            r.correlation = c;
            best_gain = std::abs(xy) / ex;  // Amplitud del eco respecto de lo reproducido (sin contar el ruido).
        }
    }
    r.level_db = best_gain > 0 ? 20 * std::log10(best_gain) : -99;
    return r;
}

}  // namespace

// Cancelador con un eco fuerte simulado (parlantes altos cerca del micrófono): lo reproducido vuelve 40 ms después,
// a la mitad de volumen y algo filtrado, con ruido de fondo. Después de adaptarse debe quitar casi todo el eco.
TEST(Audio, EchoCancellerRemovesStrongEcho) {
    constexpr std::uint32_t kRate = 48000;
    audio::EchoCanceller aec(kRate);
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 30.0f);
    std::uniform_real_distribution<float> music(-1.0f, 1.0f);
    const std::size_t delay = kRate * 40 / 1000;
    std::vector<float> played;  // Historial de lo reproducido.
    double echo_in = 0;
    double echo_out = 0;
    float lp = 0;
    std::vector<std::int16_t> out;
    const std::size_t block = kRate / 100;
    for (int b = 0; b < 600; ++b) {  // 6 s en bloques de 10 ms.
        std::vector<std::int16_t> spk(block);
        std::vector<std::int16_t> mic(block);
        for (std::size_t i = 0; i < block; ++i) {
            const float s = 6000.0f * music(rng);
            spk[i] = static_cast<std::int16_t>(s);
            played.push_back(s);
            const std::size_t k = played.size() - 1;
            lp = 0.7f * lp + 0.3f * (k >= delay ? played[k - delay] : 0.0f);  // Parlante + sala: filtro y retraso.
            const float echo = 0.5f * lp;
            mic[i] = static_cast<std::int16_t>(std::clamp(echo + noise(rng), -32768.0f, 32767.0f));
            if (b >= 400) echo_in += static_cast<double>(echo) * echo;
        }
        aec.playback(spk);
        out.clear();
        aec.capture(mic, out);
        if (b >= 400) {
            for (const std::int16_t v : out) echo_out += static_cast<double>(v) * v;
        }
    }
    const double reduction_db = 10 * std::log10(echo_in / std::max(echo_out, 1.0));
    std::printf("  Eco fuerte simulado: el cancelador lo baja %.1f dB\n", reduction_db);
    EXPECT_GT(reduction_db, 20.0) << "El cancelador debe quitar la mayor parte del eco";
}

// Eco acústico en el cliente: el audio del host sale por los parlantes y el micrófono lo puede captar y devolver.
// Suena un barrido de 1.2 s, por eso solo corre con LANCAST_BENCH=1.
TEST(Audio, EchoMeasurement) {
    wchar_t env[8]{};
    if (GetEnvironmentVariableW(L"LANCAST_BENCH", env, 8) == 0) GTEST_SKIP() << "Reproduce sonido: LANCAST_BENCH=1";
    // Control: grabar la salida misma debe dar correlación ~1 (la medición funciona).
    const EchoResult control = measure_echo(true, true);
    std::printf("  Control (grabando la salida directamente): correlación %.2f\n", control.correlation);
    EXPECT_GT(control.correlation, 0.5) << "La medición no detecta el barrido";
    const EchoResult raw = measure_echo(true);
    if (raw.frames == 0) GTEST_SKIP() << "Este equipo no tiene micrófono";
    Sleep(300);
    const EchoResult comm = measure_echo(false);
    std::printf("  Micrófono sin procesar:    correlación con lo reproducido %.2f · eco %.1f dB\n", raw.correlation, raw.level_db);
    std::printf("  Modo comunicaciones (LanCast): correlación %.2f · eco %.1f dB · cancelación de eco de Windows: %s\n",
                comm.correlation, comm.level_db, comm.aec ? "sí" : "no");
    Sleep(300);
    const EchoResult own = measure_echo(true, false, true);
    std::printf("  Cancelador de eco de LanCast: correlación %.2f · eco %.1f dB\n", own.correlation, own.level_db);
    std::printf("  (correlación < 0.15 = no se oye el eco; > 0.3 = el micrófono capta los parlantes)\n");
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
