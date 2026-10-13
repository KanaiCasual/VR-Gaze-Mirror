#include "pch.h"

#include "framing.h"

namespace gaze_mirror {

    namespace {
        float AlphaFor(float cutoffHz, float dt) {
            const float tau = 1.f / (2.f * 3.14159265f * std::max(cutoffHz, 0.001f));
            return 1.f / (1.f + tau / dt);
        }
    } // namespace

    float Framing::MaxTurn(float halfWidth, float halfHeight, float halfRoomX, float halfRoomY) {
        // A rectangle turned by t reaches halfWidth cos t + halfHeight sin t to the side and halfWidth sin t + halfHeight
        // cos t up and down. Both grow until well past 30 degrees, so the first angle that no longer fits is found by halving.
        const auto fits = [&](float t) {
            const float c = cosf(t), s = sinf(t);
            return halfWidth * c + halfHeight * s <= halfRoomX + 0.5f && halfWidth * s + halfHeight * c <= halfRoomY + 0.5f;
        };
        const float top = std::min(atan2f(halfHeight, halfWidth), atan2f(halfWidth, halfHeight));
        if (!fits(0.f)) return 0.f;
        if (fits(top)) return top;
        float lo = 0.f, hi = top;
        for (int i = 0; i < 24; i++) {
            const float mid = 0.5f * (lo + hi);
            (fits(mid) ? lo : hi) = mid;
        }
        return lo;
    }

    float Framing::EaseIntoLimit(float requested, float limit) {
        if (limit <= 0.f) return 0.f;
        const float band = std::min(0.3f * limit, 3.f * 3.14159265f / 180.f);
        const float knee = limit - band, size = fabsf(requested);
        if (size <= knee) return requested;
        const float eased = knee + band * tanhf((size - knee) / band);
        return requested < 0.f ? -eased : eased;
    }

    void Framing::reset() {
        *this = Framing();
    }

    CropRect Framing::placedBox(const Settings& settings, float imageWidth, float imageHeight) {
        CropRect box{0, 0, static_cast<int32_t>(imageWidth), static_cast<int32_t>(imageHeight)};
        if (!settings.cropEnabled || imageWidth < 64 || imageHeight < 64) return box;
        float height = settings.cropHeight * imageHeight;
        float width = settings.cropAspect > 0.f ? height * settings.cropAspect : settings.cropWidth * imageWidth;
        const float fit = std::min({1.f, imageWidth / width, imageHeight / height});
        width *= fit;
        height *= fit;
        box.width = std::clamp(static_cast<int32_t>(width) & ~1, 64, static_cast<int32_t>(imageWidth));
        box.height = std::clamp(static_cast<int32_t>(height) & ~1, 64, static_cast<int32_t>(imageHeight));
        const float left = settings.cropCenterX * imageWidth - box.width * 0.5f;
        const float top = settings.cropCenterY * imageHeight - box.height * 0.5f;
        box.x = static_cast<int32_t>(std::clamp(left, 0.f, imageWidth - box.width));
        box.y = static_cast<int32_t>(std::clamp(top, 0.f, imageHeight - box.height));
        return box;
    }

