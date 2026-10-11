#include "lancast/platform/files.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "lancast/core/bytes.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/protocol.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/d3d.hpp"
#include "win.hpp"

namespace lancast::files {
namespace {

namespace fs = std::filesystem;

std::string path_utf8(const fs::path& p) { return platform::detail::to_utf8(p.wstring()); }
fs::path path_from_utf8(std::string_view s) { return fs::path{platform::detail::to_wide(s)}; }

std::int64_t file_mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

fs::path part_of(const fs::path& final_path) { return fs::path{final_path}.concat(L".lancast-part"); }
fs::path meta_of(const fs::path& final_path) { return fs::path{final_path}.concat(L".lancast-part.meta"); }

std::optional<std::pair<std::uint64_t, std::int64_t>> read_meta(const fs::path& p) {
    std::ifstream f(p);
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (f >> size >> mtime) return std::pair{size, mtime};
    return std::nullopt;
}

crypto::Digest hash_file(const fs::path& p, std::uint64_t limit) {
    crypto::Sha256 h;
    std::ifstream f(p, std::ios::binary);
    std::vector<std::uint8_t> buf(kChunkSize);
    std::uint64_t remaining = limit;
    while (remaining > 0 && f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(buf.size(), remaining)));
        const auto n = static_cast<std::size_t>(f.gcount());
        if (n == 0) break;
        h.update({buf.data(), n});
        remaining -= n;
    }
    return h.finish();
}

}  // namespace

std::string_view to_string(State s) noexcept {
    switch (s) {
        case State::Queued:
            return "En cola";
        case State::Waiting:
            return "Esperando";
        case State::Active:
            return "Transfiriendo";
        case State::Verifying:
            return "Verificando";
        case State::Done:
            return "Completado";
        case State::Failed:
            return "Error";
        case State::Cancelled:
            return "Cancelado";
    }
    return "?";
}

std::optional<fs::path> safe_join(const fs::path& root, std::string_view relative) {
    fs::path out = root;
    std::size_t start = 0;
    int parts = 0;
    while (start <= relative.size()) {
        const std::size_t end = relative.find_first_of("/\\", start);
        std::string part{relative.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)};
        start = end == std::string_view::npos ? relative.size() + 1 : end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find(':') != std::string::npos) return std::nullopt;
        for (char& c : part) {
            if (static_cast<unsigned char>(c) < 32 || std::string_view{"<>\"|?*"}.find(c) != std::string_view::npos) c = '_';
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '.')) part.pop_back();
        if (part.empty()) return std::nullopt;
        out /= path_from_utf8(part);
        ++parts;
    }
    if (parts == 0) return std::nullopt;
    return out;
}

fs::path unique_path(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) && !fs::exists(part_of(p), ec)) return p;
    const fs::path stem = p.stem();
    const fs::path ext = p.extension();
    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = p.parent_path() / fs::path{stem}.concat(std::format(L" ({})", i)).concat(ext.wstring());
        if (!fs::exists(candidate, ec) && !fs::exists(part_of(candidate), ec)) return candidate;
    }
    return p;
}

Manager::Manager() = default;

Manager::~Manager() { detach(); }

void Manager::set_receive_dir(fs::path dir) {
    std::scoped_lock lock(mutex_);
    receive_dir_ = std::move(dir);
}

fs::path Manager::receive_dir() const {
    std::scoped_lock lock(mutex_);
    return receive_dir_;
}

void Manager::attach(std::unique_ptr<net::SecureChannel> channel) {
    detach();
    stopping_ = false;
    channel_ = std::move(channel);
    channel_->stream().set_buffers(8 * 1024 * 1024);
    connected_ = true;
    reader_ = std::thread([this] { reader_loop(); });
    sender_ = std::thread([this] { sender_loop(); });
}

void Manager::detach() {
    stopping_ = true;
    if (channel_) channel_->shutdown();
    cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (sender_.joinable()) sender_.join();
    channel_.reset();
    connected_ = false;
    fail_all_active("Conexión cerrada");
}

