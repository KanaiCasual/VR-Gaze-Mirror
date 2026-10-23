// VR Gaze Mirror 2.0 - OpenXR layer: what the headset shows for the settings app - the calibration marker (only while
// headset_marker=1 in the settings file, which only the settings app sets - nothing pressed in a game can switch it on),
// the capture countdown and the edge finder. Drawn into the layer's own see-through images (premultiplied alpha), which
// go over the game's frame as a layer of their own; never into the game's images.
#pragma once

#include "../core/renderer.h"

namespace gaze_mirror::layer {

    class Marker {
      public:
        bool start(ID3D11Device* device);
        void stop();
        // These images are going away (their swapchain is destroyed): the views of them are let go.
        void forget(const std::vector<ID3D11Texture2D*>& textures);
        // Makes the image wholly transparent, before anything is drawn into it.
        void clear(const SourceImage& image);
        // Every position and size below is in terms of the EYE's whole image. `window` says which part of it this image
        // shows (left, top, right, bottom as fractions); null = all of it. Quad views' sharp inner image shows a part:
        // the same marker, drawn into it with its window, lands where the wide image has it.
        // Draws a ring at ndc (-1..1, up positive) into the image; radius as a fraction of the image height.
        void draw(const SourceImage& image, const float ndc[2], float radius, const float color[3], const float* window = nullptr);
        // Draws the capture countdown (1..99 s) centred at ndc (-1..1, up positive); height = the digits' height as a
        // fraction of the image height.
        void drawCountdown(const SourceImage& image, int seconds, const float ndc[2], float height, const float* window = nullptr);
        // Draws the edge finder over the whole image: black, with what lies outside each edge (fractions of the image:
        // left, right, top, bottom) as a grey block, and outside the edge being set (`activeBar` 1..4) a white block that
        // flashes once a second. `activeBar` 0 paints the image black alone, for the other eye.
        void drawEdges(const SourceImage& image, const float edges[4], int activeBar, const float* window = nullptr);
        static float EdgeFlashPhase(); // 0..1 along the flash (on below 0.5), from the clock.

      private:
        struct Constants {
            float center[2];
            float aspect;
            float radius;
            float color[4];
            float digits[4]; // tens digit (-1 = none), ones digit, digit height as a fraction of the image height, unused.
        };
        ID3D11RenderTargetView* targetOf(const SourceImage& image);
        void drawWith(ID3D11PixelShader* shader, ID3D11RenderTargetView* target, const SourceImage& image, Constants constants, const float* window);
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _immediate;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _deferred;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _digitsShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _barsShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
        Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
        std::map<std::pair<ID3D11Texture2D*, uint32_t>, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> _targets;
        int _logProblem = 0;
    };

} // namespace gaze_mirror::layer
