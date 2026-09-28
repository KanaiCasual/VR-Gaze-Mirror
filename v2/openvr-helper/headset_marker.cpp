#ifndef NOMINMAX
#define NOMINMAX // windows.h (through the core's headers) must not turn std::min into a macro.
#endif
#include "headset_marker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../core/log.h"
#include "../core/ring.h"

namespace gaze_mirror {

    namespace {

        constexpr float Distance = 2.f; // Metres out along the eye's line of sight.
        constexpr int Size = 128;
        constexpr float BoxHalf = 0.8f; // The bracket box's half-size in the overlay's -1..1 square (a margin around it).

        // The 1.x reticle in the given colour: four corner brackets on a box, and a small cross in the middle. Drawn
        // with 4x4 samples per pixel, so the thin lines get soft edges.
        std::vector<uint8_t> ReticlePixels(const float color[3]) {
            const float arm = BoxHalf * 0.5f, lw = 0.05f, cross = BoxHalf * 0.25f;
            const auto inside = [&](float ax, float ay) {
                const bool horizontal = std::fabs(ay - BoxHalf) < lw * 0.5f && ax >= BoxHalf - arm && ax <= BoxHalf + lw * 0.5f;
                const bool vertical = std::fabs(ax - BoxHalf) < lw * 0.5f && ay >= BoxHalf - arm && ay <= BoxHalf + lw * 0.5f;
                const bool centre = std::min(ax, ay) < lw * 0.5f && std::max(ax, ay) < cross;
                return horizontal || vertical || centre;
            };
            std::vector<uint8_t> pixels(size_t(Size) * Size * 4, 0);
            for (int y = 0; y < Size; y++) {
                for (int x = 0; x < Size; x++) {
                    int covered = 0;
                    for (int sy = 0; sy < 4; sy++) {
                        for (int sx = 0; sx < 4; sx++) {
                            const float nx = ((x + (sx + 0.5f) / 4.f) / Size) * 2.f - 1.f;
                            const float ny = ((y + (sy + 0.5f) / 4.f) / Size) * 2.f - 1.f;
                            if (inside(std::fabs(nx), std::fabs(ny))) covered++;
                        }
                    }
                    uint8_t* p = &pixels[(size_t(y) * Size + x) * 4];
                    for (int i = 0; i < 3; i++) p[i] = uint8_t(std::clamp(color[i], 0.f, 1.f) * 255.f + 0.5f);
                    p[3] = uint8_t(covered * 255 / 16);
                }
            }
            return pixels;
        }

    } // namespace

    HeadsetMarker::~HeadsetMarker() {
        hide();
        if (_handle != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) vr::VROverlay()->DestroyOverlay(_handle);
        if (_countdown != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) vr::VROverlay()->DestroyOverlay(_countdown);
    }

