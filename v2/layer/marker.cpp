#include "pch.h"

#include "../core/log.h"
#include "marker.h"

using Microsoft::WRL::ComPtr;

namespace gaze_mirror::layer {

    namespace {
        const char MarkerShader[] = R"(
cbuffer C : register(b0) { float2 center; float aspect; float radius; float4 color; float4 digits; };
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// The 1.x reticle: four corner brackets on a box the size of the ring, and a small cross in the middle.
float4 ps(V i) : SV_Target {
    float2 a = abs((i.uv - center) * float2(aspect, 1));
    float h = radius;          // Half-size of the box, a fraction of the image height (like the ring's radius).
    float arm = h * 0.5;       // Each bracket arm reaches half way back towards the centre.
    float lw = 0.0009;         // Line width, a fraction of the image height.
    float edge = 1 - smoothstep(lw * 0.5, lw * 0.5 + 0.0005, min(abs(a.y - h), abs(a.x - h)));
    float onHorizontal = (1 - smoothstep(lw * 0.5, lw * 0.5 + 0.0005, abs(a.y - h))) * step(h - arm, a.x) * step(a.x, h + lw * 0.5);
    float onVertical = (1 - smoothstep(lw * 0.5, lw * 0.5 + 0.0005, abs(a.x - h))) * step(h - arm, a.y) * step(a.y, h + lw * 0.5);
    float cross = (1 - smoothstep(lw * 0.5, lw * 0.5 + 0.0005, min(a.x, a.y))) * step(max(a.x, a.y), h * 0.25);
    float alpha = max(max(onHorizontal, onVertical), cross) * 0.9;
    return float4(color.rgb, alpha);
}
// The capture countdown: seven-segment digits (the same geometry as SevenSegmentCoverage in the core), white with a
// dark outline, in a box of digits.z image heights, centred.
static const int masks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
static const float2 segCenter[7] = {float2(0, -0.45), float2(0.25, -0.23), float2(0.25, 0.23), float2(0, 0.45), float2(-0.25, 0.23), float2(-0.25, -0.23), float2(0, 0)};
static const float2 segHalf[7] = {float2(0.19, 0.045), float2(0.045, 0.19), float2(0.045, 0.19), float2(0.19, 0.045), float2(0.045, 0.19), float2(0.045, 0.19), float2(0.19, 0.045)};
float segments(float2 p, int digit, float grow) {
    float best = 0;
    [unroll] for (int s = 0; s < 7; s++) {
        if ((masks[digit] & (1 << s)) == 0) continue;
        float2 o = abs(p - segCenter[s]) - (segHalf[s] + grow);
        float outside = max(max(o.x, 0), max(o.y, 0));
        best = max(best, saturate(1 - outside / 0.012));
    }
    return best;
}
float4 psDigits(V i) : SV_Target {
    float2 p = (i.uv - center) * float2(aspect, 1) / digits.z;
    float inner = 0, outer = 0;
    if (digits.x >= 0) {
        inner = max(inner, segments(p - float2(-0.42, 0), int(digits.x), 0));
        outer = max(outer, segments(p - float2(-0.42, 0), int(digits.x), 0.05));
        inner = max(inner, segments(p - float2(0.42, 0), int(digits.y), 0));
        outer = max(outer, segments(p - float2(0.42, 0), int(digits.y), 0.05));
    } else {
        inner = segments(p, int(digits.y), 0);
        outer = segments(p, int(digits.y), 0.05);
    }
    float3 rgb = lerp(float3(0.02, 0.02, 0.02), color.rgb, inner);
    return float4(rgb, outer * 0.9);
}
// The edge finder: the whole image goes black; what lies outside an edge (digits = left, right, top, bottom as
// fractions of the image) is a solid block, grey for the edges that stand, white and flashing (there for half a
// second, gone for half: center.x = the phase, 0..1) for the one being set (radius = 1..4): the block is the part of
// the image the eye should not see, and it is pushed out until it is gone. radius = 0: black alone (the other eye).
float4 psBars(V i) : SV_Target {
    int active = int(radius + 0.5);
    float outsideLeft = step(i.uv.x, digits.x), outsideRight = step(digits.y, i.uv.x);
    float outsideTop = step(i.uv.y, digits.z), outsideBottom = step(digits.w, i.uv.y);
    float outside = max(max(outsideLeft, outsideRight), max(outsideTop, outsideBottom));
    float act = active == 1 ? outsideLeft : active == 2 ? outsideRight : active == 3 ? outsideTop : active == 4 ? outsideBottom : 0;
    float flash = center.x < 0.5 ? 1.0 : 0.0;
    float level = active > 0 ? lerp(0.25 * outside, flash, act) : 0;
    return float4(level, level, level, 1);
}
)";
    }

    bool Marker::start(ID3D11Device* device) {
        stop();
        _device = device;
        _device->GetImmediateContext(&_immediate);
        if (FAILED(_device->CreateDeferredContext(0, &_deferred))) return false;
        ComPtr<ID3DBlob> vertexCode, pixelCode, messages;
        ComPtr<ID3DBlob> digitsCode, barsCode;
        if (FAILED(D3DCompile(MarkerShader, sizeof(MarkerShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vertexCode, &messages)) ||
            FAILED(D3DCompile(MarkerShader, sizeof(MarkerShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pixelCode, &messages)) ||
            FAILED(D3DCompile(MarkerShader, sizeof(MarkerShader) - 1, nullptr, nullptr, nullptr, "psDigits", "ps_5_0", 0, 0, &digitsCode, &messages)) ||
            FAILED(D3DCompile(MarkerShader, sizeof(MarkerShader) - 1, nullptr, nullptr, nullptr, "psBars", "ps_5_0", 0, 0, &barsCode, &messages))) {
            Log("marker: shader did not compile: %s", messages ? static_cast<const char*>(messages->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(_device->CreatePixelShader(digitsCode->GetBufferPointer(), digitsCode->GetBufferSize(), nullptr, &_digitsShader))) return false;
        if (FAILED(_device->CreatePixelShader(barsCode->GetBufferPointer(), barsCode->GetBufferSize(), nullptr, &_barsShader))) return false;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE; // Nothing is ever drawn outside the image's own rectangle of its texture.
        D3D11_DEPTH_STENCIL_DESC depth{};
        const bool made =
            SUCCEEDED(_device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &_vertexShader)) &&
            SUCCEEDED(_device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &_pixelShader)) &&
            SUCCEEDED(_device->CreateBuffer(&buffer, nullptr, &_constants)) && SUCCEEDED(_device->CreateBlendState(&blend, &_blend)) &&
            SUCCEEDED(_device->CreateRasterizerState(&rasterizer, &_rasterizer)) && SUCCEEDED(_device->CreateDepthStencilState(&depth, &_depth));
        if (!made) stop();
        return made;
    }

    void Marker::stop() {
        _targets.clear();
        _vertexShader.Reset();
        _pixelShader.Reset();
        _constants.Reset();
        _blend.Reset();
        _rasterizer.Reset();
        _depth.Reset();
        _deferred.Reset();
        _immediate.Reset();
        _device.Reset();
    }

    void Marker::forget(const std::vector<ID3D11Texture2D*>& textures) {
        for (auto it = _targets.begin(); it != _targets.end();) {
            if (std::find(textures.begin(), textures.end(), it->first.first) != textures.end()) it = _targets.erase(it);
            else ++it;
        }
    }

    ID3D11RenderTargetView* Marker::targetOf(const SourceImage& image) {
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
            if (!target) LogFewTimes(_logProblem, 3, "marker: the headset image cannot be drawn into (bind flags 0x%x)", desc.BindFlags);
            found = _targets.emplace(key, target).first;
        }
        return found->second.Get();
    }

    void Marker::draw(const SourceImage& image, const float ndc[2], float radius, const float color[3], const float* window) {
        if (!_device || !image.texture) return;
        ID3D11RenderTargetView* target = targetOf(image);
        if (!target) return;
        Constants constants{};
        constants.center[0] = (ndc[0] + 1.f) * 0.5f;
        constants.center[1] = (1.f - ndc[1]) * 0.5f;
        constants.radius = radius;
        for (int i = 0; i < 3; i++) constants.color[i] = image.linearLight ? powf(color[i], 2.2f) : color[i];
        constants.color[3] = 1.f;
        drawWith(_pixelShader.Get(), target, image, constants, window);
    }

    void Marker::drawCountdown(const SourceImage& image, int seconds, const float ndc[2], float height, const float* window) {
        if (!_device || !image.texture || seconds <= 0 || seconds > 99) return;
        ID3D11RenderTargetView* target = targetOf(image);
        if (!target) return;
        Constants constants{};
        constants.center[0] = (ndc[0] + 1.f) * 0.5f;
        constants.center[1] = (1.f - ndc[1]) * 0.5f;
        constants.color[0] = constants.color[1] = constants.color[2] = constants.color[3] = 1.f;
        constants.digits[0] = seconds >= 10 ? float(seconds / 10) : -1.f;
        constants.digits[1] = float(seconds % 10);
        constants.digits[2] = std::clamp(height, 0.01f, 0.5f); // The digit box's height, a fraction of the image height.
        drawWith(_digitsShader.Get(), target, image, constants, window);
    }

    float Marker::EdgeFlashPhase() {
        // One flash a second (on for the first half), from the clock: every image of a frame gets the same phase.
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return float(ms % 1000) / 1000.f;
    }

    void Marker::drawEdges(const SourceImage& image, const float edges[4], int activeBar, const float* window) {
        if (!_device || !image.texture || activeBar < 0 || activeBar > 4) return;
        ID3D11RenderTargetView* target = targetOf(image);
        if (!target) return;
        Constants constants{};
        constants.center[0] = EdgeFlashPhase();
        constants.radius = float(activeBar);
        constants.color[0] = constants.color[1] = constants.color[2] = constants.color[3] = 1.f;
        for (int i = 0; i < 4; i++) constants.digits[i] = edges[i];
        drawWith(_barsShader.Get(), target, image, constants, window);
    }

    void Marker::drawWith(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, const SourceImage& image, Constants constants, const float* window) {
        // The canvas is the eye's whole image at this image's scale; the viewport is the canvas, the scissor the image.
        const float full[4] = {0.f, 0.f, 1.f, 1.f};
        const float* w = window ? window : full;
        if (w[2] - w[0] < 0.01f || w[3] - w[1] < 0.01f) return;
        const float canvasWidth = float(image.width) / (w[2] - w[0]), canvasHeight = float(image.height) / (w[3] - w[1]);
        const float canvasLeft = float(image.x) - w[0] * canvasWidth, canvasTop = float(image.y) - w[1] * canvasHeight;
        if (canvasWidth > 32000.f || canvasHeight > 32000.f || canvasLeft < -32000.f || canvasTop < -32000.f) return; // Beyond what a viewport may be.
        constants.aspect = canvasWidth / canvasHeight;
        const D3D11_VIEWPORT viewport{canvasLeft, canvasTop, canvasWidth, canvasHeight, 0, 1};
        const D3D11_RECT scissor{image.x, image.y, image.x + image.width, image.y + image.height};
        _deferred->UpdateSubresource(_constants.Get(), 0, nullptr, &constants, 0, 0);
        _deferred->OMSetRenderTargets(1, &target, nullptr);
        _deferred->OMSetBlendState(_blend.Get(), nullptr, 0xFFFFFFFF);
        _deferred->OMSetDepthStencilState(_depth.Get(), 0);
        _deferred->RSSetState(_rasterizer.Get());
        _deferred->RSSetViewports(1, &viewport);
        _deferred->RSSetScissorRects(1, &scissor);
        _deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _deferred->IASetInputLayout(nullptr);
        _deferred->VSSetShader(_vertexShader.Get(), nullptr, 0);
        _deferred->PSSetShader(shader, nullptr, 0);
        _deferred->PSSetConstantBuffers(0, 1, _constants.GetAddressOf());
        _deferred->Draw(3, 0);
        ComPtr<ID3D11CommandList> commands;
        if (SUCCEEDED(_deferred->FinishCommandList(FALSE, &commands))) _immediate->ExecuteCommandList(commands.Get(), TRUE);
    }

} // namespace gaze_mirror::layer
