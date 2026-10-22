// Paquetización + FEC + reensamblado con un simulador de pérdida de paquetes.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "lancast/core/loss_meter.hpp"
#include "lancast/core/media.hpp"

using namespace lancast;
using namespace lancast::media;

namespace {

struct Packet {
    std::vector<std::uint8_t> bytes;
};

std::vector<Packet> make_packets(const std::vector<std::uint8_t>& frame, std::uint32_t frame_id, int fec, bool key = false,
                                 std::uint16_t epoch = 0) {
    std::vector<Packet> out;
    EncodedFrameInfo info{frame_id, key, 1234567, 890, epoch};
    packetize_frame(frame, info, fec, [&](std::span<const std::uint8_t> payload) {
        out.push_back(Packet{{payload.begin(), payload.end()}});
    });
    return out;
}

std::optional<AssembledFrame> feed(FrameAssembler& a, const Packet& p) {
    ByteReader r(p.bytes);
    const auto h = VideoShardHeader::read(r);
    EXPECT_TRUE(h.has_value());
    if (!h) return std::nullopt;
    return a.add(*h, r.rest());
}

std::vector<std::uint8_t> random_frame(std::size_t size, std::mt19937& rng) {
    std::vector<std::uint8_t> f(size);
    std::uniform_int_distribution<int> byte(0, 255);
    for (auto& b : f) b = static_cast<std::uint8_t>(byte(rng));
    return f;
}

}  // namespace

