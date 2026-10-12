#include "lancast/platform/capture.hpp"

#include <algorithm>

#include "lancast/core/log.hpp"
#include "lancast/platform/desktop.hpp"
#include "win.hpp"

namespace lancast::capture {

struct DesktopCapture::Output {
    gpu::ComPtr<IDXGIOutputDuplication> duplication;
    Rect rect;  // Escritorio virtual.
    std::uint32_t rotation = 0;
    Size size;  // Tamaño de la superficie duplicada (sin rotar).
    std::vector<std::uint8_t> shape_buffer;
};

namespace {

std::uint32_t rotation_degrees(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_ROTATE90:
            return 90;
        case DXGI_MODE_ROTATION_ROTATE180:
            return 180;
        case DXGI_MODE_ROTATION_ROTATE270:
            return 270;
        default:
            return 0;
    }
}

}  // namespace

DesktopCapture::~DesktopCapture() = default;

platform::Result<std::unique_ptr<DesktopCapture>> DesktopCapture::create(const gpu::Device& device,
                                                                         const std::vector<Rect>& monitor_rects,
                                                                         const Rect& source, bool hdr) {
    std::unique_ptr<DesktopCapture> cap{new DesktopCapture()};
    cap->device_ = device;
    cap->hdr_ = hdr;
    cap->monitor_rects_ = monitor_rects;
    cap->source_ = source;
    if (auto r = cap->init_outputs(); !r) return std::unexpected(r.error());
    return cap;
}

platform::Result<void> DesktopCapture::init_outputs() {
    outputs_.clear();
    // Como servicio (SYSTEM): seguir a la pantalla de bloqueo / UAC. Para un usuario normal no hace nada.
    desk::attach_to_input_desktop();
    gpu::ComPtr<IDXGIOutput> output;
    for (UINT i = 0; device_.adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i, output.Reset()) {
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
        const Rect rect{desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                        desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
                        desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top};
        if (std::ranges::find(monitor_rects_, rect) == monitor_rects_.end()) continue;

        auto out = std::make_unique<Output>();
        out->rect = rect;
        HRESULT hr = E_NOINTERFACE;
        gpu::ComPtr<IDXGIOutput5> output5;
        if (SUCCEEDED(output.As(&output5))) {
            const DXGI_FORMAT formats[] = {format()};
            hr = output5->DuplicateOutput1(device_.device.Get(), 0, 1, formats, &out->duplication);
        }
        if (FAILED(hr) && hdr_) return platform::detail::hr_error("Captura HDR (FP16) no disponible", hr);
        if (FAILED(hr)) {
            gpu::ComPtr<IDXGIOutput1> output1;
            if (SUCCEEDED(output.As(&output1))) hr = output1->DuplicateOutput(device_.device.Get(), &out->duplication);
        }
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                return platform::detail::hr_error("Demasiadas aplicaciones capturando la pantalla", hr);
            }
            return platform::detail::hr_error("DuplicateOutput", hr);
        }
        DXGI_OUTDUPL_DESC dd{};
        out->duplication->GetDesc(&dd);
        out->rotation = rotation_degrees(dd.Rotation);
        out->size = Size{dd.ModeDesc.Width, dd.ModeDesc.Height};
        outputs_.push_back(std::move(out));
    }
    if (outputs_.empty()) {
        return std::unexpected(platform::Error{"El monitor no está conectado a la GPU usada para capturar", 0});
    }

    // Un solo monitor: textura del tamaño de la superficie (la rotación la aplica el procesador de video).
    Size size;
    if (outputs_.size() == 1) {
        size = outputs_[0]->size;
        rotation_ = outputs_[0]->rotation;
    } else {
        size = Size{static_cast<std::uint32_t>(source_.width), static_cast<std::uint32_t>(source_.height)};
        rotation_ = 0;
    }
    if (!composite_ || texture_size_ != size) {
        auto tex = gpu::create_texture(device_.device.Get(), size.width, size.height, format(),
                                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!tex) return std::unexpected(tex.error());
        composite_ = *tex;
        texture_size_ = size;
        if (outputs_.size() > 1) {
            // Fondo negro para las zonas del escritorio virtual sin monitor.
            gpu::ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(device_.device->CreateRenderTargetView(composite_.Get(), nullptr, &rtv))) {
                const float black[4] = {0, 0, 0, 1};
                device_.context->ClearRenderTargetView(rtv.Get(), black);
            }
        }
    }
    return {};
}

