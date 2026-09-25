#include "pch.h"

#include "log.h"
#include "pipeline.h"

namespace gaze_mirror {

    bool Pipeline::start(ID3D11Device* device, LONG producerKind, const char* program, const char* application) {
        stop();
        _settings.open();
        _settings.setGame(program, application);
        if (!_renderer.start(device)) return false;
        if (!_publisher.start(device, producerKind, program, application)) {
            _renderer.stop();
            return false;
        }
        _ring.reset();
        _framing.reset();
        _lastFrame = {};
        _nextDue = {};
        return true;
    }

    void Pipeline::stop() {
        _publisher.stop();
        _renderer.stop();
    }

    bool Pipeline::wanted() {
        _settings.refreshIfSignalled();
        if (_settings.get().headsetMarker || _calibrationDirty) nudgeKeys();
        if (_snapshot.wanted(_settings.signal())) _snapshotDue = true;
        _renderDue = _publisher.started() && _publisher.wanted();
        // The frame-rate cap: a schedule, so that 90 -> 60 means two of every three frames, not a stutter. A frame that
        // arrives a hair early still counts (2 ms), or a game running right at the cap would lose every other frame.
        const float fps = _settings.get().outputFps;
        if (_renderDue && fps > 0.f) {
            const auto now = std::chrono::steady_clock::now();
            const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
            const bool scheduled = _nextDue.time_since_epoch().count() != 0;
            if (scheduled && now + std::chrono::milliseconds(2) < _nextDue) {
                _renderDue = false;
            } else {
                _nextDue = (!scheduled || now - _nextDue > interval) ? now + interval : _nextDue + interval;
            }
        }
        return _renderDue || _snapshotDue || _settings.get().headsetMarker; // The marker needs no reader.
    }

    // Where the gaze lands in an eye's image, in pixels, with the user's trim.
    bool Pipeline::target(const FrameInput& input, int eye, float out[2]) const {
        const Settings& s = _settings.get();
        const EyeView& view = input.views[eye];
        if (!input.gaze.valid || !view.valid) return false;
        float x = 0, y = 0;
        if (input.gaze.hasPerEyeUv) {
            x = input.gaze.uv[eye][0] * view.width;
            y = input.gaze.uv[eye][1] * view.height;
        } else if (input.gaze.hasRay) {
            // The ray starts between the eyes; the image shows one eye half an IPD to the side, so where the ray
            // lands depends on how far away the fixated object is. focus_distance 0 = far: only the direction counts.
            const EyePose& pose = input.eyeInHead[eye];
            Vec3 fromEye = input.gaze.direction;
            if (s.focusDistance > 0.f) {
                fromEye = {input.gaze.direction.x * s.focusDistance + input.gaze.origin.x - pose.position.x,
                           input.gaze.direction.y * s.focusDistance + input.gaze.origin.y - pose.position.y,
                           input.gaze.direction.z * s.focusDistance + input.gaze.origin.z - pose.position.z};
            }
            const Vec3 inEye = InverseRotate(Normalize(pose.orientation), fromEye);
            if (!ProjectDirection(view, inEye, x, y)) return false;
        } else {
            return false;
        }
        out[0] = x + s.offsetX * view.height;
        out[1] = y - s.offsetY * view.height;
        return true;
    }

    float Pipeline::MapValue(const std::vector<std::pair<float, float>>& map, float value) {
        if (map.size() < 2) return value;
        size_t i = 1;
        while (i + 1 < map.size() && value > map[i].first) i++;
        const auto& [x0, y0] = map[i - 1];
        const auto& [x1, y1] = map[i];
        const float span = x1 - x0;
        if (std::fabs(span) < 1e-6f) return y0;
        return y0 + (value - x0) * (y1 - y0) / span;
    }

    // The axis maps come from looks along the axes. Towards a corner each axis reads a little differently (the tracker
    // gives components of a direction, not angles), so the diagonal targets measured a gain per axis at each corner;
    // it is applied in proportion to how far the OTHER axis is from the centre, up to a little beyond the corner.
    void Pipeline::CornerCorrect(const std::vector<Settings::CornerGain>& corners, float& x, float& y) {
        const Settings::CornerGain& c = corners[(x < 0.f ? 1 : 0) + (y < 0.f ? 2 : 0)];
        const float towardsY = std::min(std::fabs(y) / std::max(std::fabs(c.ty), 1e-3f), 1.25f);
        const float towardsX = std::min(std::fabs(x) / std::max(std::fabs(c.tx), 1e-3f), 1.25f);
        x *= 1.f + (c.gainX - 1.f) * towardsY;
        y *= 1.f + (c.gainY - 1.f) * towardsX;
    }

