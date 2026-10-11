#pragma once

// Formato de los datagramas UDP de medios y paquetización de frames de video con FEC por bloques.
//
// Datagrama (máx. kMaxDatagram bytes):
//   [PacketHeader 12 B (en claro, también es el nonce y el AAD de AES-GCM)]
//   [carga cifrada][tag GCM 16 B]
//
// Carga de video: [VideoShardHeader 32 B][fragmento de kShardSize bytes]

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "lancast/core/bytes.hpp"

namespace lancast::media {

inline constexpr std::size_t kMaxDatagram = 1400;
inline constexpr std::size_t kPacketHeaderSize = 12;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kVideoShardHeaderSize = 32;
inline constexpr std::size_t kShardSize = kMaxDatagram - kPacketHeaderSize - kTagSize - kVideoShardHeaderSize;
inline constexpr int kMaxDataShardsPerBlock = 100;

enum class PacketType : std::uint8_t {
    Video = 1,
    Audio = 2,
    ClientHello = 3,  // Cliente -> host: registra el endpoint UDP y lo mantiene vivo.
};

struct PacketHeader {
    PacketType type = PacketType::Video;
    std::uint64_t seq = 0;

    void write(std::uint8_t* out) const noexcept;
    static std::optional<PacketHeader> read(std::span<const std::uint8_t> in) noexcept;
};

inline constexpr std::uint8_t kFrameFlagKeyframe = 0x01;
// Envío por slices: cada slice viaja como una unidad propia (frame_id único) y VideoShardHeader::reserved
// lleva el índice del slice y si es el último del cuadro.
inline constexpr std::uint8_t kFrameFlagSlice = 0x02;
inline constexpr std::uint16_t kSliceLast = 0x8000;

struct VideoShardHeader {
    std::uint32_t frame_id = 0;
    std::uint16_t block_index = 0;
    std::uint16_t block_count = 0;
    std::uint8_t shard_index = 0;
    std::uint8_t data_shards = 0;
    std::uint8_t parity_shards = 0;
    std::uint8_t flags = 0;
    std::uint32_t frame_size = 0;
    std::uint64_t capture_time_us = 0;  // Reloj del host (QPC en µs).
    std::uint32_t encode_time_us = 0;   // Captura -> bitstream listo.
    std::uint16_t epoch = 0;            // Cambia en cada reconfiguración del stream.
    std::uint16_t reserved = 0;

    void write(ByteWriter& w) const;
    static std::optional<VideoShardHeader> read(ByteReader& r) noexcept;
};

struct EncodedFrameInfo {
    std::uint32_t frame_id = 0;
    bool keyframe = false;
    std::uint64_t capture_time_us = 0;
    std::uint32_t encode_time_us = 0;
    std::uint16_t epoch = 0;
    bool slice = false;         // Unidad parcial de un cuadro (modo slices).
    std::uint8_t slice_index = 0;
    bool last_slice = true;
};

// Divide un frame en bloques de fragmentos con paridad Reed-Solomon. Cada carga emitida
// (cabecera + fragmento) va a un datagrama.
void packetize_frame(std::span<const std::uint8_t> frame, const EncodedFrameInfo& info, int fec_percent,
                     const std::function<void(std::span<const std::uint8_t> payload)>& emit);

struct AssembledFrame {
    std::vector<std::uint8_t> data;
    EncodedFrameInfo info;
    bool recovered_by_fec = false;
};

struct AssemblerStats {
    std::uint64_t frames_completed = 0;
    std::uint64_t frames_recovered = 0;  // Completados gracias a la paridad.
    std::uint64_t frames_lost = 0;
    std::uint64_t shards_received = 0;
};

// Reensambla frames a partir de fragmentos (en cualquier orden, con duplicados y pérdidas).
class FrameAssembler {
public:
    // Devuelve un frame en cuanto todos sus bloques tienen al menos k fragmentos.
    std::optional<AssembledFrame> add(const VideoShardHeader& header, std::span<const std::uint8_t> shard);

    // Frames perdidos detectados desde la última llamada (huecos anteriores al último frame completado).
    [[nodiscard]] std::uint32_t take_lost() noexcept {
        const std::uint32_t n = pending_lost_;
        pending_lost_ = 0;
        return n;
    }
    [[nodiscard]] const AssemblerStats& stats() const noexcept { return stats_; }
    void reset();

private:
    struct Block {
        int k = 0;
        int m = 0;
        int received = 0;
        bool done = false;
        std::vector<std::uint8_t> buffer;  // (k + m) * kShardSize
        std::vector<bool> present;
    };
    struct Frame {
        VideoShardHeader header;
        std::vector<Block> blocks;
        int blocks_done = 0;
        bool used_parity = false;
    };

    std::map<std::uint32_t, Frame> frames_;
    std::optional<std::uint32_t> last_completed_;
    std::uint32_t pending_lost_ = 0;
    std::uint16_t epoch_ = 0;
    bool has_epoch_ = false;
    AssemblerStats stats_;
};

// Junta los slices de un cuadro (modo slices) y entrega el cuadro completo cuando llega el último.
// Si falta cualquier slice intermedio, el cuadro se descarta (cuenta como perdido).
class SliceJoiner {
public:
    // Devuelve un cuadro completo, o nada si aún faltan slices. Las unidades que no son slices pasan tal cual.
    std::optional<AssembledFrame> add(AssembledFrame unit);
    [[nodiscard]] std::uint32_t take_lost() noexcept {
        const std::uint32_t n = lost_;
        lost_ = 0;
        return n;
    }
    void reset() { pending_.reset(); }

private:
    std::optional<AssembledFrame> pending_;
    std::uint32_t expected_unit_ = 0;
    std::uint8_t expected_slice_ = 0;
    std::uint32_t lost_ = 0;
};

// Calcula k para cada bloque: reparte uniformemente los fragmentos de datos.
struct BlockLayout {
    int block_count = 0;
    int shards_per_block = 0;  // Los primeros bloques; el último puede tener menos.
    int total_shards = 0;
};
[[nodiscard]] BlockLayout block_layout(std::size_t frame_size) noexcept;

}  // namespace lancast::media
