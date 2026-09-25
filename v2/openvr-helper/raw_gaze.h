// VR Gaze Mirror 2.0 - the OpenVR helper: raw eye gaze from SRanibro over OSC.
//
// SRanibro takes the headset's eye cameras for itself, so the headset's own gaze (SteamVR's, OpenXR's) is gone while
// it runs, and what reaches VRChat is capped at about 30 degrees. Since 2026-09-24 it can also send the tracker's raw
// gaze straight to us ("Raw gaze OSC" in its settings, 127.0.0.1, port 9005): per eye, per tracker sample,
//
//   /sranibro/gaze/left   x y z valid openness timestamp      (and /sranibro/gaze/right)
//
// x y z the unit direction in head space (x right, y up, forward -z), valid 1/0, openness 0..1. Exact angles, full
// range, nothing to calibrate. The values go into a second shared block (protocol: RawGazeMappingName, the ExternalGaze
// layout with the z fields) that the core reads ahead of the VRChat one. One thread, waiting on its socket.
#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "../protocol/gaze_mirror_protocol.h"

namespace gaze_mirror {

    class RawGazeLink {
      public:
        ~RawGazeLink();

        // Binds 127.0.0.1:<port> and starts the thread. False = the port is taken or Winsock is missing (logged).
        bool start(uint16_t port);
        void stop();

        bool started() const {
            return _running;
        }

      private:
        void run();
        void handlePacket(const uint8_t* data, int length);
        void handleMessage(const uint8_t* data, int length);
        void publish();

        uint16_t _port = 0;
        SOCKET _socket = INVALID_SOCKET;
        std::thread _thread;
        volatile bool _running = false;
        bool _winsock = false;

        HANDLE _mapping = nullptr;
        volatile ExternalGaze* _block = nullptr;
        LONGLONG _sequence = 0;

        struct Eye {
            float dir[3] = {0, 0, -1};
            float openness = 1;
            bool valid = false;
            bool seen = false;
        } _left, _right;
        int _logged = 0;
        std::vector<std::string> _addresses; // The distinct addresses seen (the first 20), for the log.
    };

} // namespace gaze_mirror
