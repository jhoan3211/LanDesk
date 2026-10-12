#include <gtest/gtest.h>

#include "lancast/core/encoding.hpp"
#include "lancast/core/protocol.hpp"

using namespace lancast;
using namespace lancast::proto;

namespace {

template <typename T>
T roundtrip(const T& value) {
    ByteWriter w;
    value.write(w);
    ByteReader r(w.data());
    auto out = T::read(r);
    EXPECT_TRUE(out.has_value());
    EXPECT_EQ(r.remaining(), 0u);
    return out.value_or(T{});
}

}  // namespace

TEST(Bytes, RoundtripPrimitives) {
    ByteWriter w;
    w.u8(0xAB).u16(0xBEEF).u32(0xDEADBEEF).u64(0x0123456789ABCDEFull).i32(-5).f32(1.5f).str("hola ñ").boolean(true);
    ByteReader r(w.data());
    EXPECT_EQ(r.u8(), 0xAB);
    EXPECT_EQ(r.u16(), 0xBEEF);
    EXPECT_EQ(r.u32(), 0xDEADBEEFu);
    EXPECT_EQ(r.u64(), 0x0123456789ABCDEFull);
    EXPECT_EQ(r.i32(), -5);
    EXPECT_EQ(r.f32(), 1.5f);
    EXPECT_EQ(r.str(), "hola ñ");
    EXPECT_TRUE(r.boolean());
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.u8(), 0);  // Fuera de rango.
    EXPECT_FALSE(r.ok());
}

TEST(Bytes, RejectsOversizedStrings) {
    ByteWriter w;
    w.u32(1'000'000);
    ByteReader r(w.data());
    (void)r.str(100);
    EXPECT_FALSE(r.ok());
}

TEST(Protocol, HelloRoundtrip) {
    Hello h;
    h.device_id[3] = 7;
    h.device_name = "PC-Escritorio";
    h.public_key[63] = 0x42;
    h.nonce[0] = 9;
    const Hello out = roundtrip(h);
    EXPECT_EQ(out.device_id, h.device_id);
    EXPECT_EQ(out.device_name, h.device_name);
    EXPECT_EQ(out.public_key, h.public_key);
    EXPECT_EQ(out.nonce, h.nonce);
}

TEST(Protocol, HelloRejectsBadMagic) {
    ByteWriter w;
    Hello h;
    h.magic = 0x12345678;
    h.write(w);
    ByteReader r(w.data());
    EXPECT_FALSE(Hello::read(r).has_value());
}

TEST(Protocol, HostInfoRoundtrip) {
    HostInfo h;
    h.host_name = "Torre";
    h.monitors.push_back({1, "LG", {-1920, 0, 1920, 1080}, 144, false, false});
    h.monitors.push_back({2, "Samsung", {0, 0, 3840, 2160}, 60, true, true});
    h.encoders = {Codec::H264, Codec::Hevc, Codec::Av1};
    h.media_port = 47998;
    h.file_port = 47985;
    const HostInfo out = roundtrip(h);
    ASSERT_EQ(out.monitors.size(), 2u);
    EXPECT_EQ(out.monitors[0].rect, (Rect{-1920, 0, 1920, 1080}));
    EXPECT_TRUE(out.monitors[1].hdr);
    EXPECT_EQ(out.encoders, h.encoders);
    EXPECT_EQ(out.file_port, 47985);
}

TEST(Protocol, StreamConfigAndStartedRoundtrip) {
    StreamConfig c;
    c.monitor_id = 0;
    c.max_width = 2560;
    c.max_height = 1440;
    c.fps = 144;
    c.codec = CodecChoice::Hevc;
    c.bitrate_kbps = 200000;
    c.rate_control = RateControl::Vbr;
    c.fec_percent = 20;
    const StreamConfig oc = roundtrip(c);
    EXPECT_EQ(oc.fps, 144u);
    EXPECT_EQ(oc.codec, CodecChoice::Hevc);
    EXPECT_EQ(oc.rate_control, RateControl::Vbr);
    EXPECT_EQ(oc.fec_percent, 20);

    StreamStarted s;
    s.epoch = 3;
    s.codec = Codec::Hevc;
    s.width = 2560;
    s.height = 1440;
    s.source = {-2560, 0, 2560, 1440};
    s.notes = "4:4:4 no soportado";
    const StreamStarted os = roundtrip(s);
    EXPECT_EQ(os.epoch, 3);
    EXPECT_EQ(os.source, s.source);
    EXPECT_EQ(os.notes, s.notes);
}