    void Framing::update(const Settings& settings, const EyeView& view, const CropRect& placed, bool haveGaze, bool hasGazePosition, float gazeY, float dtMs) {
        dtMs = std::clamp(dtMs, 0.f, 100.f);
        updateFollow(settings, view, placed, haveGaze, hasGazePosition, gazeY, dtMs);
        updateStabilizer(settings, view, placed, dtMs);
        // The horizon held level: the box turns against the head's whole sideways tilt, measured against the world's up -
        // from the steadied orientation while steadying runs, else from the head's own. Not part of the steadying's room:
        // only the eye image's edge limits it (below).
        if (settings.levelHorizon && settings.cropEnabled && view.valid && placed.width > 0 && placed.height > 0) {
            const Quat base = settings.stabilize && _stabHasOrientation ? _stabOrientation : Normalize(view.orientation);
            const Quat level = Level(base);
            const bool follow = settings.cropFollowVertical;
            const float cx = placed.x + placed.width * 0.5f + _stabOffset[0];
            const float cy = placed.y + placed.height * 0.5f + (follow ? _followOffset : 0.f) + _stabOffset[1];
            float x = cx, y = cy, rx = cx + 200.f, ry = cy;
            _stabRoll = 0.f;
            if (Reproject(view, level, view.orientation, x, y) && Reproject(view, level, view.orientation, rx, ry) &&
                (fabsf(rx - x) > 1.f || fabsf(ry - y) > 1.f)) {
                _stabRoll = atan2f(ry - y, rx - x);
            }
        }
        const bool follows = settings.cropEnabled && settings.cropFollowVertical;
        const float top = static_cast<float>(placed.y) + (follows ? _followOffset : 0.f) + _stabOffset[1];
        const float left = static_cast<float>(placed.x) + _stabOffset[0];
        _current = placed;
        _current.x = static_cast<int32_t>(std::clamp(left + 0.5f, 0.f, std::max(view.width - placed.width, 0.f)));
        _current.y = static_cast<int32_t>(std::clamp(top + 0.5f, 0.f, std::max(view.height - placed.height, 0.f)));
        // A turned box reaches further than a level one: its corners move out by half its height (times the sine of the
        // angle) to the sides and by half its width up and down. Whatever the steadying asks for, the box as it stands
        // this frame must stay inside the eye image - nothing beyond it may be sampled (the other eye can be next door).
        // The room is the eye image, or the measured view edges (less a small margin) in the eye they were measured in -
        // never less than the box itself already covers. Near the limit the turn eases off; beyond it the picture tilts
        // with the head rather than show what lies past the view.
        _roll = 0.f;
        if (_stabRoll != 0.f && placed.width > 0 && placed.height > 0) {
            float left = 0.f, right = view.width, top = 0.f, bottom = view.height;
            if (settings.hasViewEdges && settings.viewEdgesEye == settings.eye) {
                left = settings.viewEdges[0] * view.width + ViewEdgeMargin * view.width;
                right = settings.viewEdges[1] * view.width - ViewEdgeMargin * view.width;
                top = settings.viewEdges[2] * view.height + ViewEdgeMargin * view.height;
                bottom = settings.viewEdges[3] * view.height - ViewEdgeMargin * view.height;
            }
            left = std::min(left, static_cast<float>(_current.x));
            right = std::max(right, static_cast<float>(_current.x + placed.width));
            top = std::min(top, static_cast<float>(_current.y));
            bottom = std::max(bottom, static_cast<float>(_current.y + placed.height));
            const float cx = _current.x + placed.width * 0.5f, cy = _current.y + placed.height * 0.5f;
            const float limit = MaxTurn(placed.width * 0.5f, placed.height * 0.5f, std::min(cx - left, right - cx), std::min(cy - top, bottom - cy));
            _roll = EaseIntoLimit(_stabRoll, limit);
        }
    }

    // Like a camera operator: while the gaze stays in the middle of the frame nothing moves; near the top or bottom
    // the box glides just far enough, stays there, and drifts back home once the gaze has rested for a while.
    void Framing::updateFollow(const Settings& settings, const EyeView& view, const CropRect& placed, bool haveGaze, bool hasGazePosition, float gazeY, float dtMs) {
        const bool follows = settings.cropEnabled && settings.cropFollowVertical && placed.height > 0 && placed.height < static_cast<int32_t>(view.height);
        if (!follows) {
            _followOffset = _followTarget = _followStillMs = 0.f;
            return;
        }
        const float boxHeight = static_cast<float>(placed.height);
        const float home = static_cast<float>(placed.y);
        if (haveGaze && hasGazePosition) {
            const float zoneTop = (1.f - settings.cropFollowDeadzone) * 0.5f * boxHeight;
            const float zoneBottom = (1.f + settings.cropFollowDeadzone) * 0.5f * boxHeight;
            const float allowedMin = gazeY - home - zoneBottom;
            const float allowedMax = gazeY - home - zoneTop;
            const float pushed = std::clamp(_followTarget, allowedMin, allowedMax);
            if (pushed != _followTarget) {
                _followTarget = pushed;
                _followStillMs = 0.f;
            } else {
                _followStillMs += dtMs;
                if (settings.cropFollowReturn && _followStillMs >= settings.cropFollowHomeMs) {
                    _followTarget = std::clamp(0.f, allowedMin, allowedMax);
                }
            }
        }
        const float reach = settings.cropFollowReach * boxHeight;
        _followTarget = std::clamp(_followTarget, -reach, reach);
        // With steadying on, the travel stops short of the image's edges by the steadying's room.
        const float keep = settings.stabilize ? settings.stabilizeRoom * view.height : 0.f;
        const float highest = std::min(-home + keep, 0.f);
        const float lowest = std::max(view.height - placed.height - home - keep, 0.f);
        _followTarget = std::clamp(_followTarget, highest, lowest);
        const float k = settings.cropFollowGlideMs > 0.f ? 1.f - expf(-dtMs / settings.cropFollowGlideMs) : 1.f;
        _followOffset += (_followTarget - _followOffset) * k;
    }