bool Manager::send_msg(std::span<const std::uint8_t> msg) { return channel_ && channel_->send(msg); }

void Manager::send(const std::vector<fs::path>& paths) {
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    auto enqueue = [&](const fs::path& file, const std::string& rel) {
        Item item;
        item.uid = next_uid_++;
        item.wire_id = next_wire_++;
        item.outgoing = true;
        item.name = rel;
        item.local_path = file;
        item.size = fs::file_size(file, ec);
        item.mtime = file_mtime(file);
        items_.push_back(std::move(item));
    };
    for (const fs::path& p : paths) {
        if (fs::is_directory(p, ec)) {
            const fs::path base = p.parent_path();
            for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string rel = path_utf8(fs::relative(it->path(), base, ec));
                std::ranges::replace(rel, '\\', '/');
                enqueue(it->path(), rel);
            }
        } else if (fs::is_regular_file(p, ec)) {
            enqueue(p, path_utf8(p.filename()));
        }
    }
    cv_.notify_all();
}

std::vector<Item> Manager::items() const {
    std::scoped_lock lock(mutex_);
    return items_;
}

void Manager::cancel(std::uint32_t uid) {
    std::uint32_t wire = 0;
    bool outgoing = false;
    {
        std::scoped_lock lock(mutex_);
        Item* it = find(uid);
        if (it == nullptr || it->state == State::Done || it->state == State::Failed || it->state == State::Cancelled) return;
        wire = it->wire_id;
        outgoing = it->outgoing;
        if (it->state == State::Queued) {
            it->state = State::Cancelled;
            return;
        }
        it->state = State::Cancelled;
        if (outgoing) responses_[wire].kind = Response::Kind::Cancel;
    }
    cv_.notify_all();
    if (!outgoing) {
        // Avisar al emisor: dejará de mandar bloques (el .lancast-part queda para reanudar).
        send_msg(proto::encode(proto::MsgType::FileCancel, proto::FileStatus{wire, false, "Cancelado por el receptor"}));
    }
}

void Manager::clear_finished() {
    std::scoped_lock lock(mutex_);
    std::erase_if(items_, [](const Item& i) {
        return i.state == State::Done || i.state == State::Failed || i.state == State::Cancelled;
    });
}

Item* Manager::find(std::uint32_t uid) {
    const auto it = std::ranges::find(items_, uid, &Item::uid);
    return it == items_.end() ? nullptr : &*it;
}

Item* Manager::find_wire(std::uint32_t wire_id, bool outgoing) {
    const auto it = std::ranges::find_if(items_, [&](const Item& i) { return i.wire_id == wire_id && i.outgoing == outgoing; });
    return it == items_.end() ? nullptr : &*it;
}

void Manager::update_speed(Item& item, std::uint64_t bytes) {
    const std::uint64_t now = gpu::now_us();
    auto& [t, b] = speed_marks_[item.uid];
    if (t == 0) {
        t = now;
        b = bytes;
        return;
    }
    if (now - t >= 500'000) {
        const double instant = static_cast<double>(bytes - b) * 1e6 / static_cast<double>(now - t);
        item.speed_bps = item.speed_bps == 0 ? instant : item.speed_bps * 0.5 + instant * 0.5;
        t = now;
        b = bytes;
    }
}

void Manager::fail_all_active(const std::string& reason) {
    std::scoped_lock lock(mutex_);
    for (Item& i : items_) {
        if (i.state == State::Waiting || i.state == State::Active || i.state == State::Verifying) {
            i.state = State::Failed;
            i.message = reason + " (se puede reanudar al reenviar)";
        }
    }
    for (auto& [id, in] : incoming_) in.file.close();
    incoming_.clear();
}

void Manager::sender_loop() {
    while (!stopping_) {
        std::uint32_t uid = 0;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return stopping_ || std::ranges::any_of(items_, [](const Item& i) { return i.outgoing && i.state == State::Queued; });
            });
            if (stopping_) return;
            for (Item& i : items_) {
                if (i.outgoing && i.state == State::Queued) {
                    uid = i.uid;
                    i.state = State::Waiting;
                    break;
                }
            }
        }
        if (uid != 0) send_one(uid);
    }
}