    GazeFrame Pipeline::chooseGaze(const FrameInput& input) {
        const Settings& s = _settings.get();
        // gaze_source: 0 the headset, else SRanibro's raw gaze, else VRChat; 1 headset only; 2 VRChat only; 3 SRanibro only.
        if (s.gazeSource == 1 || (s.gazeSource == 0 && input.gaze.valid)) return input.gaze;
        GazeFrame gaze; // Nothing, unless something outside has a fresh sample.
        if (s.gazeSource == 0 || s.gazeSource == 3) {
            const ExternalGazeSample raw = _raw.read();
            if (raw.valid) {
                // Both eyes shut: a blink. The ring's own hold/fade handles a short gap.
                if (raw.leftOpenness < 0.15f && raw.rightOpenness < 0.15f) return gaze;
                // The valid eyes' unit directions, averaged: head space already, x right, y up, forward -z.
                Vec3 d{0, 0, 0};
                if (raw.leftValid) d = {d.x + raw.left[0], d.y + raw.left[1], d.z + raw.left[2]};
                if (raw.rightValid) d = {d.x + raw.right[0], d.y + raw.right[1], d.z + raw.right[2]};
                const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (length > 1e-4f) {
                    gaze.valid = true;
                    gaze.hasRay = true;
                    gaze.direction = {d.x / length, d.y / length, d.z / length};
                    gaze.origin = {0.5f * (input.eyeInHead[0].position.x + input.eyeInHead[1].position.x),
                                   0.5f * (input.eyeInHead[0].position.y + input.eyeInHead[1].position.y),
                                   0.5f * (input.eyeInHead[0].position.z + input.eyeInHead[1].position.z)};
                    LogFewTimes(_logRaw, 2, "pipeline: gaze from %s (raw direction, no calibration)", raw.writer);
                    return gaze;
                }
            }
            if (s.gazeSource == 3) return gaze;
        }
        const ExternalGazeSample ext = _external.read();
        if (!ext.valid) return s.gazeSource == 2 ? gaze : input.gaze;
        // Both eyes shut: a blink. The ring's own hold/fade handles a short gap.
        if (ext.leftOpenness < 0.15f && ext.rightOpenness < 0.15f) return gaze;
        // The two eyes' pairs, averaged and scaled, as a direction in head space (x right, y up, looking down -z).
        const float rawX = 0.5f * (ext.left[0] + ext.right[0]);
        const float rawY = 0.5f * (ext.left[1] + ext.right[1]);
        const bool calibrated = s.vrchatMapX.size() >= 2 && s.vrchatMapY.size() >= 2;
        float x = calibrated ? MapValue(s.vrchatMapX, rawX) : rawX * s.vrchatScale;
        float y = calibrated ? MapValue(s.vrchatMapY, rawY) : rawY * (rawY > 0.f ? s.vrchatScaleUp : s.vrchatScaleDown);
        if (calibrated && s.vrchatCorners.size() == 4) CornerCorrect(s.vrchatCorners, x, y);
        const float length = std::sqrt(x * x + y * y + 1.f);
        gaze.valid = true;
        gaze.hasRay = true;
        gaze.direction = {x / length, y / length, -1.f / length};
        gaze.origin = {0.5f * (input.eyeInHead[0].position.x + input.eyeInHead[1].position.x),
                       0.5f * (input.eyeInHead[0].position.y + input.eyeInHead[1].position.y),
                       0.5f * (input.eyeInHead[0].position.z + input.eyeInHead[1].position.z)};
        if (calibrated) LogFewTimes(_logExternal, 2, "pipeline: gaze from %s (calibrated: %zu x %zu points%s)", ext.writer, s.vrchatMapX.size(), s.vrchatMapY.size(), s.vrchatCorners.size() == 4 ? ", corners" : "");
        else LogFewTimes(_logExternal, 2, "pipeline: gaze from %s (scale %.2f sideways, %.2f up, %.2f down)", ext.writer, s.vrchatScale, s.vrchatScaleUp, s.vrchatScaleDown);
        return gaze;
    }

