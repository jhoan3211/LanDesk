#pragma once

// Calibración del codificador del host: cuánto tarda la GPU en codificar un cuadro a la resolución nativa del
// monitor, en 4:2:0 y en 4:4:4 empaquetado. Se mide una vez por tarjeta y versión del driver (unos 3 s, en segundo
// plano) y el cliente lo usa para decidir solo si "Calidad máxima" puede sumar 4:4:4 sin subir la latencia.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "lancast/core/types.hpp"
#include "lancast/platform/error.hpp"

namespace lancast::calib {

struct Result {
    std::string gpu_key;  // Fabricante, modelo y versión del driver.
    Codec codec = Codec::Hevc;
    Size native;
    double ms_420 = 0;  // Captura convertida -> bitstream, mediana (ms).
    double ms_444 = 0;  // Lo mismo en 4:4:4 empaquetado (0 = no se pudo).
};

// Identificador estable de la GPU (el LUID cambia en cada arranque).
[[nodiscard]] std::string gpu_key(std::uint64_t luid);

// Mide el codificador de la GPU `luid` a `native` con contenido en movimiento. `cancel` lo corta (p. ej. si empieza
// una sesión): el resultado queda incompleto y no se guarda.
[[nodiscard]] platform::Result<Result> measure(std::uint64_t luid, Size native, const std::atomic<bool>& cancel);

[[nodiscard]] std::optional<Result> load(const std::string& gpu_key, Size native);
void save(const Result& r);

}  // namespace lancast::calib