void Manager::send_one(std::uint32_t uid) {
    proto::FileOffer offer;
    fs::path path;
    {
        std::scoped_lock lock(mutex_);
        Item* it = find(uid);
        if (it == nullptr) return;
        offer = proto::FileOffer{it->wire_id, it->name, it->size, it->mtime};
        path = it->local_path;
        responses_.erase(it->wire_id);
    }
    if (!send_msg(proto::encode(proto::MsgType::FileOffer, offer))) return;

    auto wait_response = [&](std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return stopping_ || responses_[offer.transfer_id].kind != Response::Kind::None; });
        Response r = responses_[offer.transfer_id];
        responses_[offer.transfer_id] = Response{};
        return r;
    };
    auto set_state = [&](State s, std::string msg = {}) {
        std::scoped_lock lock(mutex_);
        if (Item* it = find(uid)) {
            if (it->state != State::Cancelled) it->state = s;
            if (!msg.empty()) it->message = std::move(msg);
        }
    };

    const Response accept = wait_response(std::chrono::seconds(120));
    if (accept.kind == Response::Kind::Cancel) return;
    if (accept.kind != Response::Kind::Accept) {
        set_state(State::Failed, accept.kind == Response::Kind::Reject ? accept.message : "Sin respuesta del otro equipo");
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        send_msg(proto::encode(proto::MsgType::FileCancel, proto::FileStatus{offer.transfer_id, false, "No se pudo leer"}));
        set_state(State::Failed, "No se pudo abrir el archivo");
        return;
    }
    crypto::Sha256 hash;
    std::uint64_t offset = std::min(accept.offset, offer.size);
    if (offset > 0) {
        // Reanudación: el hash cubre el archivo completo, así que se recorre la parte ya enviada.
        std::vector<std::uint8_t> tmp(kChunkSize);
        std::uint64_t left = offset;
        while (left > 0 && file) {
            file.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(std::min<std::uint64_t>(tmp.size(), left)));
            const auto n = static_cast<std::size_t>(file.gcount());
            if (n == 0) break;
            hash.update({tmp.data(), n});
            left -= n;
        }
    }
    {
        std::scoped_lock lock(mutex_);
        if (Item* it = find(uid)) {
            it->state = State::Active;
            it->resumed_from = offset;
            it->done = offset;
        }
    }

    // [tipo][id u32][offset u64][datos]
    std::vector<std::uint8_t> msg(1 + 4 + 8 + kChunkSize);
    while (offset < offer.size) {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || responses_[offer.transfer_id].kind == Response::Kind::Cancel) {
                if (Item* it = find(uid)) it->state = State::Cancelled;
                send_msg(proto::encode(proto::MsgType::FileCancel, proto::FileStatus{offer.transfer_id, false, "Cancelado"}));
                return;
            }
        }
        const std::size_t n_req = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkSize, offer.size - offset));
        file.read(reinterpret_cast<char*>(msg.data() + 13), static_cast<std::streamsize>(n_req));
        const auto n = static_cast<std::size_t>(file.gcount());
        if (n == 0) {
            set_state(State::Failed, "El archivo cambió durante el envío");
            send_msg(proto::encode(proto::MsgType::FileCancel, proto::FileStatus{offer.transfer_id, false, "Error de lectura"}));
            return;
        }
        ByteWriter header(13);
        header.u8(static_cast<std::uint8_t>(proto::MsgType::FileChunk)).u32(offer.transfer_id).u64(offset);
        std::copy(header.data().begin(), header.data().end(), msg.begin());
        hash.update({msg.data() + 13, n});
        if (!send_msg({msg.data(), 13 + n})) return;
        offset += n;
        std::scoped_lock lock(mutex_);
        if (Item* it = find(uid)) {
            it->done = offset;
            update_speed(*it, offset);
        }
    }

    set_state(State::Verifying);
    send_msg(proto::encode(proto::MsgType::FileDone, proto::FileDone{offer.transfer_id, hash.finish()}));
    const Response result = wait_response(std::chrono::seconds(600));
    if (result.kind == Response::Kind::Result && result.ok) {
        set_state(State::Done, "Verificado (SHA-256)");
    } else if (result.kind != Response::Kind::Cancel) {
        set_state(State::Failed, result.message.empty() ? "Sin confirmación" : result.message);
    }
}