    FrameOutput Pipeline::frame(const FrameInput& given) {
        FrameOutput output;
        const Settings& s = _settings.get();
        FrameInput input = given;
        input.gaze = chooseGaze(given);
        const int eye = s.eye == 0 ? 0 : 1;
        const SourceImage* image = input.images[eye];
        const EyeView& view = input.views[eye];
        if (!image || !view.valid || image->width < 64 || image->height < 64) return output;

        const auto now = std::chrono::steady_clock::now();
        float dtMs = 0.f;
        if (_lastFrame.time_since_epoch().count() != 0) dtMs = std::chrono::duration<float, std::milli>(now - _lastFrame).count();
        _lastFrame = now;

        float gazeTarget[2] = {0, 0};
        const bool haveTarget = (s.enabled || (s.cropEnabled && s.cropFollowVertical)) && target(input, eye, gazeTarget);
        LogFewTimes(_logGaze, 3, "pipeline: gaze %s -> (%.0f, %.0f) of %.0f x %.0f", haveTarget ? "yes" : "no", gazeTarget[0], gazeTarget[1], view.width, view.height);
        _ring.update(s, view, haveTarget, gazeTarget, dtMs);

        const CropRect placed = Framing::placedBox(s, view.width, view.height);
        _framing.update(s, view, placed, haveTarget, _ring.hasPosition(), _ring.position()[1], dtMs);
        const CropRect crop = _framing.current();

        // The calibration marker: where the ring is, in each eye, as the headset should show it.
        if (s.headsetMarker && input.gaze.valid) {
            output.marker = true;
            for (int e = 0; e < 2; e++) {
                float point[2];
                const EyeView& v = input.views[e];
                if (v.valid && target(input, e, point)) {
                    output.markerNdc[e][0] = point[0] / v.width * 2.f - 1.f;
                    output.markerNdc[e][1] = 1.f - point[1] / v.height * 2.f;
                } else {
                    output.marker = false;
                }
            }
            output.markerRadius = s.radius;
            for (int i = 0; i < 3; i++) output.markerColor[i] = s.color[i];
        }

        // The crop tool's picture, when asked for.
        if (_snapshotDue) {
            _snapshotDue = false;
            _snapshot.take(_renderer, input.images, eye, placed);
        }
        if (!_renderDue) return output;
        _renderDue = false;

        // The published size: the crop's own, unless that is more than anybody records.
        const uint32_t maxSide = s.outputMaxSide >= 64 ? s.outputMaxSide : 16384;
        uint32_t outWidth = static_cast<uint32_t>(crop.width), outHeight = static_cast<uint32_t>(crop.height);
        const uint32_t longest = std::max(outWidth, outHeight);
        if (longest > maxSide) {
            outWidth = std::max(64u, (outWidth * maxSide / longest) & ~1u);
            outHeight = std::max(64u, (outHeight * maxSide / longest) & ~1u);
        }
        RingConstants ring;
        const bool drawRing = _ring.visible(s);
        if (drawRing) _ring.constants(s, view, ring);
        output.rendered = _renderer.render(_publisher, *image, crop, outWidth, outHeight, drawRing ? &ring : nullptr, eye);
        return output;
    }

    // Ctrl+Alt+arrows nudge the trim while the calibration marker is on (only then is the keyboard looked at); the
    // nudge is written back to the settings file shortly after the last press.
    void Pipeline::nudgeKeys() {
        Settings& s = _settings.mutableSettings();
        const auto isDown = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        const bool chord = isDown(VK_CONTROL) && isDown(VK_MENU);
        const ULONGLONG nowMs = GetTickCount64();
        const int dx = (isDown(VK_RIGHT) ? 1 : 0) - (isDown(VK_LEFT) ? 1 : 0);
        const int dy = (isDown(VK_UP) ? 1 : 0) - (isDown(VK_DOWN) ? 1 : 0);
        if (s.headsetMarker && chord && (dx != 0 || dy != 0)) {
            bool fire = false;
            if (!_nudgeWasDown) {
                fire = true;
                _nudgeNextRepeatMs = nowMs + 400;
            } else if (nowMs >= _nudgeNextRepeatMs) {
                fire = true;
                _nudgeNextRepeatMs = nowMs + 50;
            }
            if (fire) {
                const float step = 0.0005f * (isDown(VK_SHIFT) ? 5.f : 1.f);
                s.offsetX = std::clamp(s.offsetX + dx * step, -0.2f, 0.2f);
                s.offsetY = std::clamp(s.offsetY + dy * step, -0.2f, 0.2f);
                _calibrationDirty = true;
                _calibrationDirtySinceMs = nowMs;
            }
            _nudgeWasDown = true;
        } else {
            _nudgeWasDown = false;
        }
        if (_calibrationDirty && nowMs - _calibrationDirtySinceMs > 700) {
            _calibrationDirty = false;
            char x[32], y[32];
            sprintf_s(x, "%.4f", s.offsetX);
            sprintf_s(y, "%.4f", s.offsetY);
            _settings.persist({{"offset_x", x}, {"offset_y", y}});
            Log("calibration: trim saved (%s, %s)", x, y);
        }
    }

} // namespace gaze_mirror
