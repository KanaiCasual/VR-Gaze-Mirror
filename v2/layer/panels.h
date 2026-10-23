// VR Gaze Mirror 2.0 - OpenXR layer: the flat panels other OpenXR tools lay over the game - quad layers, such as
// OpenKneeboard's kneeboards - drawn into the mirror's copy of an eye, where the headset shows them. The runtime's
// compositor lays them over the world in the headset; the mirror, which takes the world's images, does the same.
#pragma once

#include "../core/renderer.h"

namespace gaze_mirror::layer {

    // One panel as it lands in one eye.
    struct Panel {
        ID3D11Texture2D* texture = nullptr; // Its picture (the mirror's copy of it).
        uint32_t arraySlice = 0;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        float uv[4] = {0.f, 0.f, 1.f, 1.f}; // The part of the texture it shows: left, top, right, bottom.
        Vec3 corners[4];                    // In the eye's view, metres (x right, y up, -z ahead): top left, top right, bottom left, bottom right.
        bool sourceAlpha = false;           // Its alpha counts; otherwise it is opaque.
        bool premultiplied = true;          // Its colours are already multiplied by its alpha (the OpenXR default).
    };

    class Panels {
      public:
        bool start(ID3D11Device* device);
        void stop();
        // These textures are going away: the views of them are let go.
        void forget(const std::vector<ID3D11Texture2D*>& textures);
        // Draws the panel into the eye's image. fov: the eye's tangents; window: as for Marker (null = the whole eye).
        // False if the panel is not wholly in front of the eye (it is then left out).
        bool draw(const SourceImage& image, const FovTan& fov, const Panel& panel, const float* window = nullptr);

      private:
        struct Constants {
            float clip[4][4];
            float uv[4][4];
            float mode[4]; // 1 = the alpha counts, 1 = premultiplied.
        };
        ID3D11RenderTargetView* targetOf(const SourceImage& image);
        ID3D11ShaderResourceView* pictureOf(const Panel& panel);
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _immediate;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _deferred;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _sampler;
        std::map<std::pair<ID3D11Texture2D*, uint32_t>, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> _targets;
        std::map<std::pair<ID3D11Texture2D*, uint32_t>, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> _pictures;
        int _logProblem = 0;
    };

} // namespace gaze_mirror::layer
