// VR Gaze Mirror 2.0 - OpenXR layer: the calibration marker, drawn INTO the headset's images just before the
// game's frame is handed on. Only while headset_marker=1 in the settings file, which only the settings app sets -
// nothing pressed in a game can switch it on.
#pragma once

#include "../core/renderer.h"

namespace gaze_mirror::layer {

    class Marker {
      public:
        bool start(ID3D11Device* device);
        void stop();
        // Draws a ring at ndc (-1..1, up positive) into the image; radius as a fraction of the image height.
        void draw(const SourceImage& image, const float ndc[2], float radius, const float color[3]);
        // Draws the capture countdown (1..99 s) large in the middle of the image.
        void drawCountdown(const SourceImage& image, int seconds);

      private:
        struct Constants {
            float center[2];
            float aspect;
            float radius;
            float color[4];
            float digits[4]; // tens digit (-1 = none), ones digit, digit height as a fraction of the image height, unused.
        };
        ID3D11RenderTargetView* targetOf(const SourceImage& image);
        void drawWith(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, const SourceImage& image, const Constants& constants);
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _immediate;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _deferred;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _digitsShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
        std::map<std::pair<ID3D11Texture2D*, uint32_t>, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> _targets;
        int _logProblem = 0;
    };

} // namespace gaze_mirror::layer