void Manager::reader_loop() {
    std::vector<std::uint8_t> msg;
    while (!stopping_ && channel_->recv(msg, kChunkSize + 1024)) {
        if (msg.empty()) continue;
        const auto type = static_cast<proto::MsgType>(msg[0]);
        const std::span<const std::uint8_t> body{msg.data() + 1, msg.size() - 1};
        ByteReader r(body);
        switch (type) {
            case proto::MsgType::FileOffer:
                handle_offer(body);
                break;
            case proto::MsgType::FileChunk:
                handle_chunk(body);
                break;
            case proto::MsgType::FileDone:
                handle_done(body);
                break;
            case proto::MsgType::FileAccept:
                if (auto a = proto::FileAccept::read(r)) {
                    std::scoped_lock lock(mutex_);
                    responses_[a->transfer_id] = Response{Response::Kind::Accept, a->offset, true, {}};
                }
                cv_.notify_all();
                break;
            case proto::MsgType::FileReject:
            case proto::MsgType::FileResult:
                if (auto s = proto::FileStatus::read(r)) {
                    std::scoped_lock lock(mutex_);
                    responses_[s->transfer_id] = Response{
                        type == proto::MsgType::FileReject ? Response::Kind::Reject : Response::Kind::Result, 0, s->ok,
                        s->message};
                }
                cv_.notify_all();
                break;
            case proto::MsgType::FileCancel:
                if (auto s = proto::FileStatus::read(r)) {
                    std::scoped_lock lock(mutex_);
                    // Puede referirse a un envío nuestro (el receptor canceló) o a una recepción (el emisor canceló).
                    if (Item* out = find_wire(s->transfer_id, true); out != nullptr && out->state != State::Done) {
                        responses_[s->transfer_id].kind = Response::Kind::Cancel;
                        out->state = State::Cancelled;
                        out->message = s->message;
                    }
                    if (auto in = incoming_.find(s->transfer_id); in != incoming_.end()) {
                        in->second.file.close();
                        if (Item* it = find(in->second.uid)) {
                            it->state = State::Cancelled;
                            it->message = s->message;
                        }
                        incoming_.erase(in);
                    }
                }
                cv_.notify_all();
                break;
            default:
                break;
        }
    }
    connected_ = false;
    cv_.notify_all();
    if (!stopping_) fail_all_active("Conexión perdida");
}

void Manager::handle_offer(std::span<const std::uint8_t> body) {
    ByteReader r(body);
    const auto offer = proto::FileOffer::read(r);
    if (!offer) return;

    auto reject = [&](const std::string& why) {
        send_msg(proto::encode(proto::MsgType::FileReject, proto::FileStatus{offer->transfer_id, false, why}));
    };

    std::error_code ec;
    const auto target = safe_join(receive_dir(), offer->relative_path);
    if (!target) {
        reject("Ruta de archivo no válida");
        return;
    }
    fs::create_directories(target->parent_path(), ec);

    // ¿Reanudación? Mismo destino con .lancast-part y metadatos iguales.
    std::uint64_t offset = 0;
    fs::path final_path = *target;
    if (const auto meta = read_meta(meta_of(*target));
        meta && meta->first == offer->size && meta->second == offer->mtime && fs::exists(part_of(*target), ec)) {
        offset = std::min<std::uint64_t>(fs::file_size(part_of(*target), ec), offer->size);
    } else if (!overwrite_) {
        final_path = unique_path(*target);
    }
    const fs::path part = part_of(final_path);

    Incoming in;
    in.final_path = final_path;
    in.part_path = part;
    in.file.open(part, offset > 0 ? (std::ios::binary | std::ios::in | std::ios::out) : (std::ios::binary | std::ios::out | std::ios::trunc));
    if (!in.file) {
        reject("No se pudo crear el archivo en el destino");
        return;
    }
    in.file.seekp(static_cast<std::streamoff>(offset));
    in.expected_offset = offset;
    {
        std::ofstream meta(meta_of(final_path), std::ios::trunc);
        meta << offer->size << ' ' << offer->mtime;
    }

    {
        std::scoped_lock lock(mutex_);
        Item item;
        item.uid = next_uid_++;
        item.wire_id = offer->transfer_id;
        item.outgoing = false;
        item.name = offer->relative_path;
        item.local_path = final_path;
        item.size = offer->size;
        item.done = offset;
        item.resumed_from = offset;
        item.mtime = offer->mtime;
        item.state = State::Active;
        if (offset > 0) item.message = std::format("Reanudado desde {:.0f} MB", offset / 1048576.0);
        in.uid = item.uid;
        items_.push_back(std::move(item));
        incoming_[offer->transfer_id] = std::move(in);
    }
    log::info("Recibiendo {} ({} bytes{})", offer->relative_path, offer->size, offset > 0 ? ", reanudado" : "");
    send_msg(proto::encode(proto::MsgType::FileAccept, proto::FileAccept{offer->transfer_id, offset}));
}