Status DesktopCapture::acquire_output(Output& out, std::uint32_t timeout_ms, bool& updated) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    gpu::ComPtr<IDXGIResource> resource;
    const HRESULT hr = out.duplication->AcquireNextFrame(timeout_ms, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Status::Timeout;
    if (FAILED(hr)) return Status::Error;

    if (info.LastPresentTime.QuadPart != 0) {
        static const std::int64_t freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        const std::int64_t q = info.LastPresentTime.QuadPart;
        last_present_us_ = static_cast<std::uint64_t>(q / freq * 1'000'000 + (q % freq) * 1'000'000 / freq);
    }
    if (info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0) {
        gpu::ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            if (outputs_.size() == 1) {
                device_.context->CopyResource(composite_.Get(), tex.Get());
            } else {
                const UINT dx = static_cast<UINT>(out.rect.x - source_.x);
                const UINT dy = static_cast<UINT>(out.rect.y - source_.y);
                D3D11_BOX box{0, 0, 0, std::min(out.size.width, texture_size_.width - dx),
                              std::min(out.size.height, texture_size_.height - dy), 1};
                device_.context->CopySubresourceRegion(composite_.Get(), 0, dx, dy, 0, tex.Get(), 0, &box);
            }
            updated = true;
        }
    }

    if (info.LastMouseUpdateTime.QuadPart != 0) {
        pointer_.visible = info.PointerPosition.Visible != FALSE;
        if (pointer_.visible) {
            pointer_.x = out.rect.x + info.PointerPosition.Position.x;
            pointer_.y = out.rect.y + info.PointerPosition.Position.y;
        }
    }
    if (info.PointerShapeBufferSize > 0) {
        out.shape_buffer.resize(info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        if (SUCCEEDED(out.duplication->GetFramePointerShape(static_cast<UINT>(out.shape_buffer.size()),
                                                            out.shape_buffer.data(), &required, &shape))) {
            proto::CursorShape c;
            c.type = static_cast<proto::CursorType>(shape.Type);
            c.width = shape.Width;
            c.height = shape.Height;
            c.pitch = shape.Pitch;
            c.hot_x = static_cast<std::int32_t>(shape.HotSpot.x);
            c.hot_y = static_cast<std::int32_t>(shape.HotSpot.y);
            c.data.assign(out.shape_buffer.begin(),
                          out.shape_buffer.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                                         out.shape_buffer.size(), static_cast<std::size_t>(shape.Pitch) * shape.Height)));
            pending_shape_ = std::move(c);
        }
    }
    out.duplication->ReleaseFrame();
    return Status::NewFrame;
}

Status DesktopCapture::acquire(std::uint32_t timeout_ms) {
    if (outputs_.empty()) {
        // Recreación pendiente (p. ej. tras cambio de modo o escritorio seguro): reintentar con pausa.
        const std::uint64_t now = gpu::now_us();
        if (now - last_recreate_us_ < 200'000) {
            Sleep(std::min<std::uint32_t>(timeout_ms, 20));
            return Status::Timeout;
        }
        last_recreate_us_ = now;
        if (auto r = init_outputs(); !r) {
            log::debug("Captura: reintentando ({})", r.error().message());
            outputs_.clear();
            return Status::Timeout;
        }
        log::info("Captura recreada");
        return Status::Recreated;
    }

    bool updated = false;
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        // Con varios monitores se sondea sin espera y solo el primero espera (poco).
        const std::uint32_t wait = outputs_.size() == 1 ? timeout_ms : (i == 0 ? std::min<std::uint32_t>(timeout_ms, 2) : 0);
        const Status s = acquire_output(*outputs_[i], wait, updated);
        if (s == Status::Error) {
            outputs_.clear();  // ACCESS_LOST u otro error: recrear en la próxima llamada.
            return Status::Recreated;
        }
    }
    return updated ? Status::NewFrame : Status::Timeout;
}

std::optional<proto::CursorShape> DesktopCapture::take_pointer_shape() {
    auto out = std::move(pending_shape_);
    pending_shape_.reset();
    return out;
}

}  // namespace lancast::capture
