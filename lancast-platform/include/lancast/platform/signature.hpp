#pragma once

// Firma digital (Authenticode) de un ejecutable: válida o no, y a nombre de quién.

#include <filesystem>
#include <string>

namespace lancast::sig {

struct Info {
    bool valid = false;    // Firmado y la firma verifica (cadena de confianza de Windows).
    std::string subject;   // Nombre del editor del certificado ("CN"), vacío si no hay firma.
    std::string error;     // Motivo si no es válida.
};

[[nodiscard]] Info verify(const std::filesystem::path& file);

}  // namespace lancast::sig
