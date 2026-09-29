// VR Gaze Mirror 2.0 - the OpenVR helper: the headset's own eye gaze, from the active OpenXR runtime.
//
// A SteamVR game gets its gaze from the headset's SteamVR driver, and some drivers pass none on (Pimax's own has no eye
// tracking in it at all) although the same headset's OpenXR runtime has it. That runtime answers through the standard
// eye-gaze interface (XR_EXT_eye_gaze_interaction) to a session that is created but never begun - one that never draws
// and never shows in the headset (tried with Pimax OpenXR, a game running in SteamVR meanwhile). So the helper keeps
// such a dormant session and asks it where the eyes point. Nothing but the headset's own software is involved.
//
// Used only when SteamVR itself gives no gaze that makes sense, and only when the active runtime is not SteamVR's own
// (whose gaze is the one SteamVR already gives). Whatever fails here, fails quietly: the ring's other sources remain.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>

#include "../core/pipeline.h"

namespace gaze_mirror {

    class RuntimeGaze {
      public:
        ~RuntimeGaze();

        // Opens the dormant session. False when there is no other runtime than SteamVR's, or it has no eye gaze, or
        // will not have a second program; tried once per call (the caller decides when to try again).
        bool start();
        void stop();
        bool started() const {
            return _session != 0;
        }
        const std::string& runtimeName() const {
            return _name;
        }

        // Where the eyes point now, as a ray in head space. Not valid when the tracker does not see the eyes.
        GazeFrame sample();

      private:
        struct Calls; // The runtime's functions.
        Calls* _calls = nullptr;
        void* _module = nullptr;
        uint64_t _instance = 0, _session = 0, _actionSet = 0, _action = 0, _gazeSpace = 0, _viewSpace = 0;
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        std::string _name;
        int _logSample = 0;
        int _logLost = 0;
    };

} // namespace gaze_mirror
