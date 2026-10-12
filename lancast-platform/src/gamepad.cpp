#include "lancast/platform/gamepad.hpp"

#include <windows.h>

#include <ViGEm/Client.h>

#include <mutex>

#include "lancast/core/log.hpp"

// Última versión del driver: el proyecto terminó en 2023 (archivado, sin más actualizaciones).
#define URL_VIGEM "https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0"

namespace lancast::gamepad {

bool driver_available() {
    static const bool available = [] {
        PVIGEM_CLIENT client = vigem_alloc();
        if (client == nullptr) return false;
        const bool ok = VIGEM_SUCCESS(vigem_connect(client));
        if (ok) vigem_disconnect(client);
        vigem_free(client);
        return ok;
    }();
    return available;
}

struct VirtualPads::Impl {
    std::mutex mutex;
    PVIGEM_CLIENT client = nullptr;
    bool failed = false;
    std::array<PVIGEM_TARGET, kMaxPads> pads{};

    bool connect() {
        if (client != nullptr) return true;
        if (failed) return false;
        client = vigem_alloc();
        if (client == nullptr || !VIGEM_SUCCESS(vigem_connect(client))) {
            if (client != nullptr) vigem_free(client);
            client = nullptr;
            failed = true;
            log::warn("Mando: no está instalado el driver ViGEmBus 1.22.0 (" URL_VIGEM ")");
            return false;
        }
        return true;
    }

    void remove(std::size_t i) {
        if (pads[i] == nullptr) return;
        vigem_target_remove(client, pads[i]);
        vigem_target_free(pads[i]);
        pads[i] = nullptr;
        log::info("Mando virtual {} quitado", i + 1);
    }

    void remove_all() {
        for (std::size_t i = 0; i < pads.size(); ++i) remove(i);
        if (client != nullptr) {
            vigem_disconnect(client);
            vigem_free(client);
            client = nullptr;
        }
    }
};

VirtualPads::VirtualPads() : impl_(std::make_unique<Impl>()) {}
VirtualPads::~VirtualPads() { remove_all(); }

void VirtualPads::update(const proto::GamepadState& state) {
    std::scoped_lock lock(impl_->mutex);
    const std::size_t i = state.index;
    if (i >= kMaxPads) return;
    if (!state.connected) {
        if (impl_->client != nullptr) impl_->remove(i);
        return;
    }
    if (!impl_->connect()) return;
    if (impl_->pads[i] == nullptr) {
        PVIGEM_TARGET pad = vigem_target_x360_alloc();
        if (pad == nullptr) return;
        if (!VIGEM_SUCCESS(vigem_target_add(impl_->client, pad))) {
            vigem_target_free(pad);
            log::warn("Mando: no se pudo crear el mando virtual {}", i + 1);
            return;
        }
        impl_->pads[i] = pad;
        log::info("Mando virtual {} conectado (Xbox 360)", i + 1);
    }
    XUSB_REPORT report{};
    report.wButtons = state.buttons;
    report.bLeftTrigger = state.left_trigger;
    report.bRightTrigger = state.right_trigger;
    report.sThumbLX = state.left_x;
    report.sThumbLY = state.left_y;
    report.sThumbRX = state.right_x;
    report.sThumbRY = state.right_y;
    vigem_target_x360_update(impl_->client, impl_->pads[i], report);
}

void VirtualPads::remove_all() {
    std::scoped_lock lock(impl_->mutex);
    impl_->remove_all();
}

}  // namespace lancast::gamepad
