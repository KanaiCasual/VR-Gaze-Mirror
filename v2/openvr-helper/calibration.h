// VR Gaze Mirror 2.0 - the OpenVR helper: calibration of the gaze that comes from VRChat.
//
// The eye values VRChat's avatar carries are whatever the eye-tracking software makes of the cameras - not angles, and
// not in proportion to them either. So the helper measures the relation once, with a small target shown in the headset
// (an OpenVR overlay, head-locked, two metres away). Two ways:
//   quick    - the target rests at twenty known directions, two seconds each; a curve per axis and a gain per corner.
//   thorough - the target moves over a 5 x 5 grid of directions and the user follows it with the eyes; everything that
//              arrives is recorded (a few thousand pairs), the delay between target and eyes is measured, and a grid map
//              is fitted (core/gaze_grid.h). About two minutes.
// The directions reach 35 degrees to each side, 28 up and 35 down - or as far as both eyes of the headset can show.
//
// Started only from the settings app (vrchat_calibrate=1 or 2 in the file), never by a key: the target is the one thing
// this program ever shows inside the headset, and it goes away by itself.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <openvr.h>

#include "../core/pipeline.h"

namespace gaze_mirror {

    class VrchatOscLink;

    class GazeCalibration {
      public:
        ~GazeCalibration();

        // Runs the whole procedure on its own thread; the result lands in the settings file through `pipeline`.
        // mode: 1 quick, 2 thorough. views/poses: each eye's field of view and place in the head (the targets are kept
        // where both eyes can see them).
        void start(VrchatOscLink& link, Pipeline& pipeline, int mode, const EyeView views[2], const EyePose poses[2]);

        bool running() const {
            return _running;
        }

      private:
        struct Reach {
            float left, right, up, down; // Degrees.
        };
        Reach reach() const;
        void run(VrchatOscLink* link, Pipeline* pipeline);
        bool runQuick(VrchatOscLink* link, Pipeline* pipeline, vr::IVROverlay* overlay, vr::VROverlayHandle_t handle, const Reach& reach, std::string& why);
        bool runThorough(VrchatOscLink* link, Pipeline* pipeline, vr::IVROverlay* overlay, vr::VROverlayHandle_t handle, const Reach& reach, std::string& why);

        std::thread _thread;
        std::atomic<bool> _running{false};
        int _mode = 1;
        EyeView _views[2];
        EyePose _poses[2];
    };

} // namespace gaze_mirror
