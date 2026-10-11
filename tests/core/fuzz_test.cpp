// Fuzzing por mutación aleatoria (semilla fija, reproducible): los lectores de mensajes, la cabecera de
// video, el reensamblador y los parsers de TOML/JSON no deben fallar ni colgarse con entradas basura.

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "lancast/core/json_reader.hpp"
#include "lancast/core/media.hpp"
#include "lancast/core/protocol.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/core/toml.hpp"

using namespace lancast;

namespace {

constexpr int kIterations = 4000;

class Mutator {
public:
    explicit Mutator(std::uint32_t seed) : rng_(seed) {}

    int below(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng_); }
    std::uint8_t byte() { return static_cast<std::uint8_t>(below(256)); }

    std::vector<std::uint8_t> random_bytes(int max_len) {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(below(max_len + 1)));
        for (auto& b : out) b = byte();
        return out;
    }

    // Aplica de 1 a 4 mutaciones: cambiar bits, truncar, insertar, borrar o poner valores extremos.
    std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> in) {
        const int count = 1 + below(4);
        for (int i = 0; i < count; ++i) {
            const int op = below(6);
            if (in.empty() && op != 2) {
                in.push_back(byte());
                continue;
            }
            const auto pos = static_cast<std::size_t>(below(static_cast<int>(in.size())));
            switch (op) {
                case 0: in[pos] ^= static_cast<std::uint8_t>(1u << below(8)); break;
                case 1: in.resize(pos); break;
                case 2: in.insert(in.begin() + static_cast<std::ptrdiff_t>(in.empty() ? 0 : pos), byte()); break;
                case 3: in.erase(in.begin() + static_cast<std::ptrdiff_t>(pos)); break;
                case 4: in[pos] = 0xFF; break;
                default: in[pos] = 0x00; break;
            }
        }
        return in;
    }

    std::string mutate_text(std::string in) {
        static constexpr char kInteresting[] = "[]{}\"'=\\,.:-+#\n\t0123456789eEtrufalsn \x01\xff";
        const int count = 1 + below(4);
        for (int i = 0; i < count; ++i) {
            const int op = below(3);
            const auto pos = in.empty() ? 0 : static_cast<std::size_t>(below(static_cast<int>(in.size())));
            const char c = kInteresting[below(static_cast<int>(sizeof(kInteresting) - 1))];
            if (op == 0 && !in.empty()) {
                in[pos] = c;
            } else if (op == 1) {
                in.insert(in.begin() + static_cast<std::ptrdiff_t>(pos), c);
            } else if (!in.empty()) {
                in.erase(pos, static_cast<std::size_t>(1 + below(8)));
            }
        }
        return in;
    }

private:
    std::mt19937 rng_;
};

// Cuerpo (sin el byte de tipo) de un mensaje válido.
template <typename T>
std::vector<std::uint8_t> body_of(proto::MsgType type, const T& msg) {
    auto bytes = proto::encode(type, msg);
    bytes.erase(bytes.begin());
    return bytes;
}

template <typename T>
void fuzz_reader(Mutator& m, const std::vector<std::uint8_t>& valid) {
    {
        ByteReader r(valid);
        EXPECT_TRUE(T::read(r).has_value()) << "La semilla válida debe leerse";
    }
    for (int i = 0; i < kIterations / 8; ++i) {
        const auto input = (i % 4 == 0) ? m.random_bytes(96) : m.mutate(valid);
        ByteReader r(input);
        (void)T::read(r);  // Solo importa que no falle ni lea fuera del búfer.
    }
}

}  // namespace