void Manager::handle_chunk(std::span<const std::uint8_t> body) {
    ByteReader r(body);
    const std::uint32_t id = r.u32();
    const std::uint64_t offset = r.u64();
    if (!r.ok()) return;
    const auto data = r.rest();

    std::scoped_lock lock(mutex_);
    auto it = incoming_.find(id);
    if (it == incoming_.end()) return;
    Incoming& in = it->second;
    Item* item = find(in.uid);
    if (offset != in.expected_offset || item == nullptr) {
        in.file.close();
        if (item) {
            item->state = State::Failed;
            item->message = "Bloques fuera de orden";
        }
        incoming_.erase(it);
        return;
    }
    in.file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    in.expected_offset += data.size();
    item->done = in.expected_offset;
    update_speed(*item, item->done);
    if (!in.file) {
        item->state = State::Failed;
        item->message = "Error de escritura (¿disco lleno?)";
        in.file.close();
        incoming_.erase(it);
    }
}

void Manager::handle_done(std::span<const std::uint8_t> body) {
    ByteReader r(body);
    const auto done = proto::FileDone::read(r);
    if (!done) return;

    Incoming in;
    Item snapshot;
    {
        std::scoped_lock lock(mutex_);
        auto it = incoming_.find(done->transfer_id);
        if (it == incoming_.end()) return;
        in = std::move(it->second);
        incoming_.erase(it);
        if (Item* item = find(in.uid)) item->state = State::Verifying;
    }
    in.file.close();

    std::error_code ec;
    const std::uint64_t size = fs::file_size(in.part_path, ec);
    const crypto::Digest digest = hash_file(in.part_path, size);
    const bool ok = crypto::equal_ct(digest, done->sha256);
    std::string message;
    if (ok) {
        fs::path final_path = in.final_path;
        if (fs::exists(final_path, ec)) {
            if (overwrite_) {
                fs::remove(final_path, ec);
            } else {
                final_path = unique_path(final_path);
            }
        }
        fs::rename(in.part_path, final_path, ec);
        fs::remove(meta_of(in.final_path), ec);
        in.final_path = final_path;
        message = ec ? "Recibido, pero no se pudo renombrar: " + ec.message() : "Verificado (SHA-256)";
    } else {
        fs::remove(in.part_path, ec);
        fs::remove(meta_of(in.final_path), ec);
        message = "La verificación SHA-256 falló: archivo descartado";
    }
    send_msg(proto::encode(proto::MsgType::FileResult, proto::FileStatus{done->transfer_id, ok, message}));

    {
        std::scoped_lock lock(mutex_);
        if (Item* item = find(in.uid)) {
            item->state = ok ? State::Done : State::Failed;
            item->message = message;
            item->local_path = in.final_path;
            snapshot = *item;
        }
    }
    if (ok && on_received) on_received(snapshot);
}

}  // namespace lancast::files
