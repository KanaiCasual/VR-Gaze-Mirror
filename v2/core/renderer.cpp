#include "pch.h"

#include "log.h"
#include "renderer.h"
#include "shader.h"

using Microsoft::WRL::ComPtr;

namespace gaze_mirror {

    Renderer::~Renderer() {
        stop();
    }

    bool Renderer::start(ID3D11Device* device) {
        stop();
        _device = device;
        _device->GetImmediateContext(&_immediate);
        const HRESULT deferred = _device->CreateDeferredContext(0, &_deferred);
        if (FAILED(deferred)) {
            Log("renderer: no deferred context (0x%08x) - this Direct3D device is single-threaded; no mirror", deferred);
            stop();
            return false;
        }
        if (!createPipeline()) {
            stop();
            return false;
        }
        return true;
    }

    void Renderer::stop() {
        forgetAll();
        _snapshotTarget.Reset();
        _snapshotStaging.Reset();
        _snapshotView.Reset();
        _vertexShader.Reset();
        _pixelShader.Reset();
        _snapshotShader.Reset();
        _sampler.Reset();
        _constants.Reset();
        _rasterizer.Reset();
        _blend.Reset();
        _depth.Reset();
        _deferred.Reset();
        _immediate.Reset();
        _device.Reset();
    }

