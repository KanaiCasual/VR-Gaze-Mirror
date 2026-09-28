#ifndef NOMINMAX
#define NOMINMAX // windows.h (through the core's headers) must not turn std::min into a macro.
#endif
#include "calibration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <openvr.h>

#include "../core/gaze_grid.h"
#include "../core/log.h"
#include "vrchat_osc.h"

namespace gaze_mirror {

    namespace {

        constexpr float Pi = 3.14159265f;
        constexpr float TargetDistance = 2.f; // Metres in front of the head.
        // How far the targets go at most (degrees): inside every mainstream headset's edges and where trackers are
        // still accurate. Up is the tightest direction everywhere, down the most generous.
        constexpr float CapSideDeg = 35.f, CapUpDeg = 28.f, CapDownDeg = 35.f;
        constexpr float EdgeShare = 0.9f; // ...and never past this much of the narrower eye's edge.

        // Quick mode: the eyes travel and settle before sampling starts, then this long is sampled.
        constexpr int SettleMs = 800;
        constexpr int SampleMs = 1400;

        // Thorough mode: the target's speed, its rest on each node, the settle at the start, the recording step, and
        // how much delay between target and eyes is looked for.
        constexpr float SpeedDegPerS = 12.f;
        constexpr float PauseMs = 600.f;
        constexpr float StartSettleMs = 1500.f;
        constexpr float StepMs = 10.f;
        constexpr int MaxLagMs = 400;
        constexpr int BlinkHoldMs = 150; // After a blink, this long is not trusted either.

        float Median(std::vector<float> values) {
            if (values.empty()) return 0.f;
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        // The target: a white ring with a cyan dot, on nothing.
        std::vector<uint8_t> TargetPixels(int size) {
            std::vector<uint8_t> pixels(size_t(size) * size * 4, 0);
            const float c = (size - 1) / 2.f;
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    const float r = std::sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c; // 0 centre .. 1 edge
                    float alpha = 0.f;
                    uint8_t red = 255, green = 255, blue = 255;
                    if (r < 0.22f) {
                        alpha = 1.f;
                        red = 40; green = 200; blue = 255;
                    } else if (r > 0.78f && r < 0.98f) {
                        alpha = std::min(1.f, std::min(r - 0.78f, 0.98f - r) * 12.f);
                    }
                    uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
                    p[0] = red; p[1] = green; p[2] = blue; p[3] = uint8_t(alpha * 255.f);
                }
            }
            return pixels;
        }

        std::string MapText(const std::vector<std::pair<float, float>>& map) {
            std::string text;
            char item[64];
            for (const auto& [raw, tangent] : map) {
                snprintf(item, sizeof(item), "%s%.4f:%.4f", text.empty() ? "" : ",", raw, tangent);
                text += item;
            }
            return text;
        }

