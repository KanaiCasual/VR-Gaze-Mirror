// VR Gaze Mirror 2.0 - core: the one draw per frame.
//
// Samples the eye image where it is (a view of it, no copy), cropped and scaled, and composites the ring in the same
// pixel shader - all eight looks of 1.x, with "screen" or "normal" blending done in the shader. The result lands in a
// shared slot texture. Everything is recorded on a deferred context and played back with "restore state", so the
// game's (or the helper's) own Direct3D state is never disturbed.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <map>
#include <utility>
#include <vector>

#include "framing.h"
#include "publisher.h"
#include "ring.h"

namespace gaze_mirror {

    struct SourceImage {
        ID3D11Texture2D* texture = nullptr;
        uint32_t arraySlice = 0;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // The format to read it as (a swapchain's; the texture may be typeless).
        int32_t x = 0, y = 0;                         // The eye image inside the texture.
        int32_t width = 0, height = 0;
        bool linearLight = false;                     // sRGB / float formats: sampled values are linear.
        // Quad views: this is the wide, coarse image of the eye, and a sharp inner image lies over part of it - the
        // mirror lays it there too. nominal*: the size the eye image would have at the inner image's sharpness; crop,
        // ring and output go by that size (0 = the image's own). focusWindow: the part of this image the inner one
        // covers, as fractions (left, top, right, bottom).
        int32_t nominalWidth = 0, nominalHeight = 0;
        const SourceImage* focus = nullptr;
        float focusWindow[4] = {0.f, 0.f, 1.f, 1.f};
        int32_t fullWidth() const {
            return nominalWidth > 0 ? nominalWidth : width;
        }
        int32_t fullHeight() const {
            return nominalHeight > 0 ? nominalHeight : height;
        }
    };

    class Renderer {
      public:
        ~Renderer();

        bool start(ID3D11Device* device);
        void stop();

        // Draws one frame: source -> crop -> scale -> ring -> slot. ring may be null (nothing drawn).
        bool render(Publisher& publisher, const SourceImage& source, const CropRect& crop, uint32_t outWidth, uint32_t outHeight,
                    const RingConstants* ring, int eye);

        // A small box-filtered copy of the WHOLE eye image, read back to memory (BGRA, top row first) - the settings
        // app's crop tool. Slow (a GPU round trip): only on request.
        bool snapshot(const SourceImage& source, uint32_t maxSide, std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height);

        // These textures are going away: let go of every view of them.
        void forget(const std::vector<ID3D11Texture2D*>& textures);
        void forgetAll();

      private:
        struct Constants {
            float sourceRect[4]; // u0, v0, du, dv in the source texture.
            float output[4];     // width / height; source is linear light; ring on; unused.
            float bounds[4];     // The output uv rectangle the ring can touch.
            float focusRect[4];  // Quad views: where the inner image lies in the output (x0, y0, x1, y1)...
            float focusSource[4]; // ...and where it is in its texture (u0, v0, du, dv). On when output[3] is 1.
            RingConstants ring;
        };
        ID3D11ShaderResourceView* viewOf(const SourceImage& source, const CropRect& crop, float rect[4]);
        // The image's own view (no copy); null when it cannot be sampled where it is.
        ID3D11ShaderResourceView* directView(const SourceImage& source, const D3D11_TEXTURE2D_DESC& desc);
        bool createPipeline();

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _immediate;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _deferred;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _snapshotShader;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
        std::map<std::pair<ID3D11Texture2D*, uint32_t>, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> _views;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _copy;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _copyView;
        uint32_t _copyWidth = 0, _copyHeight = 0;
        DXGI_FORMAT _copyFormat = DXGI_FORMAT_UNKNOWN;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _snapshotTarget, _snapshotStaging;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _snapshotView;
        int _logProblem = 0;
        int _logRender = 0;
        int _logFocus = 0;
    };

} // namespace gaze_mirror
