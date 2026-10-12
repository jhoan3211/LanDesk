#include "lancast/core/protocol.hpp"

namespace lancast::proto {
namespace {

template <typename T>
std::optional<T> finish(ByteReader& r, T&& value) {
    if (!r.ok()) return std::nullopt;
    return std::optional<T>{std::forward<T>(value)};
}

void write_codecs(ByteWriter& w, const std::vector<Codec>& codecs) {
    w.u8(static_cast<std::uint8_t>(codecs.size()));
    for (const Codec c : codecs) w.u8(static_cast<std::uint8_t>(c));
}

std::vector<Codec> read_codecs(ByteReader& r) {
    const std::uint8_t n = r.u8();
    std::vector<Codec> out;
    for (std::uint8_t i = 0; i < n && r.ok(); ++i) {
        const std::uint8_t c = r.u8();
        if (c <= static_cast<std::uint8_t>(Codec::Av1)) out.push_back(static_cast<Codec>(c));
    }
    return out;
}

void write_rect(ByteWriter& w, const Rect& rc) { w.i32(rc.x).i32(rc.y).i32(rc.width).i32(rc.height); }
Rect read_rect(ByteReader& r) {
    Rect rc;
    rc.x = r.i32();
    rc.y = r.i32();
    rc.width = r.i32();
    rc.height = r.i32();
    return rc;
}

}  // namespace

void Hello::write(ByteWriter& w) const {
    w.u32(magic).u16(version).raw(device_id).str(device_name).raw(public_key).raw(nonce);
}

std::optional<Hello> Hello::read(ByteReader& r) {
    Hello h;
    h.magic = r.u32();
    h.version = r.u16();
    h.device_id = r.fixed<16>();
    h.device_name = r.str(256);
    h.public_key = r.fixed<64>();
    h.nonce = r.fixed<32>();
    if (h.magic != kMagic) return std::nullopt;
    return finish(r, std::move(h));
}

void HelloAck::write(ByteWriter& w) const {
    w.u16(version).raw(device_id).str(device_name).raw(public_key).raw(nonce).boolean(has_pairing).boolean(accepted).str(
        message);
}

std::optional<HelloAck> HelloAck::read(ByteReader& r) {
    HelloAck h;
    h.version = r.u16();
    h.device_id = r.fixed<16>();
    h.device_name = r.str(256);
    h.public_key = r.fixed<64>();
    h.nonce = r.fixed<32>();
    h.has_pairing = r.boolean();
    h.accepted = r.boolean();
    h.message = r.str(4096);
    return finish(r, std::move(h));
}

void AuthStart::write(ByteWriter& w) const { w.boolean(use_pairing); }
std::optional<AuthStart> AuthStart::read(ByteReader& r) { return finish(r, AuthStart{r.boolean()}); }

void AuthStartAck::write(ByteWriter& w) const { w.boolean(pin_required); }
std::optional<AuthStartAck> AuthStartAck::read(ByteReader& r) { return finish(r, AuthStartAck{r.boolean()}); }

void AuthProof::write(ByteWriter& w) const { w.raw(mac); }
std::optional<AuthProof> AuthProof::read(ByteReader& r) { return finish(r, AuthProof{r.fixed<32>()}); }

void AuthResult::write(ByteWriter& w) const { w.boolean(ok).str(message).raw(mac).raw(session_id); }
std::optional<AuthResult> AuthResult::read(ByteReader& r) {
    AuthResult a;
    a.ok = r.boolean();
    a.message = r.str(4096);
    a.mac = r.fixed<32>();
    a.session_id = r.fixed<16>();
    return finish(r, std::move(a));
}

void ClientCaps::write(ByteWriter& w) const {
    w.str(client_name);
    write_codecs(w, decoders);
    w.u32(display_width).u32(display_height).u32(display_refresh);
    w.boolean(supports_slices);
    w.boolean(supports_444).boolean(supports_hdr);
}

std::optional<ClientCaps> ClientCaps::read(ByteReader& r) {
    ClientCaps c;
    c.client_name = r.str(256);
    c.decoders = read_codecs(r);
    c.display_width = r.u32();
    c.display_height = r.u32();
    c.display_refresh = r.u32();
    if (r.ok() && r.remaining() >= 1) c.supports_slices = r.boolean();
    if (r.ok() && r.remaining() >= 2) {  // Ausentes en clientes 1.3.
        c.supports_444 = r.boolean();
        c.supports_hdr = r.boolean();
    }
    return finish(r, std::move(c));
}

void HostInfo::write(ByteWriter& w) const {
    w.str(host_name);
    w.u8(static_cast<std::uint8_t>(monitors.size()));
    for (const auto& m : monitors) {
        w.u32(m.id).str(m.name);
        write_rect(w, m.rect);
        w.u32(m.refresh_hz).boolean(m.primary).boolean(m.hdr);
    }
    write_codecs(w, encoders);
    w.boolean(audio_available).boolean(input_allowed).boolean(clipboard_allowed).boolean(files_allowed);
    w.u16(media_port).u16(file_port);
    w.boolean(secure_desktop);
}

std::optional<HostInfo> HostInfo::read(ByteReader& r) {
    HostInfo h;
    h.host_name = r.str(256);
    const std::uint8_t n = r.u8();
    for (std::uint8_t i = 0; i < n && r.ok(); ++i) {
        RemoteMonitor m;
        m.id = r.u32();
        m.name = r.str(256);
        m.rect = read_rect(r);
        m.refresh_hz = r.u32();
        m.primary = r.boolean();
        m.hdr = r.boolean();
        h.monitors.push_back(std::move(m));
    }
    h.encoders = read_codecs(r);
    h.audio_available = r.boolean();
    h.input_allowed = r.boolean();
    h.clipboard_allowed = r.boolean();
    h.files_allowed = r.boolean();
    h.media_port = r.u16();
    h.file_port = r.u16();
    if (r.remaining() >= 1) h.secure_desktop = r.boolean();
    return finish(r, std::move(h));
}

void StreamConfig::write(ByteWriter& w) const {
    w.u32(monitor_id).u32(max_width).u32(max_height).u32(fps).u8(static_cast<std::uint8_t>(codec)).boolean(auto_bitrate);
    w.u32(bitrate_kbps).u8(static_cast<std::uint8_t>(rate_control)).boolean(chroma_444).boolean(hdr).boolean(audio).u8(
        fec_percent);
    w.u32(audio_rate).u8(audio_channels);
    w.boolean(low_latency);
    w.boolean(intra_refresh).boolean(slices);
    w.u8(stream_index).boolean(enabled);
    w.boolean(adaptive);
    w.u32(pacing_mbps);
}

std::optional<StreamConfig> StreamConfig::read(ByteReader& r) {
    StreamConfig c;
    c.monitor_id = r.u32();
    c.max_width = r.u32();
    c.max_height = r.u32();
    c.fps = r.u32();
    c.codec = static_cast<CodecChoice>(r.u8());
    c.auto_bitrate = r.boolean();
    c.bitrate_kbps = r.u32();
    c.rate_control = static_cast<RateControl>(r.u8());
    c.chroma_444 = r.boolean();
    c.hdr = r.boolean();
    c.audio = r.boolean();
    c.fec_percent = r.u8();
    c.audio_rate = r.u32();
    c.audio_channels = r.u8();
    if (c.audio_channels < 1 || c.audio_channels > 2) c.audio_channels = 2;
    if (c.stream_index > 1) return std::nullopt;
    if (r.ok() && r.remaining() >= 1) c.low_latency = r.boolean();  // Ausente en clientes 1.1.0.
    if (r.ok() && r.remaining() >= 2) {                               // Ausentes en clientes 1.2.0.
        c.intra_refresh = r.boolean();
        c.slices = r.boolean();
        if (r.remaining() >= 2) {
            c.stream_index = r.u8();
            c.enabled = r.boolean();
        }
        if (r.remaining() >= 1) c.adaptive = r.boolean();
        if (r.remaining() >= 4) c.pacing_mbps = r.u32();
    } else {
        c.intra_refresh = false;
        c.slices = false;
    }
    if (static_cast<int>(c.codec) > 3 || static_cast<int>(c.rate_control) > 1) return std::nullopt;
    return finish(r, std::move(c));
}

void StreamStarted::write(ByteWriter& w) const {
    w.u16(epoch).u8(static_cast<std::uint8_t>(codec)).u32(width).u32(height).u32(fps).u32(bitrate_kbps).u32(monitor_id);
    write_rect(w, source);
    w.u32(audio_rate).u8(audio_channels).str(notes);
    w.boolean(intra_refresh).boolean(slices).str(encoder_backend).u8(stream_index);
    w.boolean(chroma_444).boolean(hdr);
}

std::optional<StreamStarted> StreamStarted::read(ByteReader& r) {
    StreamStarted s;
    s.epoch = r.u16();
    s.codec = static_cast<Codec>(r.u8());
    s.width = r.u32();
    s.height = r.u32();
    s.fps = r.u32();
    s.bitrate_kbps = r.u32();
    s.monitor_id = r.u32();
    s.source = read_rect(r);
    s.audio_rate = r.u32();
    s.audio_channels = r.u8();
    s.notes = r.str(4096);
    if (r.ok() && r.remaining() >= 2) {
        s.intra_refresh = r.boolean();
        s.slices = r.boolean();
        if (r.remaining() >= 4) s.encoder_backend = r.str(128);
        if (r.ok() && r.remaining() >= 1) s.stream_index = r.u8();
    }
    if (static_cast<int>(s.codec) > 2) return std::nullopt;
    if (r.ok() && r.remaining() >= 2) {  // Ausentes en hosts 1.3.
        s.chroma_444 = r.boolean();
        s.hdr = r.boolean();
    }
    return finish(r, std::move(s));
}

void CursorShape::write(ByteWriter& w) const {
    w.u8(static_cast<std::uint8_t>(type)).u32(width).u32(height).u32(pitch).i32(hot_x).i32(hot_y).blob(data);
}

std::optional<CursorShape> CursorShape::read(ByteReader& r) {
    CursorShape c;
    c.type = static_cast<CursorType>(r.u8());
    c.width = r.u32();
    c.height = r.u32();
    c.pitch = r.u32();
    c.hot_x = r.i32();
    c.hot_y = r.i32();
    c.data = r.blob(4u * 1024 * 1024);
    if (c.width > 512 || c.height > 1024 || c.data.size() < static_cast<std::size_t>(c.pitch) * c.height) {
        return std::nullopt;
    }
    return finish(r, std::move(c));
}

void DiscoveryReply::write(ByteWriter& w) const {
    w.u32(kDiscoveryMagic).u16(kVersion).str(host_name).raw(device_id).u16(control_port).u16(web_port).boolean(busy);
}

std::optional<DiscoveryReply> DiscoveryReply::read(ByteReader& r) {
    if (r.u32() != kDiscoveryMagic) return std::nullopt;
    (void)r.u16();
    DiscoveryReply d;
    d.host_name = r.str(256);
    d.device_id = r.fixed<16>();
    d.control_port = r.u16();
    d.web_port = r.u16();
    d.busy = r.boolean();
    return finish(r, std::move(d));
}

}  // namespace lancast::proto
