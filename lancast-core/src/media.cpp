#include "lancast/core/media.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#include "lancast/core/fec.hpp"

namespace lancast::media {

void PacketHeader::write(std::uint8_t* out) const noexcept {
    out[0] = static_cast<std::uint8_t>(type);
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<std::uint8_t>(seq >> (8 * i));
}

std::optional<PacketHeader> PacketHeader::read(std::span<const std::uint8_t> in) noexcept {
    if (in.size() < kPacketHeaderSize) return std::nullopt;
    PacketHeader h;
    h.type = static_cast<PacketType>(in[0]);
    for (int i = 0; i < 8; ++i) h.seq |= static_cast<std::uint64_t>(in[4 + static_cast<std::size_t>(i)]) << (8 * i);
    return h;
}

void VideoShardHeader::write(ByteWriter& w) const {
    w.u32(frame_id)
        .u16(block_index)
        .u16(block_count)
        .u8(shard_index)
        .u8(data_shards)
        .u8(parity_shards)
        .u8(flags)
        .u32(frame_size)
        .u64(capture_time_us)
        .u32(encode_time_us)
        .u16(epoch)
        .u16(reserved);
}

std::optional<VideoShardHeader> VideoShardHeader::read(ByteReader& r) noexcept {
    VideoShardHeader h;
    h.frame_id = r.u32();
    h.block_index = r.u16();
    h.block_count = r.u16();
    h.shard_index = r.u8();
    h.data_shards = r.u8();
    h.parity_shards = r.u8();
    h.flags = r.u8();
    h.frame_size = r.u32();
    h.capture_time_us = r.u64();
    h.encode_time_us = r.u32();
    h.epoch = r.u16();
    h.reserved = r.u16();
    if (!r.ok() || h.block_count == 0 || h.block_index >= h.block_count || h.data_shards == 0 ||
        static_cast<int>(h.data_shards) + h.parity_shards > fec::kMaxTotalShards ||
        h.shard_index >= h.data_shards + h.parity_shards || h.frame_size == 0 ||
        h.frame_size > 64u * 1024 * 1024) {
        return std::nullopt;
    }
    return h;
}

BlockLayout block_layout(std::size_t frame_size) noexcept {
    BlockLayout layout;
    layout.total_shards = static_cast<int>((frame_size + kShardSize - 1) / kShardSize);
    layout.block_count = (layout.total_shards + kMaxDataShardsPerBlock - 1) / kMaxDataShardsPerBlock;
    layout.shards_per_block = (layout.total_shards + layout.block_count - 1) / layout.block_count;
    return layout;
}

void packetize_frame(std::span<const std::uint8_t> frame, const EncodedFrameInfo& info, int fec_percent,
                     const std::function<void(std::span<const std::uint8_t>)>& emit) {
    if (frame.empty()) return;
    const BlockLayout layout = block_layout(frame.size());

    std::vector<std::uint8_t> block_buf;
    std::vector<const std::uint8_t*> data_ptrs;
    std::vector<std::uint8_t*> parity_ptrs;
    ByteWriter w(kVideoShardHeaderSize + kShardSize);

    for (int b = 0; b < layout.block_count; ++b) {
        const int first_shard = b * layout.shards_per_block;
        const int k = std::min(layout.shards_per_block, layout.total_shards - first_shard);
        const int m = fec::parity_for(k, fec_percent);

        // Copia los datos del bloque con relleno de ceros hasta k * kShardSize.
        block_buf.assign(static_cast<std::size_t>(k + m) * kShardSize, 0);
        const std::size_t offset = static_cast<std::size_t>(first_shard) * kShardSize;
        const std::size_t len = std::min(static_cast<std::size_t>(k) * kShardSize, frame.size() - offset);
        std::memcpy(block_buf.data(), frame.data() + offset, len);

        auto emit_shard = [&](int shard) {
            VideoShardHeader h;
            h.frame_id = info.frame_id;
            h.block_index = static_cast<std::uint16_t>(b);
            h.block_count = static_cast<std::uint16_t>(layout.block_count);
            h.shard_index = static_cast<std::uint8_t>(shard);
            h.data_shards = static_cast<std::uint8_t>(k);
            h.parity_shards = static_cast<std::uint8_t>(m);
            h.flags = static_cast<std::uint8_t>((info.keyframe ? kFrameFlagKeyframe : 0) | (info.slice ? kFrameFlagSlice : 0));
            h.reserved = info.slice ? static_cast<std::uint16_t>(info.slice_index | (info.last_slice ? kSliceLast : 0)) : 0;
            h.frame_size = static_cast<std::uint32_t>(frame.size());
            h.capture_time_us = info.capture_time_us;
            h.encode_time_us = info.encode_time_us;
            h.epoch = info.epoch;

            w.data().clear();
            h.write(w);
            w.raw({block_buf.data() + static_cast<std::size_t>(shard) * kShardSize, kShardSize});
            emit(w.data());
        };

        // Los fragmentos de datos salen ya; la paridad se calcula mientras viajan (menor latencia del primer paquete).
        for (int s = 0; s < k; ++s) emit_shard(s);
        if (m > 0) {
            data_ptrs.resize(static_cast<std::size_t>(k));
            parity_ptrs.resize(static_cast<std::size_t>(m));
            for (int i = 0; i < k; ++i) data_ptrs[static_cast<std::size_t>(i)] = block_buf.data() + static_cast<std::size_t>(i) * kShardSize;
            for (int i = 0; i < m; ++i) {
                parity_ptrs[static_cast<std::size_t>(i)] = block_buf.data() + static_cast<std::size_t>(k + i) * kShardSize;
            }
            fec::ReedSolomon(k, m).encode(data_ptrs, parity_ptrs, kShardSize);
            for (int s = k; s < k + m; ++s) emit_shard(s);
        }
    }
}

void FrameAssembler::reset() {
    frames_.clear();
    last_completed_.reset();
    pending_lost_ = 0;
}

namespace {
// Comparación con wrap-around de ids de frame (32 bits).
bool newer(std::uint32_t a, std::uint32_t b) noexcept { return static_cast<std::int32_t>(a - b) > 0; }
}  // namespace

std::optional<AssembledFrame> FrameAssembler::add(const VideoShardHeader& h, std::span<const std::uint8_t> shard) {
    if (shard.size() != kShardSize) return std::nullopt;
    ++stats_.shards_received;

    // Nueva época (reconfiguración): se descarta todo lo anterior.
    if (!has_epoch_ || h.epoch != epoch_) {
        if (has_epoch_ && static_cast<std::int16_t>(h.epoch - epoch_) < 0) return std::nullopt;  // Paquete viejo.
        has_epoch_ = true;
        epoch_ = h.epoch;
        frames_.clear();
        last_completed_.reset();
    }
    if (last_completed_ && !newer(h.frame_id, *last_completed_)) return std::nullopt;  // Tardío o duplicado.

    // Límite de frames incompletos en vuelo (se contabilizan como perdidos al completarse uno posterior).
    while (frames_.size() >= 32 && !frames_.contains(h.frame_id)) frames_.erase(frames_.begin());

    auto [it, inserted] = frames_.try_emplace(h.frame_id);
    Frame& frame = it->second;
    if (inserted) {
        frame.header = h;
        frame.blocks.resize(h.block_count);
    } else if (frame.header.block_count != h.block_count || frame.header.frame_size != h.frame_size) {
        return std::nullopt;
    }

    Block& block = frame.blocks[h.block_index];
    if (block.k == 0) {
        block.k = h.data_shards;
        block.m = h.parity_shards;
        block.buffer.assign(static_cast<std::size_t>(block.k + block.m) * kShardSize, 0);
        block.present.assign(static_cast<std::size_t>(block.k + block.m), false);
    } else if (block.k != h.data_shards || block.m != h.parity_shards) {
        return std::nullopt;
    }
    if (block.done || block.present[h.shard_index]) return std::nullopt;

    block.present[h.shard_index] = true;
    std::memcpy(block.buffer.data() + static_cast<std::size_t>(h.shard_index) * kShardSize, shard.data(), kShardSize);
    ++block.received;
    if (block.received < block.k) return std::nullopt;

    // Bloque decodificable.
    bool missing_data = false;
    for (int j = 0; j < block.k; ++j) missing_data = missing_data || !block.present[static_cast<std::size_t>(j)];
    if (missing_data) {
        std::vector<std::uint8_t*> ptrs(static_cast<std::size_t>(block.k + block.m));
        for (std::size_t i = 0; i < ptrs.size(); ++i) ptrs[i] = block.buffer.data() + i * kShardSize;
        std::vector<bool> present_copy = block.present;
        std::unique_ptr<bool[]> present(new bool[present_copy.size()]);
        for (std::size_t i = 0; i < present_copy.size(); ++i) present[i] = present_copy[i];
        if (!fec::ReedSolomon(block.k, block.m)
                 .reconstruct(ptrs, std::span<const bool>{present.get(), present_copy.size()}, kShardSize)) {
            return std::nullopt;
        }
        frame.used_parity = true;
    }
    block.done = true;
    if (++frame.blocks_done < static_cast<int>(frame.blocks.size())) return std::nullopt;

    // Frame completo: ensamblar datos en orden.
    AssembledFrame out;
    out.data.reserve(frame.header.frame_size);
    for (const Block& b : frame.blocks) {
        out.data.insert(out.data.end(), b.buffer.begin(),
                        b.buffer.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(b.k) * kShardSize));
    }
    out.data.resize(frame.header.frame_size);
    out.info.frame_id = frame.header.frame_id;
    out.info.keyframe = (frame.header.flags & kFrameFlagKeyframe) != 0;
    out.info.capture_time_us = frame.header.capture_time_us;
    out.info.encode_time_us = frame.header.encode_time_us;
    out.info.epoch = frame.header.epoch;
    out.info.slice = (frame.header.flags & kFrameFlagSlice) != 0;
    out.info.slice_index = static_cast<std::uint8_t>(frame.header.reserved & 0xFF);
    out.info.last_slice = !out.info.slice || (frame.header.reserved & kSliceLast) != 0;
    out.recovered_by_fec = frame.used_parity;

