#pragma once

// Ventana-marcador para medir la latencia real: un cuadro siempre visible que alterna blanco/negro.
// Tiene su propio hilo con bucle de mensajes, así el host no depende de la interfaz.

#include <cstdint>
#include <memory>

#include "lancast/core/types.hpp"

namespace lancast::sys {

class MarkerWindow {
public:
    // rect: posición y tamaño en el escritorio virtual (píxeles físicos).
    explicit MarkerWindow(const Rect& rect);
    ~MarkerWindow();
    MarkerWindow(const MarkerWindow&) = delete;
    MarkerWindow& operator=(const MarkerWindow&) = delete;

    // Pinta el color y espera a que Windows componga la pantalla (DwmFlush). Devuelve el instante (µs, QPC)
    // en que el cambio quedó en pantalla, o 0 si falló.
    std::uint64_t flash(bool white);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace lancast::sys