    void Framing::updateStabilizer(const Settings& settings, const EyeView& view, const CropRect& placed, float dtMs) {
        const bool roomToMove = placed.width < static_cast<int32_t>(view.width) || placed.height < static_cast<int32_t>(view.height);
        if (!settings.stabilize || !settings.cropEnabled || !roomToMove || !view.valid) {
            _stabHasOrientation = false;
            _stabOffset[0] = _stabOffset[1] = 0.f;
            _stabRoll = 0.f;
            return;
        }
        const float dt = dtMs / 1000.f;
        const Quat raw = Normalize(view.orientation);
        Quat smoothed = _stabOrientation;
        if (!_stabHasOrientation || dt <= 0.f || dt > 0.25f || AngleBetween(smoothed, raw) > 0.5f) {
            smoothed = raw;
            _stabSpeed = 0.f;
            _stabHasOrientation = true;
        } else {
            const float rawSpeed = AngleBetween(_stabRawPrev, raw) / dt;
            _stabSpeed += (rawSpeed - _stabSpeed) * AlphaFor(1.f, dt);
            smoothed = Slerp(smoothed, raw, AlphaFor(settings.stabilizeMinCutoff + settings.stabilizeBeta * _stabSpeed, dt));
        }
        _stabOrientation = smoothed;
        _stabRawPrev = raw;

        // Where the middle of the box, as seen from the smoothed orientation, is in the image we actually have.
        const float centreX = placed.x + placed.width * 0.5f;
        const float centreY = placed.y + placed.height * 0.5f + _followOffset;
        float x = centreX, y = centreY;
        if (!Reproject(view, _stabOrientation, view.orientation, x, y)) return;
        const float roomX = std::max(settings.stabilizeRoom * view.width, 1.f);
        const float roomY = std::max(settings.stabilizeRoom * view.height, 1.f);
        float offsetX = x - centreX, offsetY = y - centreY;
        // The tilt: where "to the right" at the box's middle, as seen from the smoothed orientation, points in the image
        // we have. A box turned by that angle about its middle shows the picture level while the head tilts.
        float roll = 0.f;
        if (settings.stabilizeRoll) {
            float rightX = centreX + 200.f, rightY = centreY;
            if (Reproject(view, _stabOrientation, view.orientation, rightX, rightY) && (fabsf(rightX - x) > 1.f || fabsf(rightY - y) > 1.f)) {
                roll = atan2f(rightY - y, rightX - x);
            }
        }
        // The turn shares the room with the movement: a turned box's corners reach further out.
        const float reachX = 0.5f * placed.height * fabsf(sinf(roll)), reachY = 0.5f * placed.width * fabsf(sinf(roll));
        const float over = std::max((fabsf(offsetX) + reachX) / roomX, (fabsf(offsetY) + reachY) / roomY);
        if (over > 1.f) {
            // The smoothed view fell further behind than the room: pull it along to the edge of the room, so the
            // picture turns with the head and eases to rest the moment the head stops.
            _stabOrientation = Slerp(smoothed, raw, 1.f - 1.f / over);
            offsetX /= over;
            offsetY /= over;
            roll /= over;
        }
        _stabOffset[0] = std::clamp(offsetX, -roomX, roomX);
        _stabOffset[1] = std::clamp(offsetY, -roomY, roomY);
        _stabRoll = roll;
        _stabLogLargest = std::max({_stabLogLargest, fabsf(_stabOffset[0]), fabsf(_stabOffset[1])});
        _stabLogLargestRoll = std::max(_stabLogLargestRoll, fabsf(roll));
        if (++_stabLogFrames >= 2700) {
            Log("steadying: on (%.2f Hz, follow %.1f, tilt %s); largest counter-move lately %.0f px of %.0f px room, largest turn %.1f degrees",
                settings.stabilizeMinCutoff, settings.stabilizeBeta, settings.stabilizeRoll ? "on" : "off", _stabLogLargest, std::min(roomX, roomY),
                _stabLogLargestRoll * 57.29578f);
            _stabLogFrames = 0;
            _stabLogLargest = 0.f;
            _stabLogLargestRoll = 0.f;
        }
    }

} // namespace gaze_mirror
