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
    struct Ctx {
        Impl* impl = nullptr;
        std::uint8_t index = 0;
    };
    std::array<Ctx, kMaxPads> ctx{};  // userData de la notificación de cada mando (direcciones fijas).
    std::mutex rumble_mutex;
    RumbleCallback on_rumble;

    static VOID CALLBACK notification(PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR strong, UCHAR weak, UCHAR, LPVOID user) {
        auto* c = static_cast<Ctx*>(user);
        std::scoped_lock lock(c->impl->rumble_mutex);
        if (c->impl->on_rumble) c->impl->on_rumble(c->index, strong, weak);
    }

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
        vigem_target_x360_unregister_notification(pads[i]);
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
        impl_->ctx[i] = {impl_.get(), static_cast<std::uint8_t>(i)};
        if (!VIGEM_SUCCESS(vigem_target_x360_register_notification(impl_->client, pad, &Impl::notification, &impl_->ctx[i]))) {
            log::warn("Mando virtual {}: sin vibración", i + 1);
        }
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

void VirtualPads::set_rumble_callback(RumbleCallback cb) {
    std::scoped_lock lock(impl_->rumble_mutex);
    impl_->on_rumble = std::move(cb);
}

void VirtualPads::remove_all() {
    std::scoped_lock lock(impl_->mutex);
    impl_->remove_all();
}

}  // namespace lancast::gamepad
