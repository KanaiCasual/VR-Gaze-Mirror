// VR Gaze Mirror 2.0 - the OpenVR helper: the calibration marker inside the headset for SteamVR games.
//
// The OpenXR layer draws the marker straight into the game's images. This program has no hand in a SteamVR game's
// images, so it shows the marker as an OpenVR overlay instead: the corner-bracket reticle in the ring's colour, head-locked, two
// metres out along the line from the recorded eye through where the stream's ring is. Only while headset_marker=1 in
// the settings file, which only the settings app sets - nothing pressed in a game can switch it on.
#pragma once

#include <openvr.h>

#include "../core/pipeline.h"

namespace gaze_mirror {

    class HeadsetMarker {
      public:
        ~HeadsetMarker();

        // Shows the marker where `output` says the ring is for the recorded `eye`, or hides it when there is none.
        void update(const FrameOutput& output, const FrameInput& input, int eye);
        // Shows the capture countdown (seconds, 0 = none) straight ahead, two metres out.
        void updateCountdown(int seconds);
        // Shows the edge finder's bars over the mirrored eye's view (a side-by-side overlay: the other eye sees nothing).
        void updateEdges(const FrameOutput& output, const FrameInput& input);
        void hide(); // Everything: the marker, the countdown, the edge bars.
        void hideMarker();

      private:
        bool ensure(const float color[3]);

        vr::VROverlayHandle_t _handle = vr::k_ulOverlayHandleInvalid;
        bool _shown = false;
        vr::VROverlayHandle_t _countdown = vr::k_ulOverlayHandleInvalid;
        int _countdownShown = 0;
        vr::VROverlayHandle_t _edges = vr::k_ulOverlayHandleInvalid;
        bool _edgesShown = false;
        int _edgesBar = 0, _edgesEye = -1, _edgesHeight = 0, _edgesFlashStep = -1;
        float _edgesValues[4] = {};
        float _color[3] = {-1.f, -1.f, -1.f};
        int _logProblem = 0;
    };

} // namespace gaze_mirror
