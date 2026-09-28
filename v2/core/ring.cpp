#include "pch.h"

#include "ring.h"

namespace gaze_mirror {

    namespace {
        float AlphaFor(float cutoffHz, float dt) {
            const float tau = 1.f / (2.f * 3.14159265f * std::max(cutoffHz, 0.001f));
            return 1.f / (1.f + tau / dt);
        }
    } // namespace

    void RingState::reset() {
        *this = RingState();
    }

    void RingState::update(const Settings& settings, const EyeView& view, bool haveTarget, const float target[2], float dtMs) {
        dtMs = std::clamp(dtMs, 0.f, 100.f);
        const float height = std::max(view.height, 1.f);

        // Anchor everything in the world: when the head turns, carry the remembered positions along with the scene.
        bool snapTail = false;
        if (view.valid) {
            if (settings.worldAnchored && _hasPosition && _hasPrevOrientation) {
                if (!Reproject(view, _prevOrientation, view.orientation, _pos[0], _pos[1]) ||
                    !Reproject(view, _prevOrientation, view.orientation, _tailPos[0], _tailPos[1])) {
                    snapTail = true;
                }
                if (_hasRawPrev && !Reproject(view, _prevOrientation, view.orientation, _rawPrev[0], _rawPrev[1])) _hasRawPrev = false;
                if (!Reproject(view, _prevOrientation, view.orientation, _dwellAnchor[0], _dwellAnchor[1])) _dwellMs = 0.f;
                for (auto it = _trail.begin(); it != _trail.end();) {
                    it = Reproject(view, _prevOrientation, view.orientation, it->x, it->y) ? std::next(it) : _trail.erase(it);
                }
                for (auto it = _heat.begin(); it != _heat.end();) {
                    it = Reproject(view, _prevOrientation, view.orientation, it->x, it->y) ? std::next(it) : _heat.erase(it);
                }
            }
            _prevOrientation = view.orientation;
            _hasPrevOrientation = true;
        }

        // Smooth the position.
        if (haveTarget) {
            if (!_hasPosition || _alpha < 0.01f) snapTail = true;
            if (snapTail || (!settings.adaptiveFilter && settings.smoothingMs <= 0.f) || dtMs <= 0.f) {
                _pos[0] = target[0];
                _pos[1] = target[1];
                _velocity[0] = _velocity[1] = 0.f;
            } else if (settings.adaptiveFilter) {
                // One-Euro: smooth hard while the eye is still, barely at all while it moves fast. Speeds in image
                // heights per second so the tuning does not depend on the resolution.
                const float dt = dtMs / 1000.f;
                float rawVelocity[2] = {0.f, 0.f};
                if (_hasRawPrev) {
                    rawVelocity[0] = (target[0] - _rawPrev[0]) / (dt * height);
                    rawVelocity[1] = (target[1] - _rawPrev[1]) / (dt * height);
                }
                const float velocityAlpha = AlphaFor(3.f, dt);
                _velocity[0] += (rawVelocity[0] - _velocity[0]) * velocityAlpha;
                _velocity[1] += (rawVelocity[1] - _velocity[1]) * velocityAlpha;
                const float speed = sqrtf(_velocity[0] * _velocity[0] + _velocity[1] * _velocity[1]);
                const float k = AlphaFor(settings.filterMinCutoff + settings.filterBeta * speed, dt);
                _pos[0] += (target[0] - _pos[0]) * k;
                _pos[1] += (target[1] - _pos[1]) * k;
            } else {
                const float k = 1.f - expf(-dtMs / settings.smoothingMs);
                _pos[0] += (target[0] - _pos[0]) * k;
                _pos[1] += (target[1] - _pos[1]) * k;
            }
            _rawPrev[0] = target[0];
            _rawPrev[1] = target[1];
            _hasRawPrev = true;
            _hasPosition = true;
        }

        if (_hasPosition) {
            // The tip of the ghost tail chases the ring: stretches during eye movements, collapses during fixations.
            if (snapTail || settings.trailMs <= 0.f) {
                _tailPos[0] = _pos[0];
                _tailPos[1] = _pos[1];
            } else {
                const float k = 1.f - expf(-dtMs / settings.trailMs);
                _tailPos[0] += (_pos[0] - _tailPos[0]) * k;
                _tailPos[1] += (_pos[1] - _tailPos[1]) * k;
            }
            // Dwell: holding the gaze in a small area tightens the ring. A blink pauses the timer, not resets it.
            const float tolerance = settings.radius * 0.6f * height;
            const float dx = _pos[0] - _dwellAnchor[0], dy = _pos[1] - _dwellAnchor[1];
            if (snapTail || settings.dwellMs <= 0.f || dx * dx + dy * dy > tolerance * tolerance) {
                _dwellAnchor[0] = _pos[0];
                _dwellAnchor[1] = _pos[1];
                _dwellMs = 0.f;
            } else if (haveTarget) {
                _dwellMs += dtMs;
            }
            const float dwellTarget = (settings.dwellMs > 0.f && _dwellMs >= settings.dwellMs) ? 1.f : 0.f;
            _dwell += (dwellTarget - _dwell) * (1.f - expf(-dtMs / 140.f));
        }

        // Ride through short tracking losses (blinks) at the last position, then fade out.
        _lostMs = haveTarget ? 0.f : _lostMs + dtMs;
        const float alphaTarget = (_hasPosition && _lostMs <= settings.holdMs) ? 1.f : 0.f;
        const float fade = settings.fadeMs > 0.f ? 1.f - expf(-dtMs / settings.fadeMs) : 1.f;
        _alpha += (alphaTarget - _alpha) * fade;

        // Trail (bubble/solid): a sample every so often, dropped when older than the trail.
        const bool useTrail = (settings.style == Style::Bubble || settings.style == Style::Solid) && settings.blobTrailMs > 0.f;
        const bool useHeat = settings.style == Style::Heatmap;
        if (useTrail && _hasPosition) {
            for (TrailSample& sample : _trail) sample.ageMs += dtMs;
            const float interval = settings.blobTrailMs / (MaxTrail - 1);
            if (_trail.empty() || _trail.back().ageMs >= interval) _trail.push_back({_pos[0], _pos[1], 0.f});
            while (!_trail.empty() && (_trail.size() > MaxTrail - 1 || _trail.front().ageMs > settings.blobTrailMs)) _trail.pop_front();
        } else {
            _trail.clear();
        }

        // Heatmap: the spot under the gaze warms up; looking away leaves it cooling where it was.
        if (useHeat && haveTarget && _hasPosition) {
            const float joinRadius = 0.5f * settings.radius * height;
            size_t current = _heat.size();
            float best = joinRadius * joinRadius;
            for (size_t i = 0; i < _heat.size(); i++) {
                const float hx = _heat[i].x - _pos[0], hy = _heat[i].y - _pos[1];
                if (hx * hx + hy * hy <= best) {
                    best = hx * hx + hy * hy;
                    current = i;
                }
            }
            if (current == _heat.size()) {
                _heat.push_back({_pos[0], _pos[1], 0.f});
            } else if (current + 1 != _heat.size()) {
                const HeatSpot spot = _heat[current];
                _heat.erase(_heat.begin() + current);
                _heat.push_back(spot);
            }
            for (size_t i = 0; i + 1 < _heat.size(); i++) _heat[i].heat -= dtMs / settings.heatCoolMs;
            HeatSpot& looking = _heat.back();
            looking.heat = std::min(1.f, looking.heat + dtMs / settings.heatMs);
            const float settle = 1.f - expf(-dtMs / 300.f);
            looking.x += (_pos[0] - looking.x) * settle;
            looking.y += (_pos[1] - looking.y) * settle;
            for (size_t i = 0; i + 1 < _heat.size();) {
                if (_heat[i].heat <= 0.f) _heat.erase(_heat.begin() + i);
                else i++;
            }
            while (_heat.size() > MaxTrail) _heat.pop_front();
        } else if (useHeat) {
            for (auto it = _heat.begin(); it != _heat.end();) {
                it->heat -= dtMs / settings.heatCoolMs;
                it = it->heat <= 0.f ? _heat.erase(it) : std::next(it);
            }
        } else {
            _heat.clear();
        }
    }

