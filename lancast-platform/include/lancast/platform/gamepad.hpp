#pragma once

// Mandos virtuales de Xbox 360 en el host (driver ViGEmBus 1.22.0, la última versión: el proyecto se archivó en
// 2023 y no tendrá más actualizaciones; Windows no ofrece otra forma de crear un mando XInput sin un driver; se
// probó InputInjector y ni XInput ni Windows.Gaming.Input ven ese mando). Sin el driver no se crea nada y el host lo anuncia
// en HostInfo (gamepad_available = false) para que el cliente lo explique.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

#include "lancast/core/protocol.hpp"

namespace lancast::gamepad {

// ¿Está instalado el driver ViGEmBus? (se comprueba una vez por proceso).
[[nodiscard]] bool driver_available();

class VirtualPads {
public:
    static constexpr std::size_t kMaxPads = 4;

    VirtualPads();
    ~VirtualPads();
    VirtualPads(const VirtualPads&) = delete;
    VirtualPads& operator=(const VirtualPads&) = delete;

    // Crea el mando `state.index` la primera vez; `connected = false` lo quita.
    void update(const proto::GamepadState& state);
    void remove_all();
    // Vibración que pide el juego (desde un hilo del driver): índice, motor grande, motor pequeño.
    using RumbleCallback = std::function<void(std::uint8_t, std::uint8_t, std::uint8_t)>;
    void set_rumble_callback(RumbleCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lancast::gamepad