    // Los frames anteriores que no se completaron se consideran perdidos.
    std::uint32_t lost = 0;
    if (last_completed_) {
        lost = h.frame_id - *last_completed_ - 1;
    } else {
        for (const auto& [id, f] : frames_) {
            if (newer(h.frame_id, id)) ++lost;
        }
    }
    for (auto fit = frames_.begin(); fit != frames_.end();) {
        if (!newer(fit->first, h.frame_id)) {
            fit = frames_.erase(fit);
        } else {
            ++fit;
        }
    }
    last_completed_ = h.frame_id;
    pending_lost_ += lost;
    stats_.frames_lost += lost;
    ++stats_.frames_completed;
    if (out.recovered_by_fec) ++stats_.frames_recovered;
    return out;
}

std::optional<AssembledFrame> SliceJoiner::add(AssembledFrame unit) {
    if (pending_ && pending_->info.epoch != unit.info.epoch) pending_.reset();  // Reconfiguración: no es pérdida.
    if (!unit.info.slice) {
        if (pending_) {
            ++lost_;  // Cuadro por slices que quedó incompleto.
            pending_.reset();
        }
        return unit;
    }
    if (unit.info.slice_index == 0) {
        if (pending_) ++lost_;
        pending_ = std::move(unit);
    } else if (!pending_ || unit.info.frame_id != expected_unit_ || unit.info.slice_index != expected_slice_) {
        // Falta un slice intermedio: el cuadro no se puede decodificar.
        if (pending_) ++lost_;
        pending_.reset();
        return std::nullopt;
    } else {
        pending_->data.insert(pending_->data.end(), unit.data.begin(), unit.data.end());
        pending_->info.keyframe = pending_->info.keyframe || unit.info.keyframe;
        pending_->info.last_slice = unit.info.last_slice;
        pending_->info.encode_time_us = unit.info.encode_time_us;
        pending_->info.frame_id = unit.info.frame_id;
        pending_->recovered_by_fec = pending_->recovered_by_fec || unit.recovered_by_fec;
    }
    expected_unit_ = pending_->info.frame_id + 1;
    expected_slice_ = static_cast<std::uint8_t>(pending_->info.slice_index + 1);
    if (unit.info.slice_index != 0) expected_slice_ = static_cast<std::uint8_t>(unit.info.slice_index + 1);
    if (!pending_->info.last_slice) return std::nullopt;
    AssembledFrame out = std::move(*pending_);
    pending_.reset();
    out.info.slice = false;
    return out;
}

}  // namespace lancast::media
