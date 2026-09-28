// VR Gaze Mirror 2.0 - core: the ring's motion - everything between "the eye points there" and the numbers
// the shader draws with. Positions are pixels of the (uncropped) eye image; sizes are fractions of its height.
//   - smoothing (One-Euro or simple), hold through blinks, fade in and out;
//   - world anchoring: when the head turns, everything remembered moves with the scene;
//   - the ghost tail, the bubble/solid trail, the heatmap's spots, the dwell feedback.
#pragma once

// Seven-segment digits, for the capture countdown shown in the headset (the layer's shader carries the same geometry).
// (x, y) in a digit box of -0.3..0.3 by -0.5..0.5 (y down); returns 1 inside a lit segment of `digit`, with soft edges,
// or - with `grow` - inside the segment grown by that much (the dark outline).
namespace gaze_mirror {
    inline float SevenSegmentCoverage(float x, float y, int digit, float grow) {
        static const int masks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111}; // a=1 b=2 c=4 d=8 e=16 f=32 g=64
        static const float centers[7][2] = {{0, -0.45f}, {0.25f, -0.23f}, {0.25f, 0.23f}, {0, 0.45f}, {-0.25f, 0.23f}, {-0.25f, -0.23f}, {0, 0}};
        static const float halves[7][2] = {{0.19f, 0.045f}, {0.045f, 0.19f}, {0.045f, 0.19f}, {0.19f, 0.045f}, {0.045f, 0.19f}, {0.045f, 0.19f}, {0.19f, 0.045f}};
        if (digit < 0 || digit > 9) return 0.f;
        float best = 0.f;
        for (int s = 0; s < 7; s++) {
            if (!(masks[digit] & (1 << s))) continue;
            const float dx = (x - centers[s][0]) < 0 ? -(x - centers[s][0]) : (x - centers[s][0]);
            const float dy = (y - centers[s][1]) < 0 ? -(y - centers[s][1]) : (y - centers[s][1]);
            const float ox = dx - (halves[s][0] + grow), oy = dy - (halves[s][1] + grow);
            const float outside = (ox > 0 ? ox : 0) > (oy > 0 ? oy : 0) ? (ox > 0 ? ox : 0) : (oy > 0 ? oy : 0);
            const float cover = 1.f - outside / 0.012f;
            if (cover > best) best = cover;
        }
        return best < 0 ? 0.f : best > 1 ? 1.f : best;
    }
} // namespace gaze_mirror

#include <deque>

#include "math.h"
#include "settings.h"

namespace gaze_mirror {

    constexpr int MaxTrail = 32;

    // Exactly the shader's constant buffer.
    struct RingConstants {
        float center[2];
        float aspect[2];
        float radius;
        float thickness;
        float feather;
        float fillOpacity;
        float color[4];
        float style;
        float shadowOpacity;
        float trailCount;
        float heatGain;
        float glowWidth;
        float tailOpacity;
        float premultiply;
        float solidity;
        float glowStrength;
        float stretch; // Width / height of the ring: 1 round, more = an ellipse (the fixed centre's sharp area).
        float padding[2];
        float trail[MaxTrail][4];
    };

    class RingState {
      public:
        // Once per frame. haveTarget: the eye tracker gave a position (target, pixels of the eye image) this frame.
        // view: the image the target is in, with the head orientation it was rendered under (for the anchoring).
        void update(const Settings& settings, const EyeView& view, bool haveTarget, const float target[2], float dtMs);

        // Something is worth drawing this frame.
        bool visible(const Settings& settings) const;
        // The constants for the whole eye image (the renderer maps them into the crop).
        void constants(const Settings& settings, const EyeView& view, RingConstants& out) const;

        bool hasPosition() const {
            return _hasPosition;
        }
        // The smoothed position, pixels.
        const float* position() const {
            return _pos;
        }
        void reset();

      private:
        struct TrailSample {
            float x, y;
            float ageMs;
        };
        struct HeatSpot {
            float x, y, heat;
        };

        float _pos[2] = {0, 0};
        float _velocity[2] = {0, 0};
        float _rawPrev[2] = {0, 0};
        float _tailPos[2] = {0, 0};
        float _dwellAnchor[2] = {0, 0};
        bool _hasPosition = false;
        bool _hasRawPrev = false;
        bool _hasPrevOrientation = false;
        Quat _prevOrientation{0, 0, 0, 1};
        float _dwellMs = 0.f;
        float _dwell = 0.f;
        float _lostMs = 0.f;
        float _alpha = 0.f;
        std::deque<TrailSample> _trail;
        std::deque<HeatSpot> _heat;
    };

} // namespace gaze_mirror