    bool RingState::visible(const Settings& settings) const {
        return settings.enabled && _hasPosition && _alpha >= 0.004f;
    }

    void RingState::constants(const Settings& settings, const EyeView& view, RingConstants& out) const {
        const float width = std::max(view.width, 1.f), height = std::max(view.height, 1.f);
        out = {};
        const bool useHeat = settings.style == Style::Heatmap;
        size_t count = 0;
        if (useHeat && !_heat.empty()) {
            for (size_t i = 0; i < _heat.size() && count < MaxTrail; i++) {
                const bool looking = _lostMs == 0.f && i + 1 == _heat.size();
                out.trail[count][0] = _heat[i].x / width;
                out.trail[count][1] = _heat[i].y / height;
                out.trail[count][2] = looking ? std::max(_heat[i].heat, 0.15f) : _heat[i].heat;
                count++;
            }
        } else {
            out.trail[count][0] = _pos[0] / width;
            out.trail[count][1] = _pos[1] / height;
            out.trail[count][2] = useHeat ? 0.15f : 1.f;
            count++;
        }
        for (auto it = _trail.rbegin(); it != _trail.rend() && count < MaxTrail; ++it) {
            out.trail[count][0] = it->x / width;
            out.trail[count][1] = it->y / height;
            out.trail[count][2] = std::clamp(1.f - it->ageMs / settings.blobTrailMs, 0.f, 1.f);
            count++;
        }
        if (settings.style == Style::Ghost && count < MaxTrail) {
            float tail[2] = {_tailPos[0] - _pos[0], _tailPos[1] - _pos[1]};
            const float length = sqrtf(tail[0] * tail[0] + tail[1] * tail[1]);
            const float maxLength = settings.tailMax * height;
            const float scale = (length > maxLength && length > 0.f) ? maxLength / length : 1.f;
            out.trail[count][0] = (_pos[0] + tail[0] * scale) / width;
            out.trail[count][1] = (_pos[1] + tail[1] * scale) / height;
            out.trail[count][2] = 1.f;
            count++;
        }
        out.trailCount = static_cast<float>(count);
        out.center[0] = _pos[0] / width;
        out.center[1] = _pos[1] / height;
        out.aspect[0] = width / height;
        out.aspect[1] = 1.f;
        out.radius = settings.radius * (1.f - settings.dwellShrink * _dwell);
        out.stretch = 1.f;
        out.thickness = settings.thickness;
        out.feather = settings.feather;
        out.fillOpacity = settings.fillOpacity * _alpha;
        out.style = static_cast<float>(settings.style);
        out.shadowOpacity = settings.shadowOpacity * _alpha;
        out.glowWidth = settings.glowWidth;
        out.tailOpacity = settings.tailOpacity;
        out.premultiply = (settings.screenBlend && settings.style != Style::Spotlight) ? 1.f : 0.f;
        out.solidity = settings.solidity;
        out.glowStrength = settings.glowStrength;
        out.heatGain = 1.f;
        for (int i = 0; i < 3; i++) out.color[i] = settings.color[i];
        out.color[3] = std::min(settings.opacity * (1.f + 0.3f * _dwell), 1.f) * _alpha;
    }

} // namespace gaze_mirror