    bool Renderer::createPipeline() {
        ComPtr<ID3DBlob> vertexCode, pixelCode, snapshotCode, messages;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", flags, 0, &vertexCode, &messages)) ||
            FAILED(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", flags, 0, &pixelCode, &messages)) ||
            FAILED(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, nullptr, nullptr, nullptr, "psSnapshot", "ps_5_0", flags, 0, &snapshotCode, &messages))) {
            Log("renderer: shader did not compile: %s", messages ? static_cast<const char*>(messages->GetBufferPointer()) : "?");
            return false;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC depth{};
        const bool made =
            SUCCEEDED(_device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &_vertexShader)) &&
            SUCCEEDED(_device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &_pixelShader)) &&
            SUCCEEDED(_device->CreatePixelShader(snapshotCode->GetBufferPointer(), snapshotCode->GetBufferSize(), nullptr, &_snapshotShader)) &&
            SUCCEEDED(_device->CreateSamplerState(&sampler, &_sampler)) && SUCCEEDED(_device->CreateBuffer(&buffer, nullptr, &_constants)) &&
            SUCCEEDED(_device->CreateRasterizerState(&rasterizer, &_rasterizer)) && SUCCEEDED(_device->CreateBlendState(&blend, &_blend)) &&
            SUCCEEDED(_device->CreateDepthStencilState(&depth, &_depth));
        if (!made) Log("renderer: Direct3D objects could not be created");
        return made;
    }

    void Renderer::forget(const std::vector<ID3D11Texture2D*>& textures) {
        for (auto it = _views.begin(); it != _views.end();) {
            if (std::find(textures.begin(), textures.end(), it->first.first) != textures.end()) it = _views.erase(it);
            else ++it;
        }
    }

    void Renderer::forgetAll() {
        _views.clear();
        _copy.Reset();
        _copyView.Reset();
        _copyWidth = _copyHeight = 0;
    }

    ID3D11ShaderResourceView* Renderer::directView(const SourceImage& source, const D3D11_TEXTURE2D_DESC& desc) {
        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
        const auto key = std::make_pair(source.texture, source.arraySlice);
        auto found = _views.find(key);
        if (found == _views.end()) {
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipLevels = 1;
            viewDesc.Texture2DArray.FirstArraySlice = source.arraySlice;
            viewDesc.Texture2DArray.ArraySize = 1;
            viewDesc.Format = source.viewFormat;
            ComPtr<ID3D11ShaderResourceView> view;
            HRESULT result = _device->CreateShaderResourceView(source.texture, &viewDesc, &view);
            if (FAILED(result)) {
                viewDesc.Format = desc.Format;
                result = _device->CreateShaderResourceView(source.texture, &viewDesc, &view);
            }
            Log("renderer: view of image %p slice %u (texture format %d, read as %d, %u x %u): 0x%08x", source.texture, source.arraySlice,
                int(desc.Format), int(source.viewFormat), desc.Width, desc.Height, result);
            found = _views.emplace(key, view).first; // A null entry too: a failure is not retried every frame.
        }
        return found->second.Get();
    }

    // A view of the source for the crop rectangle; rect = u0, v0, du, dv of the crop in that view. The crop is in pixels
    // of the eye image at its full (nominal) size; the texture may hold that image smaller (quad views' wide image).
    ID3D11ShaderResourceView* Renderer::viewOf(const SourceImage& source, const CropRect& crop, float rect[4]) {
        D3D11_TEXTURE2D_DESC desc;
        source.texture->GetDesc(&desc);
        if (desc.SampleDesc.Count > 1) {
            LogFewTimes(_logProblem, 3, "renderer: the image is multisampled (%u) - not supported yet", desc.SampleDesc.Count);
            return nullptr;
        }
        const float scaleX = float(source.width) / float(std::max(source.fullWidth(), 1)), scaleY = float(source.height) / float(std::max(source.fullHeight(), 1));
        const float left = float(source.x) + float(crop.x) * scaleX, top = float(source.y) + float(crop.y) * scaleY;
        const float across = float(crop.width) * scaleX, down = float(crop.height) * scaleY;
        if (ID3D11ShaderResourceView* view = directView(source, desc)) {
            rect[0] = left / float(desc.Width);
            rect[1] = top / float(desc.Height);
            rect[2] = across / float(desc.Width);
            rect[3] = down / float(desc.Height);
            return view;
        }
        // The image cannot be sampled where it is: copy the crop (only the crop) and sample that.
        const uint32_t boxLeft = std::min(static_cast<uint32_t>(std::max(left, 0.f)), desc.Width - 1), boxTop = std::min(static_cast<uint32_t>(std::max(top, 0.f)), desc.Height - 1);
        const uint32_t width = std::clamp(static_cast<uint32_t>(across + 0.5f), 1u, desc.Width - boxLeft);
        const uint32_t height = std::clamp(static_cast<uint32_t>(down + 0.5f), 1u, desc.Height - boxTop);
        if (!_copy || _copyWidth != width || _copyHeight != height || _copyFormat != source.viewFormat) {
            D3D11_TEXTURE2D_DESC copyDesc{};
            copyDesc.Width = width;
            copyDesc.Height = height;
            copyDesc.MipLevels = 1;
            copyDesc.ArraySize = 1;
            copyDesc.Format = source.viewFormat;
            copyDesc.SampleDesc.Count = 1;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            _copyView.Reset();
            _copy.Reset();
            if (FAILED(_device->CreateTexture2D(&copyDesc, nullptr, &_copy)) || FAILED(_device->CreateShaderResourceView(_copy.Get(), nullptr, &_copyView))) {
                LogFewTimes(_logProblem, 3, "renderer: the copy texture (%u x %u, format %d) could not be made", width, height, int(source.viewFormat));
                _copy.Reset();
                return nullptr;
            }
            _copyWidth = width;
            _copyHeight = height;
            _copyFormat = source.viewFormat;
            Log("renderer: the images cannot be sampled directly - copying the crop (%u x %u) first", width, height);
        }
        const D3D11_BOX box{boxLeft, boxTop, 0, boxLeft + width, boxTop + height, 1};
        _deferred->CopySubresourceRegion(_copy.Get(), 0, 0, 0, 0, source.texture, D3D11CalcSubresource(0, source.arraySlice, desc.MipLevels), &box);
        rect[0] = rect[1] = 0.f;
        rect[2] = rect[3] = 1.f;
        return _copyView.Get();
    }

    bool Renderer::render(Publisher& publisher, const SourceImage& source, const CropRect& crop, uint32_t outWidth, uint32_t outHeight,
                          const RingConstants* ring, int eye) {
        if (!_device || !source.texture || crop.width < 2 || crop.height < 2) return false;
        const Publisher::Slot slot = publisher.acquire(outWidth, outHeight);
        if (slot.index < 0) return false;

        Constants constants{};
        ID3D11ShaderResourceView* view = viewOf(source, crop, constants.sourceRect);
        if (!view) return false;
        constants.output[0] = float(outWidth) / float(outHeight);
        constants.output[1] = source.linearLight ? 1.f : 0.f;
        constants.output[2] = ring ? 1.f : 0.f;

        // Quad views: the sharp inner image goes over the wide one, where the runtime puts it in the headset.
        ID3D11ShaderResourceView* focusView = nullptr;
        if (source.focus && source.focus->texture) {
            const SourceImage& inner = *source.focus;
            D3D11_TEXTURE2D_DESC innerDesc;
            inner.texture->GetDesc(&innerDesc);
            if (innerDesc.SampleDesc.Count == 1) focusView = directView(inner, innerDesc);
            if (focusView) {
                const float fullWidth = float(source.fullWidth()), fullHeight = float(source.fullHeight());
                constants.focusRect[0] = (source.focusWindow[0] * fullWidth - crop.x) / crop.width;
                constants.focusRect[1] = (source.focusWindow[1] * fullHeight - crop.y) / crop.height;
                constants.focusRect[2] = (source.focusWindow[2] * fullWidth - crop.x) / crop.width;
                constants.focusRect[3] = (source.focusWindow[3] * fullHeight - crop.y) / crop.height;
                constants.focusSource[0] = float(inner.x) / float(innerDesc.Width);
                constants.focusSource[1] = float(inner.y) / float(innerDesc.Height);
                constants.focusSource[2] = float(inner.width) / float(innerDesc.Width);
                constants.focusSource[3] = float(inner.height) / float(innerDesc.Height);
                constants.output[3] = 1.f;
            } else {
                LogFewTimes(_logProblem, 3, "renderer: the sharp inner image cannot be sampled where it is - the wide image alone is shown");
            }
        }

        float gazeU = 0.5f, gazeV = 0.5f;
        if (ring) {
            // From eye-image uv to output (crop) uv; distances stay fractions of the eye image's height.
            const float imageWidth = float(source.fullWidth()), imageHeight = float(source.fullHeight());
            const auto toOutput = [&](const float in[2], float out[2]) {
                out[0] = (in[0] * imageWidth - crop.x) / crop.width;
                out[1] = (in[1] * imageHeight - crop.y) / crop.height;
            };
            constants.ring = *ring;
            toOutput(ring->center, constants.ring.center);
            for (int i = 0; i < int(ring->trailCount) && i < MaxTrail; i++) toOutput(ring->trail[i], constants.ring.trail[i]);
            constants.ring.aspect[0] = float(crop.width) / imageHeight;
            constants.ring.aspect[1] = float(crop.height) / imageHeight;
            gazeU = constants.ring.center[0];
            gazeV = constants.ring.center[1];
            // Only the pixels the indicator can touch pay for it (spotlight darkens everything).
            const float marginV = 2.f * ring->radius + 4.f * ring->feather + ring->thickness + 6.f * ring->glowWidth + 0.01f;
            const float marginH = marginV * std::max(ring->stretch, 1.f);
            float x0 = 1.f, y0 = 1.f, x1 = 0.f, y1 = 0.f;
            for (int i = 0; i < int(ring->trailCount) && i < MaxTrail; i++) {
                x0 = std::min(x0, constants.ring.trail[i][0] - marginH / constants.ring.aspect[0]);
                x1 = std::max(x1, constants.ring.trail[i][0] + marginH / constants.ring.aspect[0]);
                y0 = std::min(y0, constants.ring.trail[i][1] - marginV / constants.ring.aspect[1]);
                y1 = std::max(y1, constants.ring.trail[i][1] + marginV / constants.ring.aspect[1]);
            }
            constants.bounds[0] = x0;
            constants.bounds[1] = y0;
            constants.bounds[2] = x1;
            constants.bounds[3] = y1;
            if (ring->style != float(Style::Spotlight) && (x1 <= 0.f || y1 <= 0.f || x0 >= 1.f || y0 >= 1.f)) constants.output[2] = 0.f;
        }

        const D3D11_VIEWPORT viewport{0, 0, float(outWidth), float(outHeight), 0, 1};
        _deferred->UpdateSubresource(_constants.Get(), 0, nullptr, &constants, 0, 0);
        _deferred->OMSetRenderTargets(1, &slot.target, nullptr);
        _deferred->OMSetBlendState(_blend.Get(), nullptr, 0xFFFFFFFF);
        _deferred->OMSetDepthStencilState(_depth.Get(), 0);
        _deferred->RSSetState(_rasterizer.Get());
        _deferred->RSSetViewports(1, &viewport);
        _deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deferred->IASetInputLayout(nullptr);
        _deferred->VSSetShader(_vertexShader.Get(), nullptr, 0);
        _deferred->PSSetShader(_pixelShader.Get(), nullptr, 0);
        _deferred->PSSetConstantBuffers(0, 1, _constants.GetAddressOf());
        ID3D11ShaderResourceView* const sampled[2] = {view, focusView ? focusView : view};
        _deferred->PSSetShaderResources(0, 2, sampled);
        _deferred->PSSetSamplers(0, 1, _sampler.GetAddressOf());
        _deferred->Draw(3, 0);
        ComPtr<ID3D11CommandList> commands;
        if (FAILED(_deferred->FinishCommandList(FALSE, &commands))) {
            LogFewTimes(_logProblem, 3, "renderer: the draw could not be recorded");
            return false;
        }
        _immediate->ExecuteCommandList(commands.Get(), TRUE);
        _immediate->Flush(); // Readers are on other devices: what they are told about must already be on its way.
        publisher.publish(slot, eye, ring != nullptr, gazeU, gazeV);
        LogFewTimes(_logRender, 3, "renderer: frame -> slot %d, crop %d,%d %d x %d of %d x %d -> %u x %u, ring %s", slot.index, crop.x, crop.y,
                    crop.width, crop.height, source.fullWidth(), source.fullHeight(), outWidth, outHeight, ring ? "yes" : "no");
        if (focusView) {
            LogFewTimes(_logFocus, 3, "renderer: quad views - the wide image is %d x %d, the inner one %d x %d over %.0f..%.0f %% across and %.0f..%.0f %% down",
                        source.width, source.height, source.focus->width, source.focus->height, source.focusWindow[0] * 100.f, source.focusWindow[2] * 100.f,
                        source.focusWindow[1] * 100.f, source.focusWindow[3] * 100.f);
        }
        return true;
    }

    bool Renderer::snapshot(const SourceImage& source, uint32_t maxSide, std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height) {
        if (!_device || !source.texture || source.width < 2 || source.height < 2) return false;
        const float scale = float(maxSide) / float(std::max(source.fullWidth(), source.fullHeight()));
        width = std::clamp(uint32_t(source.fullWidth() * scale), 1u, maxSide);
        height = std::clamp(uint32_t(source.fullHeight() * scale), 1u, maxSide);
        D3D11_TEXTURE2D_DESC have{};
        if (_snapshotTarget) _snapshotTarget->GetDesc(&have);
        if (!_snapshotTarget || have.Width != width || have.Height != height) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            _snapshotView.Reset();
            if (FAILED(_device->CreateTexture2D(&desc, nullptr, _snapshotTarget.ReleaseAndGetAddressOf())) ||
                FAILED(_device->CreateRenderTargetView(_snapshotTarget.Get(), nullptr, &_snapshotView))) return false;
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(_device->CreateTexture2D(&desc, nullptr, _snapshotStaging.ReleaseAndGetAddressOf()))) return false;
        }
        const CropRect whole{0, 0, source.fullWidth(), source.fullHeight()};
        Constants constants{};
        ID3D11ShaderResourceView* view = viewOf(source, whole, constants.sourceRect);
        if (!view) return false;
        constants.output[1] = source.linearLight ? 1.f : 0.f;
        constants.output[2] = float(width);
        constants.output[3] = float(height);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        _deferred->UpdateSubresource(_constants.Get(), 0, nullptr, &constants, 0, 0);
        _deferred->OMSetRenderTargets(1, _snapshotView.GetAddressOf(), nullptr);
        _deferred->OMSetBlendState(_blend.Get(), nullptr, 0xFFFFFFFF);
        _deferred->OMSetDepthStencilState(_depth.Get(), 0);
        _deferred->RSSetState(_rasterizer.Get());
        _deferred->RSSetViewports(1, &viewport);
        _deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deferred->IASetInputLayout(nullptr);
        _deferred->VSSetShader(_vertexShader.Get(), nullptr, 0);
        _deferred->PSSetShader(_snapshotShader.Get(), nullptr, 0);
        _deferred->PSSetConstantBuffers(0, 1, _constants.GetAddressOf());
        _deferred->PSSetShaderResources(0, 1, &view);
        _deferred->PSSetSamplers(0, 1, _sampler.GetAddressOf());
        _deferred->Draw(3, 0);
        _deferred->CopyResource(_snapshotStaging.Get(), _snapshotTarget.Get());
        ComPtr<ID3D11CommandList> commands;
        if (FAILED(_deferred->FinishCommandList(FALSE, &commands))) return false;
        _immediate->ExecuteCommandList(commands.Get(), TRUE);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(_immediate->Map(_snapshotStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        pixels.resize(size_t(width) * height * 4);
        for (uint32_t row = 0; row < height; row++) {
            memcpy(pixels.data() + size_t(row) * width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(row) * mapped.RowPitch, size_t(width) * 4);
        }
        _immediate->Unmap(_snapshotStaging.Get(), 0);
        return true;
    }

} // namespace gaze_mirror
