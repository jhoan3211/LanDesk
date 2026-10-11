#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {

SystemReport collect_system_report() {
    SystemReport report;

    if (auto gpus = enumerate_gpus()) {
        report.gpus = std::move(*gpus);
    } else {
        report.warnings.push_back("GPUs: " + gpus.error().message());
    }

    if (auto monitors = enumerate_monitors(report.gpus)) {
        report.monitors = std::move(*monitors);
        for (const auto& m : report.monitors) report.virtual_desktop = report.virtual_desktop.united(m.desktop);
    } else {
        report.warnings.push_back("Monitores: " + monitors.error().message());
    }

    if (auto encoders = enumerate_encoders(report.gpus)) {
        report.encoders = std::move(*encoders);
        for (const auto& e : report.encoders) {
            if (!e.runtime_present && e.backend != EncoderBackend::Qsv) {
                report.warnings.push_back(e.ffmpeg_name + ": el driver expone el codificador pero falta " +
                                          e.runtime_dll + " en System32 (reinstala el driver de la GPU).");
            }
        }
    } else {
        report.warnings.push_back("Codificadores: " + encoders.error().message());
    }

    if (auto decoders = enumerate_decoders(report.gpus)) {
        report.decoders = std::move(decoders->decoders);
        report.other_decoder_profiles = std::move(decoders->unknown_profiles);
    } else {
        report.warnings.push_back("Decodificadores: " + decoders.error().message());
    }

    if (report.encoders.empty()) {
        report.warnings.push_back("No hay codificador por hardware: este equipo no puede actuar como host.");
    }
    return report;
}

}  // namespace lancast::platform
