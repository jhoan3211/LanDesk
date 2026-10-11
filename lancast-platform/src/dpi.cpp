#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {

void enable_per_monitor_dpi_awareness() noexcept {
    // Falla con ERROR_ACCESS_DENIED si el manifiesto ya lo fijó, lo cual es el caso esperado.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        (void)GetLastError();
    }
}

}  // namespace lancast::platform
