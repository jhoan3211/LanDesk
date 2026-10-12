#include "lancast/platform/video_processor.hpp"

#include <cmath>

#include "lancast/core/log.hpp"
#include "win.hpp"

namespace lancast::gpu {

platform::Result<std::unique_ptr<VideoProcessor>> VideoProcessor::create(const Device& device, Size input, Size output,
                                                                         ColorConversion conversion) {
    std::unique_ptr<VideoProcessor> vp{new VideoProcessor()};
    vp->input_ = input;
    vp->output_ = output;
    vp->conversion_ = conversion;

    if (FAILED(device.device.As(&vp->video_device_)) || FAILED(device.context.As(&vp->video_context_))) {
        return std::unexpected(platform::Error{"La GPU no expone el procesador de video D3D11", E_NOINTERFACE});
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = input.width;
    desc.InputHeight = input.height;
    desc.OutputWidth = output.width;
    desc.OutputHeight = output.height;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT hr = vp->video_device_->CreateVideoProcessorEnumerator(&desc, &vp->enumerator_);
    if (FAILED(hr)) return platform::detail::hr_error("CreateVideoProcessorEnumerator", hr);
    hr = vp->video_device_->CreateVideoProcessor(vp->enumerator_.Get(), 0, &vp->processor_);
    if (FAILED(hr)) return platform::detail::hr_error("CreateVideoProcessor", hr);

    ID3D11VideoContext* ctx = vp->video_context_.Get();
    ID3D11VideoProcessor* proc = vp->processor_.Get();
    ctx->VideoProcessorSetStreamFrameFormat(proc, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ctx->VideoProcessorSetStreamAutoProcessingMode(proc, 0, FALSE);
    ctx->VideoProcessorSetStreamOutputRate(proc, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);

    // Espacios de color: RGB completo <-> YCbCr BT.709 de rango limitado (lo que señaliza el bitstream).
    const bool to_yuv = conversion == ColorConversion::RgbToYuv709;
    ComPtr<ID3D11VideoContext1> ctx1;
    if (SUCCEEDED(vp->video_context_.As(&ctx1))) {
        const DXGI_COLOR_SPACE_TYPE rgb = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const DXGI_COLOR_SPACE_TYPE yuv = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        ctx1->VideoProcessorSetStreamColorSpace1(proc, 0, to_yuv ? rgb : yuv);
        ctx1->VideoProcessorSetOutputColorSpace1(proc, to_yuv ? yuv : rgb);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE& rgb = to_yuv ? in : out;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE& yuv = to_yuv ? out : in;
        rgb.RGB_Range = 0;  // 0-255
        yuv.YCbCr_Matrix = 1;  // BT.709
        yuv.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        ctx->VideoProcessorSetStreamColorSpace(proc, 0, &in);
        ctx->VideoProcessorSetOutputColorSpace(proc, &out);
    }
    return vp;
}

bool VideoProcessor::process(ID3D11Texture2D* input, UINT input_slice, ID3D11Texture2D* output, const RECT* src,
                             const RECT* dst, std::uint32_t rotation) {
    auto& in_view = input_views_[{input, input_slice}];
    if (!in_view) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.FourCC = 0;
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = 0;
        d.Texture2D.ArraySlice = input_slice;
        if (const HRESULT hr = video_device_->CreateVideoProcessorInputView(input, enumerator_.Get(), &d, &in_view); FAILED(hr)) {
            log::warn("Procesador de video: vista de entrada falló (0x{:08X})", static_cast<std::uint32_t>(hr));
            input_views_.erase({input, input_slice});
            return false;
        }
        if (input_views_.size() > 64) input_views_.clear();  // Evita crecer sin límite si cambian las texturas.
    }
    auto& out_view = output_views_[output];
    if (!out_view) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (const HRESULT hr = video_device_->CreateVideoProcessorOutputView(output, enumerator_.Get(), &d, &out_view); FAILED(hr)) {
            log::warn("Procesador de video: vista de salida falló (0x{:08X})", static_cast<std::uint32_t>(hr));
            output_views_.erase(output);
            return false;
        }
        if (output_views_.size() > 16) output_views_.clear();
    }

    ID3D11VideoContext* ctx = video_context_.Get();
    ID3D11VideoProcessor* proc = processor_.Get();

    const RECT full_in{0, 0, static_cast<LONG>(input_.width), static_cast<LONG>(input_.height)};
    const RECT full_out{0, 0, static_cast<LONG>(output_.width), static_cast<LONG>(output_.height)};
    RECT s = src != nullptr ? *src : full_in;
    RECT d = dst != nullptr ? *dst : full_out;
    // El procesador rechaza un destino que se sale de la salida (p. ej. escala 1:1 en una ventana más chica que el
    // stream: la ventana quedaba negra). Se recorta el destino y el origen en la misma proporción.
    if (rotation == 0 && d.right > d.left && d.bottom > d.top) {
        const double kx = static_cast<double>(s.right - s.left) / static_cast<double>(d.right - d.left);
        const double ky = static_cast<double>(s.bottom - s.top) / static_cast<double>(d.bottom - d.top);
        if (d.left < 0) {
            s.left += static_cast<LONG>(std::lround(-d.left * kx));
            d.left = 0;
        }
        if (d.top < 0) {
            s.top += static_cast<LONG>(std::lround(-d.top * ky));
            d.top = 0;
        }
        if (d.right > full_out.right) {
            s.right -= static_cast<LONG>(std::lround((d.right - full_out.right) * kx));
            d.right = full_out.right;
        }
        if (d.bottom > full_out.bottom) {
            s.bottom -= static_cast<LONG>(std::lround((d.bottom - full_out.bottom) * ky));
            d.bottom = full_out.bottom;
        }
        if (s.right <= s.left || s.bottom <= s.top || d.right <= d.left || d.bottom <= d.top) return true;  // Nada visible.
    }
    ctx->VideoProcessorSetStreamSourceRect(proc, 0, TRUE, &s);
    ctx->VideoProcessorSetStreamDestRect(proc, 0, TRUE, &d);
    ctx->VideoProcessorSetOutputTargetRect(proc, TRUE, &full_out);

    D3D11_VIDEO_COLOR bg{};
    if (conversion_ == ColorConversion::RgbToYuv709) {
        bg.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};  // Negro en rango limitado.
        ctx->VideoProcessorSetOutputBackgroundColor(proc, TRUE, &bg);
    } else {
        bg.RGBA = {0.f, 0.f, 0.f, 1.f};
        ctx->VideoProcessorSetOutputBackgroundColor(proc, FALSE, &bg);
    }

    D3D11_VIDEO_PROCESSOR_ROTATION rot = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    if (rotation == 90) rot = D3D11_VIDEO_PROCESSOR_ROTATION_90;
    if (rotation == 180) rot = D3D11_VIDEO_PROCESSOR_ROTATION_180;
    if (rotation == 270) rot = D3D11_VIDEO_PROCESSOR_ROTATION_270;
    ctx->VideoProcessorSetStreamRotation(proc, 0, rot != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY, rot);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in_view.Get();
    const HRESULT hr = ctx->VideoProcessorBlt(proc, out_view.Get(), 0, 1, &stream);
    if (FAILED(hr)) log::warn("VideoProcessorBlt falló (0x{:08X})", static_cast<std::uint32_t>(hr));
    return SUCCEEDED(hr);
}

}  // namespace lancast::gpu
