// VR Gaze Mirror 2.0 - core: settings.
//
// The same file and the same "something changed" signal the settings app already uses, so the app drives 2.0 as it
// is: the file is read once at start and again only when the app bumps its counter in shared memory - compared once
// per frame, a memory read. Every key of 1.x means the same thing here.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "log.h"

namespace gaze_mirror {

    enum class Style { Ring = 0, Glow = 1, Dot = 2, Spotlight = 3, Bubble = 4, Solid = 5, Heatmap = 6, Ghost = 7 };

    struct Settings {
        // Ring.
        bool enabled = true;
        Style style = Style::Ghost;
        bool screenBlend = true;
        float solidity = 0.4f;
        float radius = 0.042f; // Sizes: fractions of the (uncropped) eye image height.
        float thickness = 0.0010f;
        float feather = 0.0010f;
        float color[3] = {40 / 255.f, 170 / 255.f, 245 / 255.f};
        float opacity = 0.5f;
        float fillOpacity = 0.f;
        float glowWidth = 0.0028f;
        float glowStrength = 0.4f;
        float tailOpacity = 0.7f;
        float tailMax = 0.14f;
        float trailMs = 150.f;
        float blobTrailMs = 450.f;
        float heatMs = 2000.f;
        float heatCoolMs = 700.f;
        bool worldAnchored = true;
        float shadowOpacity = 0.3f;
        // Motion.
        bool adaptiveFilter = true;
        float filterMinCutoff = 1.5f;
        float filterBeta = 8.f;
        float smoothingMs = 60.f;
        float holdMs = 300.f;
        float fadeMs = 150.f;
        float dwellMs = 500.f;
        float dwellShrink = 0.25f;
        // Placement.
        float focusDistance = 0.f; // Metres; 0 = far away.
        float offsetX = 0.f;       // Trim, fractions of the image height (+X right, +Y up).
        float offsetY = 0.f;
        bool headsetMarker = false;
        // Crop.
        bool cropEnabled = false;
        float cropAspect = 16.f / 9.f; // 0 = free (cropWidth counts).
        float cropCenterX = 0.5f;
        float cropCenterY = 0.5f;
        float cropHeight = 0.5f;
        float cropWidth = 1.f;
        bool cropFollowVertical = false;
        float cropFollowDeadzone = 0.4f;
        float cropFollowReach = 1.f;
        float cropFollowGlideMs = 500.f;
        bool cropFollowReturn = true;
        float cropFollowHomeMs = 1200.f;
        bool stabilize = false;
        float stabilizeMinCutoff = 1.f;
        float stabilizeBeta = 8.f;
        float stabilizeRoom = 0.02f;
        // New in 2.0.
        int eye = 1;                // mirror_eye=left|right. The right eye unless told otherwise.
        uint32_t outputMaxSide = 3840; // output_max_side: the published picture's longest side is never more.
        float outputFps = 0.f;      // output_fps: 0 = every frame the game makes.
        int gazeSource = 0;         // gaze_source=auto|headset|vrchat|sranibro: 0 headset, else SRanibro raw, else VRChat; 1/2/3 that one only.
        int rawGazePort = 9005;     // raw_gaze_port: where the helper listens for SRanibro's raw gaze (its "Raw gaze OSC" port).
        float vrchatScale = 1.6f;   // vrchat_scale: multiplies the avatar's sideways gaze before it becomes a direction (setups differ).
        float vrchatScaleUp = 1.6f;   // vrchat_scale_up: the same for looking up...
        float vrchatScaleDown = 1.6f; // vrchat_scale_down: ...and down (trackers under the eye read the two very differently).
        bool vrchatCalibrate = false; // vrchat_calibrate=1: the app asks the helper to run the in-headset calibration once.
        // vrchat_map_x / vrchat_map_y: "value:tangent,..." from that calibration, sorted by value. With two or more
        // points on each axis these replace the scales: value -> tangent of the angle, straight lines between points.
        std::vector<std::pair<float, float>> vrchatMapX, vrchatMapY;
        // vrchat_map_corners: "tx,ty,gainX,gainY;..." for the four quadrants (+x+y, -x+y, +x-y, -x-y) from the diagonal
        // targets: at the corner (tx, ty) the axis maps alone came out short or long by these factors. Empty = no correction.
        struct CornerGain {
            float tx, ty, gainX, gainY;
        };
        std::vector<CornerGain> vrchatCorners;
        std::string vrchatCalibrated; // vrchat_calibrated: what the last calibration left ("running n/m" while it goes).
    };

    class SettingsSource {
      public:
        ~SettingsSource();

        // Reads the file now and connects to the app's signal.
        void open();

        // The game this producer runs for; a crop profile made for it (crop-profiles.ini, see below) then overrides the
        // crop keys of the file, now and at every reload. Empty = no profile.
        void setGame(const char* program, const char* application);
        const std::string& activeProfile() const {
            return _activeProfile;
        }

        // Once per frame. True when the settings were read again.
        bool refreshIfSignalled();

        const Settings& get() const {
            return _settings;
        }
        Settings& mutableSettings() {
            return _settings;
        }

        // Writes these keys back into the file, leaving every other line exactly as it is (the calibration nudge).
        void persist(const std::vector<std::pair<std::string, std::string>>& values);

        // The app's block: also carries the crop tool's picture request and the capture key.
        struct Signal {
            uint32_t magic;
            uint32_t version;
            volatile LONG generation;
            volatile LONG snapshotRequest;
            volatile LONG captureKey;
        };
        Signal* signal() const {
            return _signal;
        }

      private:
        void read();

        void applyProfile(Settings& settings);
        static void ApplyKey(Settings& settings, const std::string& key, const std::string& value);

        Settings _settings;
        std::string _program, _application, _activeProfile;
        HANDLE _mapping = nullptr;
        Signal* _signal = nullptr;
        LONG _seenGeneration = 0;
    };

    std::wstring SettingsFilePath();
    // %LocalAppData%\GazeMirror\crop-profiles.ini: "[name]" sections with "game=<program or application>"
    // and any crop_* / stabilize* keys, written by the settings app.
    std::wstring ProfilesFilePath();

} // namespace gaze_mirror