TEST(Protocol, CursorShapeValidatesSize) {
    CursorShape c;
    c.width = 32;
    c.height = 32;
    c.pitch = 128;
    c.data.resize(128 * 32);
    EXPECT_EQ(roundtrip(c).data.size(), c.data.size());

    c.data.resize(10);  // Datos insuficientes para pitch * height.
    ByteWriter w;
    c.write(w);
    ByteReader r(w.data());
    EXPECT_FALSE(CursorShape::read(r).has_value());
}

TEST(Protocol, EncodeAddsType) {
    const auto bytes = encode(MsgType::Key, Key{0x1E, false, true});
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], static_cast<std::uint8_t>(MsgType::Key));
}

TEST(Encoding, HexAndBase64) {
    const std::vector<std::uint8_t> data{0x00, 0x01, 0xAB, 0xFF, 0x10};
    EXPECT_EQ(to_hex(data), "0001abff10");
    EXPECT_EQ(from_hex("0001ABff10").value(), data);
    EXPECT_FALSE(from_hex("abc").has_value());
    EXPECT_FALSE(from_hex("zz").has_value());

    const std::string_view text = "Many hands make light work.";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    EXPECT_EQ(base64_encode(bytes), "TWFueSBoYW5kcyBtYWtlIGxpZ2h0IHdvcmsu");
    EXPECT_EQ(base64_decode("TWFueSBoYW5kcyBtYWtlIGxpZ2h0IHdvcmsu").value(), bytes);
    for (std::size_t n = 0; n < 5; ++n) {
        const std::vector<std::uint8_t> part(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
        EXPECT_EQ(base64_decode(base64_encode(part)).value(), part);
    }
}

TEST(Protocol, StreamConfigFromOlderClientHasNewFeaturesOff) {
    // Un cliente 1.2 no envía intra_refresh ni slices: el host debe tratarlos como desactivados.
    StreamConfig c;
    ByteWriter w;
    c.write(w);
    std::vector<std::uint8_t> old_bytes = w.take();
    old_bytes.resize(old_bytes.size() - 4 - 1 - 4);  // Sin los de la 1.3 (intra_refresh, slices, stream_index, enabled), 1.4 y 1.7.
    ByteReader r(old_bytes);
    const auto out = StreamConfig::read(r);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->intra_refresh);
    EXPECT_FALSE(out->slices);
    EXPECT_TRUE(out->low_latency);
    EXPECT_EQ(out->stream_index, 0);
    EXPECT_TRUE(out->enabled);
}

TEST(Protocol, StreamStartedOptionalFieldsRoundtrip) {
    StreamStarted s;
    s.intra_refresh = true;
    s.slices = true;
    s.encoder_backend = "AMF directo";
    const StreamStarted out = roundtrip(s);
    EXPECT_TRUE(out.intra_refresh);
    EXPECT_TRUE(out.slices);
    EXPECT_EQ(out.encoder_backend, "AMF directo");
}

TEST(Protocol, HostInfoSecureDesktopIsOptional) {
    HostInfo h;
    h.host_name = "Servicio";
    h.secure_desktop = true;
    EXPECT_TRUE(roundtrip(h).secure_desktop);

    // Un host 1.2 no envía el campo: el cliente no debe ofrecer Ctrl+Alt+Supr.
    ByteWriter w;
    h.write(w);
    std::vector<std::uint8_t> old_bytes = w.take();
    old_bytes.resize(old_bytes.size() - 1 - 7);  // Sin secure_desktop (1.3) ni MAC/mando (1.7).
    ByteReader r(old_bytes);
    const auto out = HostInfo::read(r);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->secure_desktop);
    EXPECT_EQ(out->host_name, "Servicio");
}

TEST(Protocol, SecureAttentionHasNoBody) {
    const auto bytes = encode(MsgType::SecureAttention);
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 34);
}

