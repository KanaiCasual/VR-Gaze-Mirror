// VR Gaze Mirror 2.0 - core: which part of the eye image goes out.
//   - the crop box the user placed (the whole image when cropping is off);
//   - follow: the box glides up and down so that what is looked at stays in frame;
//   - steadying: the head orientation goes through a One-Euro filter and the box counter-moves small head movement
//     inside the room kept free around it - sideways and up and down, and by turning about its middle against a
//     sideways tilt of the head. Moving and turning the box is free - it is only where the one draw samples from.
#pragma once

#include "math.h"
#include "settings.h"

namespace gaze_mirror {

    struct CropRect {
        int32_t x = 0, y = 0;
        int32_t width = 0, height = 0;
    };

    class Framing {
      public:
        // The box the user placed, in pixels of the eye image (same rules as 1.x: even sizes, never outside).
        static CropRect placedBox(const Settings& settings, float imageWidth, float imageHeight);

        // Once per frame, after RingState::update. gazeY: the ring's smoothed vertical position (pixels), when there is one.
        void update(const Settings& settings, const EyeView& view, const CropRect& placed, bool haveGaze, bool hasGazePosition, float gazeY, float dtMs);

        // The box as it is this frame: placed + follow + steadying, clamped to the image.
        CropRect current() const {
            return _current;
        }
        float followOffset() const {
            return _followOffset;
        }
        // The angle the box is turned by about its middle this frame, radians, in the image's own sense (x to the right,
        // y down): the picture's "to the right" points along (cos, sin) in the eye image. 0 = level.
        float roll() const {
            return _roll;
        }

        void reset();

      private:
        void updateFollow(const Settings& settings, const EyeView& view, const CropRect& placed, bool haveGaze, bool hasGazePosition, float gazeY, float dtMs);
        void updateStabilizer(const Settings& settings, const EyeView& view, const CropRect& placed, float dtMs);

        CropRect _current;
        float _followOffset = 0.f, _followTarget = 0.f, _followStillMs = 0.f;
        Quat _stabOrientation{0, 0, 0, 1}, _stabRawPrev{0, 0, 0, 1};
        bool _stabHasOrientation = false;
        float _stabSpeed = 0.f;
        float _stabOffset[2] = {0.f, 0.f};
        float _stabRoll = 0.f; // What the steadying asks for...
        float _roll = 0.f;     // ...and what the room around the box, where it stands this frame, allows.
        float _stabLogLargestRoll = 0.f;
        int _stabLogFrames = 0;
        float _stabLogLargest = 0.f;
    };

} // namespace gaze_mirror