        std::string Now() {
            const std::time_t t = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &t);
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
            return text;
        }

        // The target at these tangents (right, up), head-locked, two metres out.
        void Place(vr::IVROverlay* overlay, vr::VROverlayHandle_t handle, float tanYaw, float tanPitch) {
            // Head-locked: x right, y up, z towards the viewer (so in front is -z).
            vr::HmdMatrix34_t m = {{{1, 0, 0, TargetDistance * tanYaw}, {0, 1, 0, TargetDistance * tanPitch}, {0, 0, 1, -TargetDistance}}};
            overlay->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &m);
        }

        std::string ReachText(float left, float right, float up, float down) {
            char text[96];
            if (std::fabs(left - right) < 0.5f) snprintf(text, sizeof(text), "reach %.0f sideways, %.0f up, %.0f down", right, up, down);
            else snprintf(text, sizeof(text), "reach %.0f left, %.0f right, %.0f up, %.0f down", left, right, up, down);
            return text;
        }

    } // namespace

    GazeCalibration::~GazeCalibration() {
        if (_thread.joinable()) _thread.join();
    }

    void GazeCalibration::start(VrchatOscLink& link, Pipeline& pipeline, int mode, const EyeView views[2], const EyePose poses[2]) {
        if (_running) return;
        if (_thread.joinable()) _thread.join();
        _running = true;
        _mode = mode == 2 ? 2 : 1;
        for (int e = 0; e < 2; e++) {
            _views[e] = views[e];
            _poses[e] = poses[e];
        }
        _thread = std::thread([this, &link, &pipeline] { run(&link, &pipeline); });
    }

    // How far a target can go in each direction before it leaves the narrower eye's view (with a margin), checked the
    // same way the ring is placed: the direction in head space, turned into each eye, against that eye's edges.
    GazeCalibration::Reach GazeCalibration::reach() const {
        const auto furthest = [&](float dirX, float dirY, float capDeg) {
            if (!_views[0].valid || !_views[1].valid) return capDeg;
            for (float deg = capDeg; deg > 5.f; deg -= 1.f) {
                const float t = std::tan(deg * Pi / 180.f);
                const Vec3 d{dirX * t, dirY * t, -1.f};
                bool visible = true;
                for (int e = 0; e < 2 && visible; e++) {
                    const Vec3 inEye = InverseRotate(Normalize(_poses[e].orientation), d);
                    if (-inEye.z < 1e-3f) {
                        visible = false;
                        break;
                    }
                    const float x = inEye.x / -inEye.z, y = inEye.y / -inEye.z;
                    const FovTan& f = _views[e].fov;
                    visible = x >= EdgeShare * f.left && x <= EdgeShare * f.right && y >= EdgeShare * f.down && y <= EdgeShare * f.up;
                }
                if (visible) return deg;
            }
            return 5.f;
        };
        return {furthest(-1.f, 0.f, CapSideDeg), furthest(1.f, 0.f, CapSideDeg), furthest(0.f, 1.f, CapUpDeg), furthest(0.f, -1.f, CapDownDeg)};
    }

    void GazeCalibration::run(VrchatOscLink* link, Pipeline* pipeline) {
        Log("calibration: starting (%s)", _mode == 2 ? "thorough, moving target" : "quick, twenty targets");
        // Whatever happens, the request is taken back, so it does not run again on the next settings reload.
        pipeline->mutableSettings().vrchatCalibrate = 0;
        const auto fail = [&](const std::string& why) {
            Log("calibration: failed - %s", why.c_str());
            pipeline->persist({{"vrchat_calibrate", "0"}, {"vrchat_calibrated", "failed: " + why}});
            _running = false;
        };

        vr::IVROverlay* overlay = vr::VROverlay();
        if (!overlay) return fail("SteamVR's overlay interface is not available to this program");
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        vr::EVROverlayError error = overlay->CreateOverlay("kanaicasual.gazemirror.calibration", "Gaze Mirror calibration", &handle);
        if (error != vr::VROverlayError_None) {
            char text[96];
            snprintf(text, sizeof(text), "the target overlay could not be made (%d)", int(error));
            return fail(text);
        }
        const int size = 64;
        std::vector<uint8_t> pixels = TargetPixels(size);
        overlay->SetOverlayRaw(handle, pixels.data(), size, size, 4);
        overlay->SetOverlayWidthInMeters(handle, 0.10f);

        const Reach r = reach();
        Log("calibration: %s (the caps are %.0f sideways, %.0f up, %.0f down; trimmed to %.0f %% of the narrower eye's edge)",
            ReachText(r.left, r.right, r.up, r.down).c_str(), CapSideDeg, CapUpDeg, CapDownDeg, EdgeShare * 100.f);
        std::string why;
        const bool ok = _mode == 2 ? runThorough(link, pipeline, overlay, handle, r, why) : runQuick(link, pipeline, overlay, handle, r, why);
        overlay->HideOverlay(handle);
        overlay->DestroyOverlay(handle);
        if (!ok) return fail(why);
        _running = false;
    }

    // ---- quick: twenty resting targets --------------------------------------------------------------------------------

    bool GazeCalibration::runQuick(VrchatOscLink* link, Pipeline* pipeline, vr::IVROverlay* overlay, vr::VROverlayHandle_t handle, const Reach& r, std::string& why) {
        struct Target {
            float yawDeg, pitchDeg; // Right and up positive.
        };
        // Centre first, then outwards along each axis in turn, alternating sides so the eyes never travel far at once:
        // sideways in four steps, up in three and down in four (people look further down than up); then the four
        // corners. Twenty looks, about 45 seconds. Beyond the last target the map runs on in a straight line.
        const Target targets[] = {{0, 0},
                                  {0.27f * r.right, 0}, {-0.27f * r.left, 0}, {0.53f * r.right, 0}, {-0.53f * r.left, 0},
                                  {0.77f * r.right, 0}, {-0.77f * r.left, 0}, {r.right, 0}, {-r.left, 0},
                                  {0, 0.32f * r.up}, {0, -0.23f * r.down}, {0, 0.64f * r.up}, {0, -0.47f * r.down},
                                  {0, r.up}, {0, -0.73f * r.down}, {0, -r.down},
                                  {0.73f * r.right, 0.73f * r.up}, {-0.73f * r.left, 0.73f * r.up},
                                  {0.73f * r.right, -0.67f * r.down}, {-0.73f * r.left, -0.67f * r.down}};

        std::vector<std::pair<float, float>> xs, ys; // (value, tangent)
        struct Corner {
            float tanYaw, tanPitch, valueX, valueY;
        };
        std::vector<Corner> corners;
        int missing = 0;
        int step = 0;
        for (const Target& target : targets) {
            // The app shows which target is up (one small settings write per target; nothing runs on a timer). The request
            // itself is taken back in the same write: every write makes the settings reload, and a reload with the request
            // still in the file would queue a second run.
            char progress[32];
            snprintf(progress, sizeof(progress), "running %d/%d", ++step, int(std::size(targets)));
            pipeline->persist({{"vrchat_calibrate", "0"}, {"vrchat_calibrated", progress}});
            const float tanYaw = std::tan(target.yawDeg * Pi / 180.f), tanPitch = std::tan(target.pitchDeg * Pi / 180.f);
            Place(overlay, handle, tanYaw, tanPitch);
            overlay->ShowOverlay(handle);
            std::this_thread::sleep_for(std::chrono::milliseconds(SettleMs));
            std::vector<float> vx, vy;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(SampleMs);
            while (std::chrono::steady_clock::now() < until) {
                const VrchatOscLink::EyeSample sample = link->sample();
                if (sample.valid) {
                    vx.push_back(sample.x);
                    vy.push_back(sample.y);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (vx.size() < 20) {
                missing++;
                Log("calibration: target %+.0f/%+.0f deg - no eye values arrived", target.yawDeg, target.pitchDeg);
                continue;
            }
            const float mx = Median(vx), my = Median(vy);
            Log("calibration: target %+.0f/%+.0f deg -> value (%+.3f, %+.3f) from %zu samples", target.yawDeg, target.pitchDeg, mx, my, vx.size());
            if (target.pitchDeg == 0.f) xs.emplace_back(mx, tanYaw);
            if (target.yawDeg == 0.f) ys.emplace_back(my, tanPitch);
            if (target.pitchDeg != 0.f && target.yawDeg != 0.f) corners.push_back({tanYaw, tanPitch, mx, my});
        }

        if (xs.size() < 3 || ys.size() < 3) {
            why = missing > 0 ? "no eye values arrived from VRChat (is OSC on, and the avatar one with eye tracking?)" : "too few targets";
            return false;
        }
        const auto byValue = [](const std::pair<float, float>& a, const std::pair<float, float>& b) { return a.first < b.first; };
        std::sort(xs.begin(), xs.end(), byValue);
        std::sort(ys.begin(), ys.end(), byValue);
        // The curve must go one way: a value that reads higher must mean further along. Where it does not, the tracker
        // could not tell two targets apart; those are dropped rather than folded into the curve.
        const auto monotonic = [](std::vector<std::pair<float, float>>& map) {
            std::vector<std::pair<float, float>> kept;
            for (const auto& point : map) {
                if (!kept.empty() && point.second <= kept.back().second) continue;
                kept.push_back(point);
            }
            map = kept;
        };
        monotonic(xs);
        monotonic(ys);
        if (xs.size() < 2 || ys.size() < 2) {
            why = "the values did not change with the direction of the look";
            return false;
        }

        // The corners: what the axis maps alone make of each diagonal look, against where the target was. The gain per
        // axis is stored per quadrant (+x+y, -x+y, +x-y, -x-y); a missing or unusable corner leaves that quadrant at 1.
        std::vector<Settings::CornerGain> gains(4, Settings::CornerGain{0.f, 0.f, 1.f, 1.f});
        for (const Corner& corner : corners) {
            const float mappedX = Pipeline::MapValue(xs, corner.valueX), mappedY = Pipeline::MapValue(ys, corner.valueY);
            Settings::CornerGain& gain = gains[(corner.tanYaw < 0.f ? 1 : 0) + (corner.tanPitch < 0.f ? 2 : 0)];
            gain.tx = corner.tanYaw;
            gain.ty = corner.tanPitch;
            if (std::fabs(mappedX) > 0.05f) gain.gainX = std::clamp(corner.tanYaw / mappedX, 0.5f, 2.f);
            if (std::fabs(mappedY) > 0.05f) gain.gainY = std::clamp(corner.tanPitch / mappedY, 0.5f, 2.f);
            Log("calibration: corner %+.0f/%+.0f deg - the axis maps alone give %+.1f/%+.1f deg; gain %.3f x %.3f",
                std::atan(corner.tanYaw) * 180.f / Pi, std::atan(corner.tanPitch) * 180.f / Pi,
                std::atan(mappedX) * 180.f / Pi, std::atan(mappedY) * 180.f / Pi, gain.gainX, gain.gainY);
        }
        std::string mapCorners;
        if (corners.size() == 4) {
            for (const Settings::CornerGain& gain : gains) {
                char item[96];
                snprintf(item, sizeof(item), "%s%.4f,%.4f,%.4f,%.4f", mapCorners.empty() ? "" : ";", gain.tx, gain.ty, gain.gainX, gain.gainY);
                mapCorners += item;
            }
        } else {
            Log("calibration: %zu of 4 corners seen - no corner correction", corners.size());
            gains.clear();
        }

        const std::string mapX = MapText(xs), mapY = MapText(ys), when = Now();
        char info[160];
        snprintf(info, sizeof(info), "quick: %d targets, %s%s", int(std::size(targets)) - missing, ReachText(r.left, r.right, r.up, r.down).c_str(),
                 corners.size() == 4 ? "" : ", axes only");
        Log("calibration: done - x %s; y %s; corners %s; %s", mapX.c_str(), mapY.c_str(), mapCorners.empty() ? "none" : mapCorners.c_str(), info);
        Settings& s = pipeline->mutableSettings();
        s.vrchatMapX = xs;
        s.vrchatMapY = ys;
        s.vrchatCorners = gains;
        s.vrchatGrid = {};
        pipeline->persist({{"vrchat_calibrate", "0"}, {"vrchat_map_x", mapX}, {"vrchat_map_y", mapY}, {"vrchat_map_corners", mapCorners},
                           {"vrchat_map_grid", ""}, {"vrchat_calibration_info", info}, {"vrchat_calibrated", when}});
        return true;
    }

    // ---- thorough: the moving target ----------------------------------------------------------------------------------

    bool GazeCalibration::runThorough(VrchatOscLink* link, Pipeline* pipeline, vr::IVROverlay* overlay, vr::VROverlayHandle_t handle, const Reach& r, std::string& why) {
        const std::vector<grid::Waypoint> path = grid::BuildPath(r.left, r.right, r.up, r.down, SpeedDegPerS, PauseMs, StartSettleMs, StepMs);
        if (path.empty()) {
            why = "no path";
            return false;
        }
        const float totalMs = path.back().timeMs + StepMs;
        Log("calibration: the target's path takes %.0f s (%zu steps)", totalMs / 1000.f, path.size());

        std::vector<grid::Record> records;
        records.reserve(path.size());
        uint32_t lastSerial = 0;
        int lastProgress = -1;
        float blinkUntilMs = -1.f;
        Place(overlay, handle, path[0].tanX, path[0].tanY);
        overlay->ShowOverlay(handle);
        const auto begin = std::chrono::steady_clock::now();
        for (size_t i = 0; i < path.size(); i++) {
            const grid::Waypoint& w = path[i];
            // The overlay goes where the path says NOW (by the clock, so a late step never leaves the target behind).
            Place(overlay, handle, w.tanX, w.tanY);
            const VrchatOscLink::EyeSample sample = link->sample();
            const bool fresh = sample.valid && sample.serial != lastSerial;
            if (fresh) lastSerial = sample.serial;
            if (sample.valid && sample.openness < 0.15f) blinkUntilMs = w.timeMs + float(BlinkHoldMs);
            const bool open = sample.valid && sample.openness >= 0.15f && w.timeMs > blinkUntilMs;
            records.push_back({w.timeMs, w.tanX, w.tanY, w.paused, sample.x, sample.y, fresh, open});
            // Progress for the app in tenths (a settings write each; the request is taken back in the same write).
            const int progress = int(10.f * w.timeMs / totalMs);
            if (progress != lastProgress) {
                lastProgress = progress;
                char text[48];
                snprintf(text, sizeof(text), "running moving target, %d %%", progress * 10);
                pipeline->persist({{"vrchat_calibrate", "0"}, {"vrchat_calibrated", text}});
            }
            // Pace by the clock: sleep until this step's time.
            const auto due = begin + std::chrono::milliseconds(int(w.timeMs + StepMs));
            std::this_thread::sleep_until(due);
        }
        overlay->HideOverlay(handle);

        size_t freshCount = 0, openCount = 0;
        for (const grid::Record& rec : records) {
            if (rec.fresh) freshCount++;
            if (rec.fresh && rec.open) openCount++;
        }
        Log("calibration: %zu records, %zu with new eye values, %zu of those with the eyes open", records.size(), freshCount, openCount);
        if (openCount < 200) {
            why = freshCount < 50 ? "no eye values arrived from VRChat (is OSC on, and the avatar one with eye tracking?)" : "too few usable eye values";
            return false;
        }
        grid::Result result;
        if (!grid::Calibrate(records, StepMs, MaxLagMs, result)) {
            why = "the values could not be fitted";
            return false;
        }
        const std::string mapGrid = grid::ToText(result.grid), when = Now();
        char info[200];
        snprintf(info, sizeof(info), "thorough: %d samples (%d dropped), the eyes trail the target by %d ms, fit %.1f deg, %s", result.samples,
                 result.dropped, result.lagMs, result.errorDeg, ReachText(r.left, r.right, r.up, r.down).c_str());
        Log("calibration: done - %s; grid %s", info, mapGrid.c_str());
        Settings& s = pipeline->mutableSettings();
        s.vrchatGrid = result.grid;
        s.vrchatMapX.clear();
        s.vrchatMapY.clear();
        s.vrchatCorners.clear();
        pipeline->persist({{"vrchat_calibrate", "0"}, {"vrchat_map_grid", mapGrid}, {"vrchat_map_x", ""}, {"vrchat_map_y", ""}, {"vrchat_map_corners", ""},
                           {"vrchat_calibration_info", info}, {"vrchat_calibrated", when}});
        return true;
    }

} // namespace gaze_mirror