TEST(Protocol, ColorFeaturesAreOptional) {
    // Cliente 1.4: anuncia 4:4:4 y HDR; un cliente 1.3 no los envía.
    ClientCaps caps;
    caps.client_name = "Cliente";
    caps.supports_slices = true;
    caps.supports_444 = true;
    caps.supports_hdr = true;
    const ClientCaps back = roundtrip(caps);
    EXPECT_TRUE(back.supports_444);
    EXPECT_TRUE(back.supports_hdr);
    {
        ByteWriter w;
        caps.write(w);
        std::vector<std::uint8_t> old = w.take();
        old.resize(old.size() - 2);
        ByteReader r(old);
        const auto out = ClientCaps::read(r);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out->supports_slices);
        EXPECT_FALSE(out->supports_444);
        EXPECT_FALSE(out->supports_hdr);
    }

    // Host 1.4: StreamStarted con 4:4:4/HDR; uno 1.3 no los envía.
    StreamStarted st;
    st.width = 1920;
    st.height = 1080;
    st.chroma_444 = true;
    st.hdr = true;
    const StreamStarted st_back = roundtrip(st);
    EXPECT_TRUE(st_back.chroma_444);
    EXPECT_TRUE(st_back.hdr);
    {
        ByteWriter w;
        st.write(w);
        std::vector<std::uint8_t> old = w.take();
        old.resize(old.size() - 2);
        ByteReader r(old);
        const auto out = StreamStarted::read(r);
        ASSERT_TRUE(out.has_value());
        EXPECT_FALSE(out->chroma_444);
        EXPECT_FALSE(out->hdr);
    }

    // StreamConfig 1.4: "adaptive" opcional (ausente = activado).
    StreamConfig cfg;
    cfg.adaptive = false;
    EXPECT_FALSE(roundtrip(cfg).adaptive);
    {
        ByteWriter w;
        cfg.write(w);
        std::vector<std::uint8_t> old = w.take();
        old.resize(old.size() - 1 - 4);  // Sin "adaptive" (1.4) ni "pacing_mbps" (1.7).
        ByteReader r(old);
        const auto out = StreamConfig::read(r);
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(out->adaptive);
    }

    // StreamConfig 1.7: "pacing_mbps" opcional (ausente = sin límite).
    cfg.pacing_mbps = 500;
    EXPECT_EQ(roundtrip(cfg).pacing_mbps, 500u);
    {
        ByteWriter w;
        cfg.write(w);
        std::vector<std::uint8_t> old = w.take();
        old.resize(old.size() - 4);
        ByteReader r(old);
        const auto out = StreamConfig::read(r);
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->pacing_mbps, 0u);
        EXPECT_FALSE(out->adaptive);
    }
}

TEST(Protocol, GamepadAndWakeOnLanFields) {
    GamepadState g;
    g.index = 2;
    g.buttons = 0x1010;  // A + Start
    g.left_trigger = 200;
    g.left_x = -32768;
    g.right_y = 32767;
    ByteWriter w;
    g.write(w);
    ByteReader r(w.data());
    const auto back = GamepadState::read(r);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->index, 2);
    EXPECT_EQ(back->buttons, 0x1010);
    EXPECT_EQ(back->left_trigger, 200);
    EXPECT_EQ(back->left_x, -32768);
    EXPECT_EQ(back->right_y, 32767);

    // HostInfo 1.7: MAC y mando opcionales (un host 1.6 no los envía).
    HostInfo h;
    h.host_name = "PC";
    h.mac = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    h.gamepad_available = true;
    const HostInfo hb = roundtrip(h);
    EXPECT_EQ(hb.mac, h.mac);
    EXPECT_TRUE(hb.gamepad_available);
    ByteWriter hw;
    h.write(hw);
    std::vector<std::uint8_t> old = hw.take();
    old.resize(old.size() - 7);
    ByteReader hr(old);
    const auto ho = HostInfo::read(hr);
    ASSERT_TRUE(ho.has_value());
    EXPECT_FALSE(ho->gamepad_available);
    EXPECT_EQ(ho->mac, (std::array<std::uint8_t, 6>{}));
}
