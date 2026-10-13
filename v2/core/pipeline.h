// VR Gaze Mirror 2.0 - core: one frame, start to finish. The OpenXR layer and the OpenVR helper both hand
// this the same things - the eye images, where the head is, where the eyes point - and it does the rest: settings,
// ring motion, framing, the one draw, the crop tool's picture, the calibration nudge keys, the frame-rate cap.
#pragma once

#include <chrono>

#include "external_gaze.h"
#include "framing.h"
#include "publisher.h"
#include "renderer.h"
#include "ring.h"
#include "settings.h"
#include "snapshot.h"

namespace gaze_mirror {

    // How the eye tracker's answer arrives.
    struct GazeFrame {
        bool valid = false;
        // A ray in VIEW (head) space: OpenXR's eye-gaze extension.
        bool hasRay = false;
        Vec3 origin{0, 0, 0};
        Vec3 direction{0, 0, -1};
        // Or already a point in each eye's image (0..1, origin top left): SteamVR's per-eye gaze.
        bool hasPerEyeUv = false;
        float uv[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    };

    // Where each eye sits in the head (some headsets' displays are turned outwards).
    struct EyePose {
        Vec3 position{0, 0, 0};
        Quat orientation{0, 0, 0, 1};
    };

    struct FrameInput {
        const SourceImage* images[2] = {nullptr, nullptr}; // Per eye; the one not mirrored may be null (snapshot only).
        EyeView views[2];                                  // Per eye: size, fov, head orientation.
        EyePose eyeInHead[2];
        GazeFrame gaze;
        int gazeSupport = 0; // What the producer knows: 0 nothing, 1 the headset gives eye gaze, 2 it does not (the OpenXR layer asks).
    };

    struct FrameOutput {
        bool rendered = false;
        // Calibration marker for the headset (headset_marker=1): per eye, NDC (-1..1, up positive); the producer that
        // can draw into the headset shows a marker there.
        bool marker = false;
        float markerNdc[2][2] = {};
        float markerRadius = 0.f; // Fraction of the eye image height.
        float markerColor[3] = {};
        // The app's capture countdown, seconds left (0 = none): the producer shows the number in the middle of the view.
        int countdownSeconds = 0;
        // Where to draw it, per eye (NDC, -1..1, up positive): a point two metres straight ahead of the head, so both eyes
        // see ONE number at a distance (the SteamVR helper's overlay sits there too). The middle of each eye's image is
        // not that: with the usual asymmetric views the two middles point tens of degrees apart and never fuse.
        float countdownNdc[2][2] = {};
        // How tall the digits are, per eye, as a fraction of that eye's image height: the size of a 20 cm digit two
        // metres away, the same as the SteamVR helper's overlay, whatever the eye's field of view.
        float countdownHeight[2] = {0.05f, 0.05f};
        // The app's visible-edge finder: the bar being set (0 = off) and the four bars as fractions of the mirrored eye's
        // image (left, right, top, bottom); drawn in that eye only.
        int edgeBar = 0;
        float edges[4] = {};
        int mirroredEye = 1;
    };

    class Pipeline {
      public:
        // underSteamVR: an OpenXR game on SteamVR's runtime - it yields to the SteamVR helper unless picture_source=game.
        // takeOver: the helper takes the picture from such a game's layer.
        bool start(ID3D11Device* device, LONG producerKind, const char* program, const char* application, bool underSteamVR = false, bool takeOver = false);
        // For an OpenXR game's layer that does not publish: whether it should start now - nobody publishes, or the SteamVR
        // helper does while the game's own picture is wanted. A memory read and the settings signal; called now and then.
        bool layerShouldPublish(bool underSteamVR);
        void stop();
        bool started() const {
            return _publisher.started();
        }
        bool lost() const {
            return _publisher.lost();
        }

        // Cheap. Whether the frame is worth handing over at all (a reader wants it, and it is due).
        bool wanted();

        FrameOutput frame(const FrameInput& input);

        const Settings& settings() const {
            return _settings.get();
        }
        Settings& mutableSettings() {
            return _settings.mutableSettings();
        }
        // Writes values into the settings file (the helper's calibration result); the app picks them up from there.
        void persist(const std::vector<std::pair<std::string, std::string>>& values) {
            _settings.persist(values);
        }
        // A calibrated curve: straight lines between its points, continued straight beyond the ends.
        static float MapValue(const std::vector<std::pair<float, float>>& map, float value);
        static void CornerCorrect(const std::vector<Settings::CornerGain>& corners, float& x, float& y);
        void forget(const std::vector<ID3D11Texture2D*>& textures) {
            _renderer.forget(textures);
        }

      private:
        bool target(const FrameInput& input, int eye, float out[2]) const;
        // The frame's gaze, or VRChat's in its place (gaze_source), as a ray in head space.
        GazeFrame chooseGaze(const FrameInput& input);
        void nudgeKeys();
        void watchCoverKey();

        SettingsSource _settings;
        Publisher _publisher;
        Renderer _renderer;
        RingState _ring;
        Framing _framing;
        ExternalGazeReader _external;                    // VRChat's avatar eyes (the fallback).
        ExternalGazeReader _raw{RawGazeMappingName};    // Raw gaze from the gaze input port (exact, ahead of the fallback).
        ExternalGazeReader _handed{RuntimeGazeMappingName}; // The headset's own gaze, handed over by the helper.
        SnapshotService _snapshot;
        std::chrono::steady_clock::time_point _lastFrame{};
        std::chrono::steady_clock::time_point _nextDue{};
        bool _nudgeWasDown = false;
        HANDLE _coverKeyEvent = nullptr;
        bool _coverKeyReady = false; // The key has been seen up since its last press.
        ULONGLONG _nudgeNextRepeatMs = 0;
        bool _calibrationDirty = false;
        ULONGLONG _calibrationDirtySinceMs = 0;
        bool _snapshotDue = false; // Decided in wanted(), used up in frame().
        bool _renderDue = false;
        std::chrono::steady_clock::time_point _headsetGazeAt{}; // When the game's own runtime last gave a gaze.
        bool _handedGazeUsed = false;  // This frame's headset gaze came through the helper.
        int _logHanded = 0;
        int _logGaze = 0;
        int _logExternal = 0;
        int _logRaw = 0;
    };

} // namespace gaze_mirror
