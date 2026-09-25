#include "pch.h"

#include "settings.h"

namespace gaze_mirror {

    namespace {
        constexpr wchar_t SignalName[] = L"GazeOverlay.SettingsSignal";
        constexpr uint32_t SignalMagic = 0x53534F47; // 'GOSS'

        std::string Trim(const std::string& text) {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        float Number(const std::string& value, float fallback, float low, float high) {
            char* end = nullptr;
            const float parsed = strtof(value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite(parsed)) return fallback;
            return std::clamp(parsed, low, high);
        }
        // "tx,ty,gainX,gainY;..." -> the four corner gains, or nothing when they do not all read.
        std::vector<Settings::CornerGain> Corners(const std::string& value) {
            std::vector<Settings::CornerGain> corners;
            size_t start = 0;
            while (start < value.size()) {
                size_t end = value.find(';', start);
                if (end == std::string::npos) end = value.size();
                const std::string item = value.substr(start, end - start);
                float v[4];
                const char* p = item.c_str();
                bool ok = true;
                for (int i = 0; i < 4 && ok; i++) {
                    char* stop = nullptr;
                    v[i] = std::strtof(p, &stop);
                    ok = stop != p && std::isfinite(v[i]) && (i == 3 || *stop == ',');
                    p = stop + 1;
                }
                if (ok) corners.push_back({v[0], v[1], std::clamp(v[2], 0.5f, 2.f), std::clamp(v[3], 0.5f, 2.f)});
                start = end + 1;
            }
            if (corners.size() != 4) corners.clear();
            return corners;
        }
        // "value:tangent,value:tangent,..." -> sorted points; anything unreadable is left out.
        std::vector<std::pair<float, float>> Map(const std::string& value) {
            std::vector<std::pair<float, float>> points;
            size_t start = 0;
            while (start < value.size()) {
                size_t end = value.find(',', start);
                if (end == std::string::npos) end = value.size();
                const std::string item = value.substr(start, end - start);
                const size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    char* stop = nullptr;
                    const float raw = std::strtof(item.c_str(), &stop);
                    const float tangent = std::strtof(item.c_str() + colon + 1, nullptr);
                    if (stop == item.c_str() + colon && std::isfinite(raw) && std::isfinite(tangent)) points.emplace_back(raw, tangent);
                }
                start = end + 1;
            }
            std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            return points;
        }

        bool Flag(const std::string& value, bool fallback) {
            if (value == "0" || value == "false" || value == "off") return false;
            if (value == "1" || value == "true" || value == "on") return true;
            return fallback;
        }
    } // namespace

    std::wstring SettingsFilePath() {
        wchar_t overridden[MAX_PATH];
        const DWORD overriddenLength = GetEnvironmentVariableW(L"GAZE_MIRROR_SETTINGS_FILE", overridden, MAX_PATH);
        if (overriddenLength > 0 && overriddenLength < MAX_PATH) return overridden;
        wchar_t folder[MAX_PATH];
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return {};
        return std::wstring(folder) + L"\\XR_APILAYER_NOVENDOR_OBSMirror_gaze.cfg";
    }

    std::wstring ProfilesFilePath() {
        wchar_t overridden[MAX_PATH];
        const DWORD overriddenLength = GetEnvironmentVariableW(L"GAZE_MIRROR_PROFILES_FILE", overridden, MAX_PATH);
        if (overriddenLength > 0 && overriddenLength < MAX_PATH) return overridden;
        wchar_t folder[MAX_PATH];
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return {};
        return std::wstring(folder) + L"\\GazeMirror\\crop-profiles.ini";
    }

    void SettingsSource::setGame(const char* program, const char* application) {
        _program = program ? program : "";
        _application = application ? application : "";
        applyProfile(_settings);
    }

    SettingsSource::~SettingsSource() {
        if (_signal) UnmapViewOfFile(_signal);
        if (_mapping) CloseHandle(_mapping);
    }

