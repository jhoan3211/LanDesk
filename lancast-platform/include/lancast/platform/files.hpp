#pragma once

// Transferencia de archivos bidireccional sobre un canal TCP cifrado dedicado.
// Bloques de 4 MB, reanudable (archivos .lancast-part) y verificada con SHA-256 (CNG con SHA-NI).
// Nunca sobrescribe: si el destino existe se guarda como "nombre (1).ext" salvo que se configure lo contrario.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lancast/platform/secure_channel.hpp"

namespace lancast::files {

inline constexpr std::size_t kChunkSize = 4u * 1024 * 1024;

enum class State { Queued, Waiting, Active, Verifying, Done, Failed, Cancelled };
[[nodiscard]] std::string_view to_string(State s) noexcept;

struct Item {
    std::uint32_t uid = 0;       // Identificador local (para la interfaz).
    std::uint32_t wire_id = 0;   // Identificador en el protocolo (del emisor).
    bool outgoing = false;
    std::string name;            // Ruta relativa con '/'.
    std::filesystem::path local_path;
    std::uint64_t size = 0;
    std::uint64_t done = 0;      // Bytes transferidos (incluye lo reanudado).
    std::uint64_t resumed_from = 0;
    State state = State::Queued;
    std::string message;
    double speed_bps = 0;
    std::int64_t mtime = 0;
};

class Manager {
public:
    Manager();
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    void set_receive_dir(std::filesystem::path dir);
    void set_overwrite(bool overwrite) { overwrite_ = overwrite; }
    [[nodiscard]] std::filesystem::path receive_dir() const;

    void attach(std::unique_ptr<net::SecureChannel> channel);
    void detach();
    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

    // Encola archivos y carpetas (recursivo) para enviar al otro equipo.
    void send(const std::vector<std::filesystem::path>& paths);
    [[nodiscard]] std::vector<Item> items() const;
    void cancel(std::uint32_t uid);
    void clear_finished();

    // Se llama (desde el hilo lector) cuando termina de recibirse un archivo.
    std::function<void(const Item&)> on_received;

private:
    struct Incoming {
        std::uint32_t uid = 0;
        std::filesystem::path final_path;
        std::filesystem::path part_path;
        std::ofstream file;
        std::uint64_t expected_offset = 0;
    };
    struct Response {
        enum class Kind { None, Accept, Reject, Result, Cancel } kind = Kind::None;
        std::uint64_t offset = 0;
        bool ok = false;
        std::string message;
    };

    void reader_loop();
    void sender_loop();
    void send_one(std::uint32_t uid);
    void handle_offer(std::span<const std::uint8_t> body);
    void handle_chunk(std::span<const std::uint8_t> body);
    void handle_done(std::span<const std::uint8_t> body);
    void fail_all_active(const std::string& reason);
    Item* find(std::uint32_t uid);
    Item* find_wire(std::uint32_t wire_id, bool outgoing);
    void update_speed(Item& item, std::uint64_t bytes);
    bool send_msg(std::span<const std::uint8_t> msg);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Item> items_;
    std::map<std::uint32_t, Incoming> incoming_;            // por wire_id del emisor remoto
    std::map<std::uint32_t, Response> responses_;           // por wire_id propio
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> speed_marks_;  // uid -> (t_us, bytes)
    std::unique_ptr<net::SecureChannel> channel_;
    std::thread reader_;
    std::thread sender_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> overwrite_{false};
    std::filesystem::path receive_dir_;
    std::uint32_t next_uid_ = 1;
    std::uint32_t next_wire_ = 1;
    std::uint32_t current_send_ = 0;
};

// Ruta segura dentro de `root` a partir de una ruta relativa remota (sin "..", unidades ni caracteres inválidos).
[[nodiscard]] std::optional<std::filesystem::path> safe_join(const std::filesystem::path& root, std::string_view relative);
// "a.txt" -> "a (1).txt" si existe, etc.
[[nodiscard]] std::filesystem::path unique_path(const std::filesystem::path& p);

}  // namespace lancast::files