    // The seconds as seven-segment digits (the core's geometry), white with a dark outline, 256 x 128 RGBA.
    static std::vector<uint8_t> CountdownPixels(int seconds) {
        constexpr int W = 256, H = 128;
        std::vector<uint8_t> pixels(size_t(W) * H * 4, 0);
        const int tens = seconds >= 10 ? seconds / 10 : -1, ones = seconds % 10;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                // Digit-box units: the box height is 0.8 of the texture height, x follows the same scale.
                const float px = (x + 0.5f - W / 2.f) / (H * 0.8f), py = (y + 0.5f - H / 2.f) / (H * 0.8f);
                float inner = 0, outer = 0;
                if (tens >= 0) {
                    inner = std::max(SevenSegmentCoverage(px + 0.42f, py, tens, 0.f), SevenSegmentCoverage(px - 0.42f, py, ones, 0.f));
                    outer = std::max(SevenSegmentCoverage(px + 0.42f, py, tens, 0.05f), SevenSegmentCoverage(px - 0.42f, py, ones, 0.05f));
                } else {
                    inner = SevenSegmentCoverage(px, py, ones, 0.f);
                    outer = SevenSegmentCoverage(px, py, ones, 0.05f);
                }
                uint8_t* p = &pixels[(size_t(y) * W + x) * 4];
                const uint8_t grey = uint8_t(5 + 250 * inner);
                p[0] = p[1] = p[2] = grey;
                p[3] = uint8_t(outer * 230);
            }
        }
        return pixels;
    }

    void HeadsetMarker::updateCountdown(int seconds) {
        vr::IVROverlay* overlay = vr::VROverlay();
        if (!overlay) return;
        if (seconds <= 0 || seconds > 99) {
            if (_countdownShown != 0 && _countdown != vr::k_ulOverlayHandleInvalid) overlay->HideOverlay(_countdown);
            _countdownShown = 0;
            return;
        }
        if (_countdown == vr::k_ulOverlayHandleInvalid) {
            if (overlay->CreateOverlay("kanaicasual.gazemirror.countdown", "Gaze Mirror capture countdown", &_countdown) != vr::VROverlayError_None) {
                _countdown = vr::k_ulOverlayHandleInvalid;
                return;
            }
            overlay->SetOverlayWidthInMeters(_countdown, 0.5f);
            const vr::HmdMatrix34_t m = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -2.f}}}; // Straight ahead, two metres out.
            overlay->SetOverlayTransformTrackedDeviceRelative(_countdown, vr::k_unTrackedDeviceIndex_Hmd, &m);
        }
        if (seconds != _countdownShown) {
            const std::vector<uint8_t> pixels = CountdownPixels(seconds);
            overlay->SetOverlayRaw(_countdown, const_cast<uint8_t*>(pixels.data()), 256, 128, 4);
            if (_countdownShown == 0) overlay->ShowOverlay(_countdown);
            _countdownShown = seconds;
        }
    }

    bool HeadsetMarker::ensure(const float color[3]) {
        vr::IVROverlay* overlay = vr::VROverlay();
        if (!overlay) return false;
        if (_handle == vr::k_ulOverlayHandleInvalid) {
            const vr::EVROverlayError error = overlay->CreateOverlay("kanaicasual.gazemirror.marker", "Gaze Mirror calibration marker", &_handle);
            if (error != vr::VROverlayError_None) {
                _handle = vr::k_ulOverlayHandleInvalid;
                if (_logProblem++ == 0) Log("marker: the overlay could not be made (%d)", int(error));
                return false;
            }
            Log("marker: shown in the headset while the app's calibration switch is on");
        }
        if (color[0] != _color[0] || color[1] != _color[1] || color[2] != _color[2]) {
            const std::vector<uint8_t> pixels = ReticlePixels(color);
            overlay->SetOverlayRaw(_handle, const_cast<uint8_t*>(pixels.data()), Size, Size, 4);
            for (int i = 0; i < 3; i++) _color[i] = color[i];
        }
        return true;
    }

    void HeadsetMarker::update(const FrameOutput& output, const FrameInput& input, int eye) {
        const EyeView& view = input.views[eye];
        if (!output.marker || !view.valid || !ensure(output.markerColor)) {
            hide();
            return;
        }
        // The ring's spot in the eye's image (NDC, up positive) -> a direction in that eye's view, then into head space.
        const float tanX = view.fov.left + (output.markerNdc[eye][0] + 1.f) * 0.5f * (view.fov.right - view.fov.left);
        const float tanY = view.fov.down + (output.markerNdc[eye][1] + 1.f) * 0.5f * (view.fov.up - view.fov.down);
        const EyePose& pose = input.eyeInHead[eye];
        Vec3 direction = Rotate(Normalize(pose.orientation), Vec3{tanX, tanY, -1.f});
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        direction = {direction.x / length, direction.y / length, direction.z / length};
        const Vec3 at{pose.position.x + direction.x * Distance, pose.position.y + direction.y * Distance, pose.position.z + direction.z * Distance};
        // The ring's radius, as the fraction of the image height it is, becomes an angle: the bracket box (which sits at
        // BoxHalf of the overlay's half-width) gets that half-size two metres out.
        const float tanRadius = output.markerRadius * (view.fov.up - view.fov.down);
        const float width = std::clamp(2.f * Distance * tanRadius / BoxHalf, 0.02f, 1.5f);
        vr::IVROverlay* overlay = vr::VROverlay();
        // Head-locked, facing the head (x right, y up, z towards the viewer).
        const vr::HmdMatrix34_t m = {{{1, 0, 0, at.x}, {0, 1, 0, at.y}, {0, 0, 1, at.z}}};
        overlay->SetOverlayTransformTrackedDeviceRelative(_handle, vr::k_unTrackedDeviceIndex_Hmd, &m);
        overlay->SetOverlayWidthInMeters(_handle, width);
        if (!_shown) {
            overlay->ShowOverlay(_handle);
            _shown = true;
        }
    }

    void HeadsetMarker::hide() {
        if (_shown && vr::VROverlay()) vr::VROverlay()->HideOverlay(_handle);
        _shown = false;
        updateCountdown(0);
    }

} // namespace gaze_mirror
