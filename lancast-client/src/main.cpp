// lancast-client: fase 1 — inventario de hardware (`--list`), centrado en la decodificación.
// Las fases siguientes añaden la red, la decodificación D3D11VA, la presentación y la interfaz.

#include <windows.h>

#include <cstdio>
#include <string_view>

#include "lancast/core/report.hpp"
#include "lancast/platform/platform.hpp"

namespace {

constexpr std::string_view kUsage =
    "LanCast cliente 0.1.0\n"
    "\n"
    "Uso: lancast-client --list [--json] [--verbose]\n"
    "\n"
    "  --list      Muestra GPUs, monitores y decodificadores por hardware disponibles\n"
    "  --json      Salida en JSON\n"
    "  --verbose   Incluye LUIDs y perfiles DXVA no reconocidos\n"
    "  --help      Muestra esta ayuda\n";

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    lancast::platform::enable_per_monitor_dpi_awareness();

    bool list = false;
    bool json = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--list" || arg == "-l") {
            list = true;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage.data(), stdout);
            return 0;
        } else {
            std::fprintf(stderr, "Argumento desconocido: %s\n\n%s", argv[i], kUsage.data());
            return 2;
        }
    }

    if (!list) {
        std::fputs(kUsage.data(), stdout);
        return 0;
    }

    lancast::SystemReport report = lancast::platform::collect_system_report();
    // El aviso de "no puede ser host" no aplica al cliente.
    std::erase_if(report.warnings, [](const std::string& w) { return w.starts_with("No hay codificador"); });
    if (report.decoders.empty()) {
        report.warnings.push_back("No hay decodificación HEVC/AV1/H.264 por hardware: el cliente usaría la CPU.");
    }

    const std::string text = json ? lancast::format_report_json(report) : lancast::format_report_text(report, verbose);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return report.gpus.empty() ? 1 : 0;
}