TEST(Packetizer, DatagramsFitInMtu) {
    std::mt19937 rng(1);
    const auto frame = random_frame(500'000, rng);
    for (const auto& p : make_packets(frame, 1, 20)) {
        EXPECT_EQ(p.bytes.size(), kVideoShardHeaderSize + kShardSize);
        EXPECT_LE(p.bytes.size() + kPacketHeaderSize + kTagSize, kMaxDatagram);
    }
}

TEST(Packetizer, BlockLayoutSplitsLargeFrames) {
    const auto small = block_layout(1000);
    EXPECT_EQ(small.block_count, 1);
    EXPECT_EQ(small.total_shards, 1);

    const auto large = block_layout(kShardSize * 250 + 1);
    EXPECT_EQ(large.total_shards, 251);
    EXPECT_EQ(large.block_count, 3);
    EXPECT_LE(large.shards_per_block, kMaxDataShardsPerBlock);
}

TEST(Assembler, ReassemblesInAnyOrderWithoutLoss) {
    std::mt19937 rng(2);
    for (const std::size_t size : {std::size_t{1}, std::size_t{1340}, std::size_t{1341}, std::size_t{200'000}}) {
        const auto frame = random_frame(size, rng);
        auto packets = make_packets(frame, 5, 15, true);
        std::shuffle(packets.begin(), packets.end(), rng);

        FrameAssembler a;
        std::optional<AssembledFrame> out;
        for (const auto& p : packets) {
            if (auto f = feed(a, p)) out = std::move(f);
        }
        ASSERT_TRUE(out.has_value()) << size;
        EXPECT_EQ(out->data, frame);
        EXPECT_TRUE(out->info.keyframe);
        EXPECT_EQ(out->info.capture_time_us, 1234567u);
        EXPECT_EQ(out->info.encode_time_us, 890u);
    }
}

TEST(Assembler, RecoversLostPacketsWithParity) {
    std::mt19937 rng(3);
    const auto frame = random_frame(300'000, rng);
    auto packets = make_packets(frame, 9, 20);

    // Pierde 1 de cada 10 paquetes (10% < 20% de paridad).
    FrameAssembler a;
    std::optional<AssembledFrame> out;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (i % 10 == 3) continue;
        if (auto f = feed(a, packets[i])) out = std::move(f);
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->data, frame);
    EXPECT_TRUE(out->recovered_by_fec);
    EXPECT_EQ(a.stats().frames_recovered, 1u);
}

TEST(Assembler, ReportsUnrecoverableFramesAsLost) {
    std::mt19937 rng(4);
    FrameAssembler a;
    const auto f1 = random_frame(50'000, rng);
    const auto f2 = random_frame(50'000, rng);
    const auto f3 = random_frame(50'000, rng);

    for (const auto& p : make_packets(f1, 1, 10)) (void)feed(a, p);
    auto p2 = make_packets(f2, 2, 10);
    for (std::size_t i = 0; i < p2.size() / 2; ++i) (void)feed(a, p2[i]);  // Mitad perdida: irrecuperable.
    std::optional<AssembledFrame> out;
    for (const auto& p : make_packets(f3, 3, 10)) {
        if (auto f = feed(a, p)) out = std::move(f);
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->info.frame_id, 3u);
    EXPECT_EQ(a.take_lost(), 1u);
    EXPECT_EQ(a.take_lost(), 0u);
}

TEST(Assembler, IgnoresDuplicatesAndLatePackets) {
    std::mt19937 rng(5);
    FrameAssembler a;
    const auto f1 = random_frame(5000, rng);
    auto packets = make_packets(f1, 1, 0);
    int delivered = 0;
    for (int round = 0; round < 2; ++round) {
        for (const auto& p : packets) delivered += feed(a, p).has_value() ? 1 : 0;
    }
    EXPECT_EQ(delivered, 1);
}

TEST(Assembler, NewEpochDiscardsOldFrames) {
    std::mt19937 rng(6);
    FrameAssembler a;
    const auto f = random_frame(5000, rng);
    for (const auto& p : make_packets(f, 100, 0, false, 1)) (void)feed(a, p);
    std::optional<AssembledFrame> out;
    for (const auto& p : make_packets(f, 1, 0, true, 2)) {
        if (auto r = feed(a, p)) out = std::move(r);
    }
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->info.epoch, 2);
    // Paquetes de la época anterior se descartan.
    for (const auto& p : make_packets(f, 101, 0, false, 1)) EXPECT_FALSE(feed(a, p).has_value());
}

// Criterio de aceptación: FEC 15% y 1% de pérdida aleatoria -> prácticamente ningún frame perdido.
TEST(LossSimulator, FifteenPercentFecHandlesOnePercentLoss) {
    std::mt19937 rng(7);
    std::bernoulli_distribution drop(0.01);
    FrameAssembler a;
    int completed = 0;
    constexpr int kFrames = 2000;
    for (int i = 0; i < kFrames; ++i) {
        // Frames de ~170 KB (1440p144 a 200 Mbps) con un keyframe grande cada 500.
        const auto frame = random_frame(i % 500 == 0 ? 800'000 : 170'000, rng);
        for (const auto& p : make_packets(frame, static_cast<std::uint32_t>(i + 1), 15, i % 500 == 0)) {
            if (drop(rng)) continue;
            if (auto f = feed(a, p)) {
                EXPECT_EQ(f->data, frame);
                ++completed;
            }
        }
    }
    EXPECT_GE(completed, kFrames - 2) << "perdidos: " << a.stats().frames_lost;
    EXPECT_GT(a.stats().frames_recovered, 0u);
}

// --- Envío por slices -----------------------------------------------------------------------------

namespace {

std::vector<Packet> slice_packets(const std::vector<std::uint8_t>& data, std::uint32_t unit_id, std::uint8_t slice,
                                  bool last, std::uint16_t epoch = 0) {
    std::vector<Packet> out;
    EncodedFrameInfo info{unit_id, slice == 0, 100, 5, epoch};
    info.slice = true;
    info.slice_index = slice;
    info.last_slice = last;
    packetize_frame(data, info, 10, [&](std::span<const std::uint8_t> p) { out.push_back(Packet{{p.begin(), p.end()}}); });
    return out;
}

}  // namespace

TEST(SliceJoiner, JoinsSlicesIntoOneFrame) {
    std::mt19937 rng(11);
    const auto s0 = random_frame(30'000, rng);
    const auto s1 = random_frame(20'000, rng);
    const auto s2 = random_frame(25'000, rng);
    FrameAssembler a;
    SliceJoiner j;
    std::vector<AssembledFrame> frames;
    std::uint32_t id = 1;
    for (const auto& [data, idx, last] : {std::tuple{&s0, 0, false}, std::tuple{&s1, 1, false}, std::tuple{&s2, 2, true}}) {
        for (const auto& p : slice_packets(*data, id, static_cast<std::uint8_t>(idx), last)) {
            if (auto unit = feed(a, p)) {
                if (auto f = j.add(std::move(*unit))) frames.push_back(std::move(*f));
            }
        }
        ++id;
    }
    ASSERT_EQ(frames.size(), 1u);
    std::vector<std::uint8_t> expected = s0;
    expected.insert(expected.end(), s1.begin(), s1.end());
    expected.insert(expected.end(), s2.begin(), s2.end());
    EXPECT_EQ(frames[0].data, expected);
    EXPECT_TRUE(frames[0].info.keyframe);
    EXPECT_FALSE(frames[0].info.slice);
    EXPECT_EQ(j.take_lost(), 0u);
}

TEST(SliceJoiner, DropsFrameWithMissingSlice) {
    std::mt19937 rng(12);
    FrameAssembler a;
    SliceJoiner j;
    int delivered = 0;
    // Cuadro 1: slices 0 y 2 (falta el 1) -> perdido. Cuadro 2 completo -> entregado.
    for (const auto& [id, idx, last, skip] : {std::tuple{1u, 0, false, false}, std::tuple{2u, 1, false, true},
                                              std::tuple{3u, 2, true, false}, std::tuple{4u, 0, false, false},
                                              std::tuple{5u, 1, true, false}}) {
        if (skip) continue;
        for (const auto& p : slice_packets(random_frame(8'000, rng), id, static_cast<std::uint8_t>(idx), last)) {
            if (auto unit = feed(a, p)) {
                if (j.add(std::move(*unit))) ++delivered;
            }
        }
    }
    EXPECT_EQ(delivered, 1);
    EXPECT_GE(j.take_lost() + a.take_lost(), 1u);
}

// Pérdida medida por secuencia: el desorden (audio que se adelanta a una tanda de video) no cuenta como pérdida;
// lo que de verdad no llega, sí.
TEST(LossMeter, ReorderIsNotLoss) {
    LossMeter m(64);
    // 1..1000 con tandas desordenadas: cada 20 paquetes, uno llega 10 lugares antes.
    std::vector<std::uint64_t> order;
    for (std::uint64_t s = 1; s <= 1000; ++s) order.push_back(s);
    for (std::size_t i = 10; i + 10 < order.size(); i += 20) std::swap(order[i], order[i + 10]);
    for (const auto s : order) m.received(s);
    const auto c = m.take();
    EXPECT_EQ(c.lost, 0u);
    EXPECT_GE(c.expected, 900u);
}

TEST(LossMeter, CountsRealLoss) {
    LossMeter m(64);
    std::mt19937 rng(3);
    std::uint64_t dropped = 0;
    for (std::uint64_t s = 1; s <= 20'000; ++s) {
        if (s <= 19'900 && rng() % 100 < 5) {  // 5 % (los últimos no se pueden decidir todavía)
            ++dropped;
            continue;
        }
        m.received(s);
    }
    const auto c = m.take();
    EXPECT_EQ(c.lost, dropped);
    EXPECT_NEAR(c.percent(), 5.0, 0.6);
    EXPECT_EQ(m.take().expected, 0u) << "take() reinicia la cuenta";
}

TEST(LossMeter, LateBeyondGraceAndRestart) {
    LossMeter m(4);
    for (std::uint64_t s : {1, 2, 4, 5, 6, 7, 8, 3}) m.received(s);  // El 3 llega tarde: ya se contó perdido.
    EXPECT_EQ(m.take().lost, 1u);
    // El host vuelve a empezar desde 1 (nueva sesión): no es una pérdida enorme.
    for (std::uint64_t s = 200'000; s < 200'010; ++s) m.received(s);
    m.take();
    for (std::uint64_t s = 1; s <= 20; ++s) m.received(s);
    EXPECT_EQ(m.take().lost, 0u);
}
