// VR Gaze Mirror 2.0 - the OpenVR helper: the headset's own eye gaze, from its OpenXR runtime.
//
// A SteamVR game gets its gaze from the headset's SteamVR driver, and some drivers pass none on (Pimax's own has no eye
// tracking in it at all) although the same headset's OpenXR runtime has it. That runtime answers through the standard
// eye-gaze interface (XR_EXT_eye_gaze_interaction) to a session that is created but never begun - one that never draws
// and never shows in the headset (tried with Pimax OpenXR, a game running in SteamVR meanwhile). So the helper keeps
// such a dormant session and asks it where the eyes point. Nothing but the headset's own software is involved.
//
// Which runtime: the PC's active one, unless that is SteamVR's own (whose gaze is the one SteamVR already gives); then
// the other installed ones (the registry lists them) - but only one that looks like THIS headset's, by name: loading a
// runtime that belongs to some other headset may start that headset's software.
//
// Used only when SteamVR itself gives no gaze that makes sense. Whatever fails here, fails quietly: the ring's other
// sources remain.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <thread>

#include "../core/pipeline.h"

namespace gaze_mirror {

    class RuntimeGaze {
      public:
        ~RuntimeGaze();

        // Opens the dormant session with the first runtime that will have it. headset: what SteamVR calls the headset
        // (maker, model, tracking system), to tell which installed runtime is its own. Tried once per call (the caller
        // decides when to try again).
        bool start(const std::string& headset);
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
        bool open(const std::string& name, const std::wstring& library);

        Calls* _calls = nullptr;
        uint64_t _instance = 0, _session = 0, _actionSet = 0, _action = 0, _gazeSpace = 0, _viewSpace = 0;
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        std::string _name;
        int _logSample = 0;
        int _logLost = 0;
    };

    // Hands that gaze to an OpenXR game's layer: a game that runs through SteamVR's OpenXR runtime has only the gaze the
    // SteamVR driver passes on, the same as a SteamVR game - so while such a game is mirrored and has no gaze of its
    // own, the helper (which runs next to SteamVR anyway) writes the headset's into a shared block the layer reads.
    // Runs only while that is so; one sample every 8 ms.
    class RuntimeGazeFeed {
      public:
        ~RuntimeGazeFeed();
        void start(RuntimeGaze& source);
        void stop();
        bool running() const {
            return _running;
        }

      private:
        void run(RuntimeGaze* source);

        std::thread _thread;
        std::atomic<bool> _running{false};
        void* _mapping = nullptr;
        volatile ExternalGaze* _block = nullptr;
        LONGLONG _sequence = 0;
    };

} // namespace gaze_mirror