    void SettingsSource::open() {
        if (!_mapping) {
            _mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Signal), SignalName);
            const bool created = _mapping && GetLastError() != ERROR_ALREADY_EXISTS;
            if (_mapping) _signal = static_cast<Signal*>(MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Signal)));
            if (_signal) {
                if (created) {
                    _signal->magic = SignalMagic;
                    _signal->version = 3;
                }
                _seenGeneration = _signal->generation;
            }
        }
        read();
    }

    bool SettingsSource::refreshIfSignalled() {
        if (!_signal) return false;
        const LONG now = _signal->generation;
        if (now == _seenGeneration) return false;
        _seenGeneration = now;
        read();
        return true;
    }

    void SettingsSource::read() {
        Settings fresh;
        const std::wstring path = SettingsFilePath();
        FILE* file = path.empty() ? nullptr : _wfsopen(path.c_str(), L"r", _SH_DENYNO);
        if (!file) {
            Log("settings: no file - defaults are used");
            _settings = fresh;
            return;
        }
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            std::string text = line;
            const size_t comment = text.find('#');
            if (comment != std::string::npos) text.erase(comment);
            const size_t equals = text.find('=');
            if (equals == std::string::npos) continue;
            ApplyKey(fresh, Trim(text.substr(0, equals)), Trim(text.substr(equals + 1)));
        }
        fclose(file);
        applyProfile(fresh);
        _settings = fresh;
        Log("settings: ring %s style %d, crop %s h %.3f aspect %.3f at (%.3f, %.3f) follow %s steady %s, eye %s, output max %u fps %.0f%s%s",
            fresh.enabled ? "on" : "off", int(fresh.style), fresh.cropEnabled ? "on" : "off", fresh.cropHeight, fresh.cropAspect,
            fresh.cropCenterX, fresh.cropCenterY, fresh.cropFollowVertical ? "on" : "off", fresh.stabilize ? "on" : "off",
            fresh.eye == 0 ? "left" : "right", fresh.outputMaxSide, fresh.outputFps, _activeProfile.empty() ? "" : ", crop profile ",
            _activeProfile.c_str());
    }

    // The crop profile made for the running game, if any, on top of the file's values.
    void SettingsSource::applyProfile(Settings& settings) {
        _activeProfile.clear();
        if (_program.empty() && _application.empty()) return;
        const std::wstring path = ProfilesFilePath();
        FILE* file = path.empty() ? nullptr : _wfsopen(path.c_str(), L"r", _SH_DENYNO);
        if (!file) return;
        // First pass: find the section whose game matches; second pass: apply its keys.
        std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> sections;
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            std::string text = Trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';') continue;
            if (text.front() == '[' && text.back() == ']') {
                sections.push_back({Trim(text.substr(1, text.size() - 2)), {}});
                continue;
            }
            const size_t equals = text.find('=');
            if (equals == std::string::npos || sections.empty()) continue;
            sections.back().second.push_back({Trim(text.substr(0, equals)), Trim(text.substr(equals + 1))});
        }
        fclose(file);
        const auto sameName = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && _strnicmp(a.c_str(), b.c_str(), a.size()) == 0;
        };
        for (const auto& section : sections) {
            bool matches = false;
            for (const auto& entry : section.second) {
                if (entry.first == "game" && !entry.second.empty() && (sameName(entry.second, _program) || sameName(entry.second, _application))) matches = true;
            }
            if (!matches) continue;
            for (const auto& entry : section.second) {
                if (entry.first != "game" && entry.first != "mirror_eye" && entry.first.rfind("output_", 0) != 0) ApplyKey(settings, entry.first, entry.second);
            }
            _activeProfile = section.first;
            return;
        }
    }

    void SettingsSource::ApplyKey(Settings& fresh, const std::string& key, const std::string& value) {
        {
            if (key == "enabled") fresh.enabled = Flag(value, fresh.enabled);
            else if (key == "style") {
                static const std::pair<const char*, Style> names[] = {{"ring", Style::Ring}, {"glow", Style::Glow}, {"dot", Style::Dot},
                                                                      {"spotlight", Style::Spotlight}, {"bubble", Style::Bubble},
                                                                      {"solid", Style::Solid}, {"heatmap", Style::Heatmap}, {"ghost", Style::Ghost}};
                for (const auto& name : names) if (value == name.first) fresh.style = name.second;
            } else if (key == "blend") fresh.screenBlend = value != "normal";
            else if (key == "solidity") fresh.solidity = Number(value, fresh.solidity, 0.f, 1.f);
            else if (key == "radius") fresh.radius = Number(value, fresh.radius, 0.001f, 1.f);
            else if (key == "thickness") fresh.thickness = Number(value, fresh.thickness, 0.f, 1.f);
            else if (key == "feather") fresh.feather = Number(value, fresh.feather, 0.0005f, 1.f);
            else if (key == "color") {
                int r = 255, g = 255, b = 255;
                if (sscanf_s(value.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
                    fresh.color[0] = std::clamp(r, 0, 255) / 255.f;
                    fresh.color[1] = std::clamp(g, 0, 255) / 255.f;
                    fresh.color[2] = std::clamp(b, 0, 255) / 255.f;
                }
            } else if (key == "opacity") fresh.opacity = Number(value, fresh.opacity, 0.f, 1.f);
            else if (key == "fill_opacity") fresh.fillOpacity = Number(value, fresh.fillOpacity, 0.f, 1.f);
            else if (key == "glow") fresh.glowWidth = Number(value, fresh.glowWidth, 0.0001f, 0.2f);
            else if (key == "glow_strength") fresh.glowStrength = Number(value, fresh.glowStrength, 0.f, 1.f);
            else if (key == "tail_opacity") fresh.tailOpacity = Number(value, fresh.tailOpacity, 0.f, 1.f);
            else if (key == "tail_max") fresh.tailMax = Number(value, fresh.tailMax, 0.f, 2.f);
            else if (key == "trail_ms") fresh.trailMs = Number(value, fresh.trailMs, 0.f, 10000.f);
            else if (key == "blob_trail_ms") fresh.blobTrailMs = Number(value, fresh.blobTrailMs, 0.f, 10000.f);
            else if (key == "heat_ms") fresh.heatMs = Number(value, fresh.heatMs, 50.f, 60000.f);
            else if (key == "heat_cool_ms") fresh.heatCoolMs = Number(value, fresh.heatCoolMs, 50.f, 60000.f);
            else if (key == "tail_space") fresh.worldAnchored = value != "screen";
            else if (key == "shadow_opacity") fresh.shadowOpacity = Number(value, fresh.shadowOpacity, 0.f, 1.f);
            else if (key == "filter") fresh.adaptiveFilter = value != "simple";
            else if (key == "filter_min_cutoff") fresh.filterMinCutoff = Number(value, fresh.filterMinCutoff, 0.05f, 60.f);
            else if (key == "filter_beta") fresh.filterBeta = Number(value, fresh.filterBeta, 0.f, 1000.f);
            else if (key == "smoothing_ms") fresh.smoothingMs = Number(value, fresh.smoothingMs, 0.f, 100000.f);
            else if (key == "hold_ms") fresh.holdMs = Number(value, fresh.holdMs, 0.f, 100000.f);
            else if (key == "fade_ms") fresh.fadeMs = Number(value, fresh.fadeMs, 0.f, 100000.f);
            else if (key == "dwell_ms") fresh.dwellMs = Number(value, fresh.dwellMs, 0.f, 100000.f);
            else if (key == "dwell_shrink") fresh.dwellShrink = Number(value, fresh.dwellShrink, 0.f, 0.9f);
            else if (key == "focus_distance") fresh.focusDistance = Number(value, fresh.focusDistance, 0.f, 100000.f);
            else if (key == "offset_x") fresh.offsetX = Number(value, 0.f, -1.f, 1.f);
            else if (key == "offset_y") fresh.offsetY = Number(value, 0.f, -1.f, 1.f);
            else if (key == "headset_marker") fresh.headsetMarker = Flag(value, false);
            else if (key == "crop_enabled") fresh.cropEnabled = Flag(value, false);
            else if (key == "crop_aspect") {
                float w = 0, h = 0;
                if (value == "free" || value == "0") fresh.cropAspect = 0.f;
                else if (sscanf_s(value.c_str(), "%f:%f", &w, &h) == 2 && w > 0 && h > 0) fresh.cropAspect = std::clamp(w / h, 0.1f, 10.f);
                else fresh.cropAspect = Number(value, fresh.cropAspect, 0.f, 10.f);
            } else if (key == "crop_center_x") fresh.cropCenterX = Number(value, 0.5f, 0.f, 1.f);
            else if (key == "crop_center_y") fresh.cropCenterY = Number(value, 0.5f, 0.f, 1.f);
            else if (key == "crop_height") fresh.cropHeight = Number(value, 0.5f, 0.02f, 1.f);
            else if (key == "crop_width") fresh.cropWidth = Number(value, 1.f, 0.02f, 1.f);
            else if (key == "crop_follow") fresh.cropFollowVertical = value == "vertical";
            else if (key == "crop_follow_deadzone") fresh.cropFollowDeadzone = Number(value, 0.4f, 0.f, 0.9f);
            else if (key == "crop_follow_reach") fresh.cropFollowReach = Number(value, 1.f, 0.f, 10.f);
            else if (key == "crop_follow_glide_ms") fresh.cropFollowGlideMs = Number(value, 500.f, 0.f, 10000.f);
            else if (key == "crop_follow_return") fresh.cropFollowReturn = Flag(value, true);
            else if (key == "crop_follow_home_ms") fresh.cropFollowHomeMs = Number(value, 1200.f, 0.f, 60000.f);
            else if (key == "stabilize") fresh.stabilize = Flag(value, false);
            else if (key == "stabilize_min_cutoff") fresh.stabilizeMinCutoff = Number(value, 1.f, 0.02f, 30.f);
            else if (key == "stabilize_beta") fresh.stabilizeBeta = Number(value, 8.f, 0.f, 200.f);
            else if (key == "stabilize_room") fresh.stabilizeRoom = Number(value, 0.02f, 0.f, 0.25f);
            else if (key == "mirror_eye") fresh.eye = value == "left" ? 0 : 1;
            else if (key == "output_max_side") fresh.outputMaxSide = static_cast<uint32_t>(Number(value, 3840.f, 0.f, 16384.f));
            else if (key == "output_fps") fresh.outputFps = Number(value, 0.f, 0.f, 1000.f);
            else if (key == "gaze_source") fresh.gazeSource = value == "headset" ? 1 : value == "vrchat" ? 2 : value == "sranibro" ? 3 : 0;
            else if (key == "raw_gaze_port") fresh.rawGazePort = int(Number(value, 9005.f, 1024.f, 65535.f));
            else if (key == "vrchat_scale") fresh.vrchatScale = Number(value, 1.6f, 0.1f, 8.f);
            else if (key == "vrchat_scale_up") fresh.vrchatScaleUp = Number(value, 1.6f, 0.1f, 8.f);
            else if (key == "vrchat_scale_down") fresh.vrchatScaleDown = Number(value, 1.6f, 0.1f, 8.f);
            else if (key == "vrchat_calibrate") fresh.vrchatCalibrate = Flag(value, false);
            else if (key == "vrchat_map_x") fresh.vrchatMapX = Map(value);
            else if (key == "vrchat_map_y") fresh.vrchatMapY = Map(value);
            else if (key == "vrchat_map_corners") fresh.vrchatCorners = Corners(value);
            else if (key == "vrchat_calibrated") fresh.vrchatCalibrated = value;
        }
    }

    // Rewrites only the lines of the given keys; unknown keys are appended. Comments and order stay as they are.
    void SettingsSource::persist(const std::vector<std::pair<std::string, std::string>>& values) {
        const std::wstring path = SettingsFilePath();
        if (path.empty()) return;
        std::vector<std::string> lines;
        if (FILE* file = _wfsopen(path.c_str(), L"r", _SH_DENYNO)) {
            char line[512];
            while (fgets(line, sizeof(line), file)) {
                std::string text = line;
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                lines.push_back(text);
            }
            fclose(file);
        }
        std::vector<bool> written(values.size(), false);
        for (std::string& line : lines) {
            std::string text = line;
            const size_t comment = text.find('#');
            if (comment != std::string::npos) text.erase(comment);
            const size_t equals = text.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = Trim(text.substr(0, equals));
            for (size_t i = 0; i < values.size(); i++) {
                if (key == values[i].first) {
                    line = key + "=" + values[i].second;
                    written[i] = true;
                }
            }
        }
        for (size_t i = 0; i < values.size(); i++) {
            if (!written[i]) lines.push_back(values[i].first + "=" + values[i].second);
        }
        const std::wstring temporary = path + L".tmp";
        FILE* file = _wfsopen(temporary.c_str(), L"w", _SH_DENYWR);
        if (!file) return;
        for (const std::string& line : lines) fprintf(file, "%s\n", line.c_str());
        fclose(file);
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

} // namespace gaze_mirror
