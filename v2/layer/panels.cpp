#include "pch.h"

#include "../core/log.h"
#include "panels.h"

using Microsoft::WRL::ComPtr;

namespace gaze_mirror::layer {

    namespace {
        // The four corners come in clip space (w = the distance ahead), so the picture is interpolated with perspective.
        const char PanelShader[] = R"(
cbuffer C : register(b0) { float4 clip[4]; float4 uv[4]; float4 mode; };
Texture2D<float4> picture : register(t0);
SamplerState linearClamp : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.p = clip[id];
    o.uv = uv[id].xy;
    return o;
}
float4 ps(V i) : SV_Target {
    float4 c = picture.Sample(linearClamp, i.uv);
    if (mode.x < 0.5) c.a = 1;               // Its alpha does not count: opaque.
    else if (mode.y < 0.5) c.rgb *= c.a;     // Straight alpha: made premultiplied, as the blend expects.
    return c;
}
)";
    }

    bool Panels::start(ID3D11Device* device) {
        stop();
        _device = device;
        _device->GetImmediateContext(&_immediate);
        if (FAILED(_device->CreateDeferredContext(0, &_deferred))) return false;
        ComPtr<ID3DBlob> vertexCode, pixelCode, messages;
        if (FAILED(D3DCompile(PanelShader, sizeof(PanelShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vertexCode, &messages)) ||
            FAILED(D3DCompile(PanelShader, sizeof(PanelShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pixelCode, &messages))) {
            Log("panels: shader did not compile: %s", messages ? static_cast<const char*>(messages->GetBufferPointer()) : "?");
            stop();
            return false;
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BLEND_DESC blend{}; // Premultiplied "over"; the eye image's own alpha is left as it is.
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = FALSE; // The corners are all in front; nothing is cut by depth.
        rasterizer.ScissorEnable = TRUE;    // Nothing outside the eye image's rectangle of its texture.
        D3D11_DEPTH_STENCIL_DESC depth{};
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        const bool made =
            SUCCEEDED(_device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &_vertexShader)) &&
            SUCCEEDED(_device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &_pixelShader)) &&
            SUCCEEDED(_device->CreateBuffer(&buffer, nullptr, &_constants)) && SUCCEEDED(_device->CreateBlendState(&blend, &_blend)) &&
            SUCCEEDED(_device->CreateRasterizerState(&rasterizer, &_rasterizer)) && SUCCEEDED(_device->CreateDepthStencilState(&depth, &_depth)) &&
            SUCCEEDED(_device->CreateSamplerState(&sampler, &_sampler));
        if (!made) stop();
        return made;
    }

    void Panels::stop() {
        _targets.clear();
        _pictures.clear();
        _vertexShader.Reset();
        _pixelShader.Reset();
        _constants.Reset();
        _blend.Reset();
        _rasterizer.Reset();
        _depth.Reset();
        _sampler.Reset();
        _deferred.Reset();
        _immediate.Reset();
        _device.Reset();
    }

    void Panels::forget(const std::vector<ID3D11Texture2D*>& textures) {
        const auto gone = [&](ID3D11Texture2D* texture) { return std::find(textures.begin(), textures.end(), texture) != textures.end(); };
        for (auto it = _targets.begin(); it != _targets.end();) it = gone(it->first.first) ? _targets.erase(it) : std::next(it);
        for (auto it = _pictures.begin(); it != _pictures.end();) it = gone(it->first.first) ? _pictures.erase(it) : std::next(it);
    }

    ID3D11RenderTargetView* Panels::targetOf(const SourceImage& image) {
        const auto key = std::make_pair(image.texture, image.arraySlice);
        auto found = _targets.find(key);
        if (found == _targets.end()) {
            D3D11_TEXTURE2D_DESC desc;
            image.texture->GetDesc(&desc);
            ComPtr<ID3D11RenderTargetView> target;
            if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) {
                D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
                viewDesc.Format = image.viewFormat;
                viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                viewDesc.Texture2DArray.FirstArraySlice = image.arraySlice;
                viewDesc.Texture2DArray.ArraySize = 1;
                if (FAILED(_device->CreateRenderTargetView(image.texture, &viewDesc, &target))) {
                    viewDesc.Format = desc.Format;
                    _device->CreateRenderTargetView(image.texture, &viewDesc, &target);
                }
            }
            if (!target) LogFewTimes(_logProblem, 3, "panels: the eye image's copy cannot be drawn into (bind flags 0x%x)", desc.BindFlags);
            found = _targets.emplace(key, target).first;
        }
        return found->second.Get();
    }

    ID3D11ShaderResourceView* Panels::pictureOf(const Panel& panel) {
        const auto key = std::make_pair(panel.texture, panel.arraySlice);
        auto found = _pictures.find(key);
        if (found == _pictures.end()) {
            D3D11_TEXTURE2D_DESC desc;
            panel.texture->GetDesc(&desc);
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = panel.viewFormat;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipLevels = desc.MipLevels;
            viewDesc.Texture2DArray.FirstArraySlice = panel.arraySlice;
            viewDesc.Texture2DArray.ArraySize = 1;
            ComPtr<ID3D11ShaderResourceView> picture;
            if (FAILED(_device->CreateShaderResourceView(panel.texture, &viewDesc, &picture))) {
                viewDesc.Format = desc.Format;
                _device->CreateShaderResourceView(panel.texture, &viewDesc, &picture);
            }
            if (!picture) LogFewTimes(_logProblem, 3, "panels: a panel's picture cannot be read (format %d)", int(desc.Format));
            found = _pictures.emplace(key, picture).first;
        }
        return found->second.Get();
    }

    bool Panels::draw(const SourceImage& image, const FovTan& fov, const Panel& panel, const float* window) {
        if (!_device || !image.texture || !panel.texture) return false;
        // ponytail: a panel partly behind the eye is left out whole, not cut at the eye; panels sit well in front.
        for (const Vec3& corner : panel.corners) {
            if (corner.z > -0.01f) return false;
        }
        const float spanX = fov.right - fov.left, spanY = fov.up - fov.down;
        if (spanX < 1e-4f || spanY < 1e-4f) return false;
        ID3D11RenderTargetView* target = targetOf(image);
        ID3D11ShaderResourceView* picture = pictureOf(panel);
        if (!target || !picture) return false;

        Constants constants{};
        const float u[4] = {panel.uv[0], panel.uv[2], panel.uv[0], panel.uv[2]}, v[4] = {panel.uv[1], panel.uv[1], panel.uv[3], panel.uv[3]};
        for (int i = 0; i < 4; i++) {
            const Vec3& p = panel.corners[i];
            const float ahead = -p.z;
            const float x = (2.f * (p.x / ahead) - (fov.right + fov.left)) / spanX; // -1..1 across the eye's view.
            const float y = (2.f * (p.y / ahead) - (fov.up + fov.down)) / spanY;
            constants.clip[i][0] = x * ahead;
            constants.clip[i][1] = y * ahead;
            constants.clip[i][2] = 0.f;
            constants.clip[i][3] = ahead;
            constants.uv[i][0] = u[i];
            constants.uv[i][1] = v[i];
        }
        constants.mode[0] = panel.sourceAlpha ? 1.f : 0.f;
        constants.mode[1] = panel.premultiplied ? 1.f : 0.f;

        // The canvas is the eye's whole view at this image's scale; the viewport is the canvas, the scissor the image (as Marker).
        const float full[4] = {0.f, 0.f, 1.f, 1.f};
        const float* w = window ? window : full;
        if (w[2] - w[0] < 0.01f || w[3] - w[1] < 0.01f) return false;
        const float canvasWidth = float(image.width) / (w[2] - w[0]), canvasHeight = float(image.height) / (w[3] - w[1]);
        const float canvasLeft = float(image.x) - w[0] * canvasWidth, canvasTop = float(image.y) - w[1] * canvasHeight;
        if (canvasWidth > 32000.f || canvasHeight > 32000.f || canvasLeft < -32000.f || canvasTop < -32000.f) return false;
        const D3D11_VIEWPORT viewport{canvasLeft, canvasTop, canvasWidth, canvasHeight, 0, 1};
        const D3D11_RECT scissor{image.x, image.y, image.x + image.width, image.y + image.height};
        _deferred->UpdateSubresource(_constants.Get(), 0, nullptr, &constants, 0, 0);
        _deferred->OMSetRenderTargets(1, &target, nullptr);
        _deferred->OMSetBlendState(_blend.Get(), nullptr, 0xFFFFFFFF);
        _deferred->OMSetDepthStencilState(_depth.Get(), 0);
        _deferred->RSSetState(_rasterizer.Get());
        _deferred->RSSetViewports(1, &viewport);
        _deferred->RSSetScissorRects(1, &scissor);
        _deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        _deferred->IASetInputLayout(nullptr);
        _deferred->VSSetShader(_vertexShader.Get(), nullptr, 0);
        _deferred->VSSetConstantBuffers(0, 1, _constants.GetAddressOf());
        _deferred->PSSetShader(_pixelShader.Get(), nullptr, 0);
        _deferred->PSSetConstantBuffers(0, 1, _constants.GetAddressOf());
        _deferred->PSSetShaderResources(0, 1, &picture);
        _deferred->PSSetSamplers(0, 1, _sampler.GetAddressOf());
        _deferred->Draw(4, 0);
        ComPtr<ID3D11CommandList> commands;
        if (SUCCEEDED(_deferred->FinishCommandList(FALSE, &commands))) _immediate->ExecuteCommandList(commands.Get(), TRUE);
        return true;
    }

} // namespace gaze_mirror::layer
