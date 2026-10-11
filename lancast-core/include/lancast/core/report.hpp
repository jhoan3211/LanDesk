#pragma once

#include <string>

#include "lancast/core/types.hpp"

namespace lancast {

// Texto legible para `--list` (en español). `verbose` añade LUIDs y perfiles de decodificación desconocidos.
[[nodiscard]] std::string format_report_text(const SystemReport& report, bool verbose);

// JSON estable para `--list --json` (útil para scripts y para comparar host/cliente).
[[nodiscard]] std::string format_report_json(const SystemReport& report);

}  // namespace lancast
