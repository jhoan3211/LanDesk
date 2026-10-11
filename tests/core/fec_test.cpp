#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <random>

#include "lancast/core/fec.hpp"

using namespace lancast::fec;

namespace {

std::vector<std::vector<std::uint8_t>> random_shards(int count, std::size_t size, std::mt19937& rng) {
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::vector<std::uint8_t>> shards(static_cast<std::size_t>(count), std::vector<std::uint8_t>(size));
    for (auto& s : shards) {
        for (auto& b : s) b = static_cast<std::uint8_t>(byte(rng));
    }
    return shards;
}

}  // namespace

TEST(GaloisField, MultiplicationAndInverse) {
    for (int a = 1; a < 256; ++a) {
        EXPECT_EQ(gf_mul(static_cast<std::uint8_t>(a), gf_inv(static_cast<std::uint8_t>(a))), 1) << a;
        EXPECT_EQ(gf_mul(static_cast<std::uint8_t>(a), 1), a);
        EXPECT_EQ(gf_mul(static_cast<std::uint8_t>(a), 0), 0);
    }
    // Conmutatividad y distributividad sobre XOR.
    for (int a = 0; a < 256; a += 7) {
        for (int b = 0; b < 256; b += 11) {
            const auto ua = static_cast<std::uint8_t>(a);
            const auto ub = static_cast<std::uint8_t>(b);
            EXPECT_EQ(gf_mul(ua, ub), gf_mul(ub, ua));
            EXPECT_EQ(gf_mul(ua, static_cast<std::uint8_t>(ub ^ 0x5A)),
                      static_cast<std::uint8_t>(gf_mul(ua, ub) ^ gf_mul(ua, 0x5A)));
        }
    }
}

TEST(GaloisField, SimdRegionMatchesScalar) {
    std::mt19937 rng(42);
    for (const std::size_t size : {1u, 15u, 16u, 31u, 33u, 100u, 1340u}) {
        auto src = random_shards(1, size, rng)[0];
        auto dst = random_shards(1, size, rng)[0];
        for (const int c : {0, 1, 2, 0x53, 0xFF}) {
            auto expected = dst;
            for (std::size_t i = 0; i < size; ++i) expected[i] ^= gf_mul(static_cast<std::uint8_t>(c), src[i]);
            auto actual = dst;
            mul_add_region(actual.data(), src.data(), static_cast<std::uint8_t>(c), size);
            EXPECT_EQ(actual, expected) << "size=" << size << " c=" << c;
        }
    }
}

TEST(ReedSolomon, RecoversAnyCombinationOfLosses) {
    std::mt19937 rng(7);
    constexpr std::size_t kSize = 257;
    for (const auto [k, m] : {std::pair{1, 1}, std::pair{4, 2}, std::pair{10, 3}, std::pair{50, 8}, std::pair{100, 15},
                              std::pair{200, 55}}) {
        const ReedSolomon rs(k, m);
        auto data = random_shards(k, kSize, rng);
        std::vector<std::vector<std::uint8_t>> parity(static_cast<std::size_t>(m), std::vector<std::uint8_t>(kSize));
        std::vector<const std::uint8_t*> dp;
        std::vector<std::uint8_t*> pp;
        for (auto& d : data) dp.push_back(d.data());
        for (auto& p : parity) pp.push_back(p.data());
        rs.encode(dp, pp, kSize);

        for (int trial = 0; trial < 20; ++trial) {
            // Pierde exactamente m fragmentos aleatorios (el peor caso recuperable).
            std::vector<int> idx(static_cast<std::size_t>(k + m));
            std::iota(idx.begin(), idx.end(), 0);
            std::shuffle(idx.begin(), idx.end(), rng);

            std::vector<std::vector<std::uint8_t>> shards;
            for (auto& d : data) shards.push_back(d);
            for (auto& p : parity) shards.push_back(p);
            std::unique_ptr<bool[]> present(new bool[static_cast<std::size_t>(k + m)]);
            std::fill_n(present.get(), k + m, true);
            for (int i = 0; i < m; ++i) {
                const auto lost = static_cast<std::size_t>(idx[static_cast<std::size_t>(i)]);
                present[lost] = false;
                std::fill(shards[lost].begin(), shards[lost].end(), std::uint8_t{0xEE});
            }
            std::vector<std::uint8_t*> ptrs;
            for (auto& s : shards) ptrs.push_back(s.data());

            ASSERT_TRUE(rs.reconstruct(ptrs, {present.get(), static_cast<std::size_t>(k + m)}, kSize))
                << "k=" << k << " m=" << m;
            for (int j = 0; j < k; ++j) {
                ASSERT_EQ(shards[static_cast<std::size_t>(j)], data[static_cast<std::size_t>(j)])
                    << "k=" << k << " m=" << m << " shard " << j;
            }
        }
    }
}

TEST(ReedSolomon, FailsWithTooFewShards) {
    const ReedSolomon rs(4, 2);
    std::vector<std::vector<std::uint8_t>> shards(6, std::vector<std::uint8_t>(8, 1));
    std::vector<std::uint8_t*> ptrs;
    for (auto& s : shards) ptrs.push_back(s.data());
    const bool present[6] = {true, false, false, true, false, true};
    EXPECT_FALSE(rs.reconstruct(ptrs, present, 8));
}

TEST(ReedSolomon, ParityCount) {
    EXPECT_EQ(parity_for(10, 0), 0);
    EXPECT_EQ(parity_for(1, 15), 1);
    EXPECT_EQ(parity_for(10, 15), 2);
    EXPECT_EQ(parity_for(100, 20), 20);
    EXPECT_EQ(parity_for(250, 50), 5);  // Limitado por k + m <= 255.
}