TEST(Fuzz, ProtocolReadersSurviveGarbage) {
    Mutator m(0xC0FFEE);

    proto::Hello hello;
    hello.device_name = "Equipo";
    fuzz_reader<proto::Hello>(m, body_of(proto::MsgType::Hello, hello));

    proto::ClientCaps caps;
    caps.client_name = "Cliente";
    caps.decoders.push_back(Codec::Hevc);
    caps.supports_slices = true;
    fuzz_reader<proto::ClientCaps>(m, body_of(proto::MsgType::ClientCaps, caps));

    proto::HostInfo info;
    info.host_name = "Host";
    proto::RemoteMonitor mon;
    mon.id = 1;
    mon.name = "Monitor";
    info.monitors.push_back(mon);
    fuzz_reader<proto::HostInfo>(m, body_of(proto::MsgType::HostInfo, info));

    proto::StreamConfig cfg;
    cfg.stream_index = 1;
    fuzz_reader<proto::StreamConfig>(m, body_of(proto::MsgType::StreamConfig, cfg));

    proto::StreamStarted started;
    started.encoder_backend = "AMF";
    fuzz_reader<proto::StreamStarted>(m, body_of(proto::MsgType::StreamStarted, started));

    fuzz_reader<proto::TextMessage>(m, body_of(proto::MsgType::StreamError, proto::TextMessage{"error"}));
    fuzz_reader<proto::PingPong>(m, body_of(proto::MsgType::Ping, proto::PingPong{}));
    fuzz_reader<proto::MouseAbs>(m, body_of(proto::MsgType::MouseAbs, proto::MouseAbs{0.5f, 0.5f, 1}));
    fuzz_reader<proto::MouseButton>(m, body_of(proto::MsgType::MouseButton, proto::MouseButton{}));
    fuzz_reader<proto::MouseWheel>(m, body_of(proto::MsgType::MouseWheel, proto::MouseWheel{120, false}));
    fuzz_reader<proto::Key>(m, body_of(proto::MsgType::Key, proto::Key{}));
    fuzz_reader<proto::CursorPos>(m, body_of(proto::MsgType::CursorPos, proto::CursorPos{}));
    fuzz_reader<proto::StreamStoppedMsg>(m, body_of(proto::MsgType::StreamStopped, proto::StreamStoppedMsg{1, "x"}));
    fuzz_reader<proto::LatencyProbe>(m, body_of(proto::MsgType::LatencyProbe, proto::LatencyProbe{}));
    fuzz_reader<proto::LatencyMark>(m, body_of(proto::MsgType::LatencyMark, proto::LatencyMark{}));

    proto::CursorShape shape;
    shape.width = 2;
    shape.height = 2;
    shape.pitch = 8;
    shape.data.assign(16, 0x80);
    fuzz_reader<proto::CursorShape>(m, body_of(proto::MsgType::CursorShape, shape));

    proto::FileOffer offer;
    offer.relative_path = "carpeta/archivo.bin";
    offer.size = 1234;
    fuzz_reader<proto::FileOffer>(m, body_of(proto::MsgType::FileOffer, offer));

    proto::DiscoveryReply reply;
    reply.host_name = "Host";
    fuzz_reader<proto::DiscoveryReply>(m, body_of(proto::MsgType::HostInfo, reply));
}

TEST(Fuzz, VideoHeaderAndAssemblerSurviveGarbage) {
    Mutator m(0xBADC0DE);
    std::vector<std::uint8_t> frame(5000);
    for (auto& b : frame) b = m.byte();
    std::vector<std::vector<std::uint8_t>> packets;
    media::packetize_frame(frame, media::EncodedFrameInfo{7, true, 1, 2, 3}, 20,
                           [&](std::span<const std::uint8_t> p) { packets.emplace_back(p.begin(), p.end()); });
    ASSERT_FALSE(packets.empty());

    media::FrameAssembler assembler;
    media::SliceJoiner joiner;
    for (int i = 0; i < kIterations; ++i) {
        const auto& seed = packets[static_cast<std::size_t>(m.below(static_cast<int>(packets.size())))];
        const auto input = (i % 5 == 0) ? m.random_bytes(1500) : m.mutate(seed);
        ByteReader r(input);
        const auto header = media::VideoShardHeader::read(r);
        if (!header) continue;
        if (auto out = assembler.add(*header, r.rest())) {
            (void)joiner.add(std::move(*out));
        }
        (void)assembler.take_lost();
        (void)joiner.take_lost();
    }

    // Después de la basura, un cuadro válido de otra época se sigue reensamblando.
    assembler.reset();
    std::optional<media::AssembledFrame> done;
    media::packetize_frame(frame, media::EncodedFrameInfo{1, true, 1, 2, 9}, 20, [&](std::span<const std::uint8_t> p) {
        ByteReader r(p);
        const auto h = media::VideoShardHeader::read(r);
        ASSERT_TRUE(h.has_value());
        if (auto out = assembler.add(*h, r.rest())) done = std::move(out);
    });
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->data, frame);
}

TEST(Fuzz, PacketHeaderSurvivesGarbage) {
    Mutator m(42);
    for (int i = 0; i < kIterations; ++i) {
        const auto input = m.random_bytes(24);
        (void)media::PacketHeader::read(input);
    }
}

TEST(Fuzz, TomlAndSettingsSurviveGarbage) {
    Mutator m(7);
    AppSettings defaults;
    defaults.device_name = "Equipo \"raro\" = [x]";
    const std::string valid = settings_to_toml(defaults);
    for (int i = 0; i < kIterations / 4; ++i) {
        const std::string input = m.mutate_text(valid);
        std::string error;
        (void)toml::Document::parse(input, &error);
        const AppSettings s = settings_from_toml(input);
        // Los valores cargados siempre quedan dentro de rangos válidos.
        EXPECT_GE(s.client.stream.fps, 10u);
        EXPECT_LE(s.client.stream.fps, 360u);
    }
}

TEST(Fuzz, JsonReaderSurvivesGarbage) {
    Mutator m(99);
    const std::string valid =
        R"({"tag_name":"v1.2.3","body":"SHA-256: abc\n","assets":[{"name":"LanCast.exe","size":123,"browser_download_url":"https://x"}],"draft":false,"n":-1.5e3,"u":"\u00e9"})";
    ASSERT_TRUE(json::parse(valid).has_value());
    for (int i = 0; i < kIterations; ++i) {
        (void)json::parse(m.mutate_text(valid));
    }
    // Anidamiento profundo: no debe desbordar la pila.
    EXPECT_FALSE(json::parse(std::string(100000, '[')).has_value());
    EXPECT_FALSE(json::parse(std::string(100000, '{')).has_value());
}
