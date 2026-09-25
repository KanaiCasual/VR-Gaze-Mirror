// VR Gaze Mirror 2.0 - the OpenVR helper (GazeMirrorHelper.exe).
//
// The producer for games that use OpenVR instead of OpenXR (they never load an OpenXR layer). It sits next to
// SteamVR as a background program and, whenever a reader wants pictures, reads SteamVR's own mirror of one eye,
// SteamVR's eye-gaze point and the head pose, and hands them to the same core the OpenXR layer uses - so crop, framing
// and ring are the same, and the OBS plugin and the mirror window cannot tell the two apart.
//
//   - Started by SteamVR itself ("--steamvr" on that command line) once the settings app has registered it with
//     "--register" (undone with "--unregister"), or by the settings app. It never starts SteamVR: with SteamVR
//     closed it leaves at once.
//   - Leaves when SteamVR closes (VREvent_Quit) and when an OpenXR game's layer takes the shared block over it stands
//     by, on an event, until that game is gone.
//   - Nothing polls: with no reader it sleeps on the producer-wake event (SteamVR's own event queue is looked at
//     twice a second, which its API requires of every program connected to it).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <openvr.h>

#include "../core/log.h"
#include "../core/pipeline.h"
#include "calibration.h"
#include "headset_marker.h"
#include "raw_gaze.h"
#include "vrchat_osc.h"

using Microsoft::WRL::ComPtr;
using namespace gaze_mirror;

namespace {

    HANDLE g_stop = nullptr;

    BOOL WINAPI OnConsoleClose(DWORD) {
        SetEvent(g_stop);
        return TRUE;
    }

    // The rotation part of a 3x4 pose matrix as a quaternion (device -> world).
    Quat Orientation(const vr::HmdMatrix34_t& m) {
        Quat q;
        q.w = sqrtf(std::max(0.f, 1 + m.m[0][0] + m.m[1][1] + m.m[2][2])) / 2;
        q.x = sqrtf(std::max(0.f, 1 + m.m[0][0] - m.m[1][1] - m.m[2][2])) / 2;
        q.y = sqrtf(std::max(0.f, 1 - m.m[0][0] + m.m[1][1] - m.m[2][2])) / 2;
        q.z = sqrtf(std::max(0.f, 1 - m.m[0][0] - m.m[1][1] + m.m[2][2])) / 2;
        q.x = copysignf(q.x, m.m[2][1] - m.m[1][2]);
        q.y = copysignf(q.y, m.m[0][2] - m.m[2][0]);
        q.z = copysignf(q.z, m.m[1][0] - m.m[0][1]);
        return Normalize(q);
    }

    std::string ProcessName(uint32_t pid) {
        std::string name;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD length = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                std::wstring full(path, length);
                const size_t slash = full.find_last_of(L"\\/");
                if (slash != std::wstring::npos) full.erase(0, slash + 1);
                char utf8[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
                name = utf8;
            }
            CloseHandle(process);
        }
        return name;
    }

    struct SceneApp {
        uint32_t pid = 0;
        std::string program, key;
    };

    SceneApp CurrentScene(vr::IVRCompositor* compositor) {
        SceneApp scene;
        scene.pid = compositor->GetCurrentSceneFocusProcess();
        if (scene.pid == 0 || !vr::VRApplications()) return scene;
        scene.program = ProcessName(scene.pid);
        char key[vr::k_unMaxApplicationKeyLength] = {};
        vr::VRApplications()->GetApplicationKeyByProcessId(scene.pid, key, sizeof(key));
        scene.key = key;
        return scene;
    }

    struct Mirror {
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11Texture2D> texture;
        int eye = -1;
        uint32_t width = 0, height = 0;
    };

    bool OpenMirror(vr::IVRCompositor* compositor, ID3D11Device* device, int eye, Mirror& mirror) {
        if (mirror.view && mirror.eye == eye) return true;
        if (mirror.view) compositor->ReleaseMirrorTextureD3D11(mirror.view.Get());
        mirror = {};
        void* raw = nullptr;
        const vr::EVRCompositorError error = compositor->GetMirrorTextureD3D11(eye == 0 ? vr::Eye_Left : vr::Eye_Right, device, &raw);
        if (error != vr::VRCompositorError_None || !raw) {
            Log("helper: GetMirrorTextureD3D11 failed (%d)", int(error));
            return false;
        }
        mirror.view.Attach(static_cast<ID3D11ShaderResourceView*>(raw));
        ComPtr<ID3D11Resource> resource;
        mirror.view->GetResource(&resource);
        resource.As(&mirror.texture);
        D3D11_TEXTURE2D_DESC desc{};
        if (mirror.texture) mirror.texture->GetDesc(&desc);
        mirror.eye = eye;
        mirror.width = desc.Width;
        mirror.height = desc.Height;
        Log("helper: mirror of the %s eye: %u x %u, format %d", eye == 0 ? "left" : "right", desc.Width, desc.Height, int(desc.Format));
        return mirror.texture != nullptr;
    }

    constexpr char AppKey[] = "kanaicasual.quadviewsgazemirror.helper";

    // Tells SteamVR about this program (an application manifest) and asks it to start it with SteamVR, or takes that
    // back. Works without a headset (utility connection). Exit code 0 = done.
    //
    // The manifest sits next to the exe and names it by a relative path: the installer ships it there, so an uninstall
    // takes it away and SteamVR is left with nothing to start. Where the exe's folder has none and cannot be written to
    // (nothing here ever runs elevated), one goes to the user's data folder with the full path instead.
    int Register(bool on) {
        wchar_t modulePath[MAX_PATH];
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring exeFolder = modulePath;
        exeFolder.erase(exeFolder.find_last_of(L'\\'));
        wchar_t dataFolder[MAX_PATH];
        const DWORD dataLength = GetEnvironmentVariableW(L"LOCALAPPDATA", dataFolder, MAX_PATH);
        if (dataLength == 0 || dataLength >= MAX_PATH) return 2;
        const std::wstring dataManifest = std::wstring(dataFolder) + L"\\GazeMirror\\GazeMirrorHelper.vrmanifest";
        const std::wstring legacyManifest = std::wstring(dataFolder) + L"\\QuadViewsGazeMirror\\GazeMirrorHelper.vrmanifest";
        const auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
        const auto utf8 = [](const std::wstring& wide) {
            char text[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text, sizeof(text) - 1, nullptr, nullptr);
            return std::string(text);
        };
        const auto writeManifest = [](const std::wstring& path, const std::string& binary) {
            FILE* file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
            if (!file) return false;
            // is_dashboard_overlay: SteamVR only auto-launches programs it regards as overlays; this one never shows one.
            fprintf(file,
                    "{\n  \"source\": \"builtin\",\n  \"applications\": [ {\n"
                    "    \"app_key\": \"%s\",\n    \"launch_type\": \"binary\",\n"
                    "    \"binary_path_windows\": \"%s\",\n    \"arguments\": \"--steamvr\",\n"
                    "    \"is_dashboard_overlay\": true,\n"
                    "    \"strings\": { \"en_us\": { \"name\": \"VR Gaze Mirror helper\", "
                    "\"description\": \"Mirror picture with gaze ring for SteamVR games (OBS, mirror window)\" } }\n  } ]\n}\n",
                    AppKey, binary.c_str());
            fclose(file);
            return true;
        };

        std::wstring manifestPath = exeFolder + L"\\GazeMirrorHelper.vrmanifest";
        if (on && !exists(manifestPath) && !writeManifest(manifestPath, "GazeMirrorHelper.exe")) {
            std::string exePath; // JSON: backslashes and quotes escaped.
            for (const char c : utf8(modulePath)) {
                if (c == '\\' || c == '"') exePath += '\\';
                exePath += c;
            }
            CreateDirectoryW((std::wstring(dataFolder) + L"\\GazeMirror").c_str(), nullptr);
            manifestPath = dataManifest;
            if (!writeManifest(manifestPath, exePath)) {
                Log("register: the manifest could not be written");
                return 2;
            }
        }

        vr::EVRInitError initError = vr::VRInitError_None;
        vr::VR_Init(&initError, vr::VRApplication_Utility);
        if (initError != vr::VRInitError_None || !vr::VRApplications()) {
            Log("register: SteamVR's application list is not reachable (%d: %s)", int(initError), vr::VR_GetVRInitErrorAsEnglishDescription(initError));
            return 3;
        }
        int result = 0;
        if (on) {
            // Manifests earlier builds left elsewhere carry the same key, and SteamVR starts the binary of the FIRST
            // manifest it has for a key: the 1.x and 1.9 dev builds' one under the old data folder name, the 1.9.x
            // builds' one in the data folder. They go first, so that the one next to the exe is what SteamVR starts.
            for (const std::wstring& old : {legacyManifest, dataManifest}) {
                if (old == manifestPath || !exists(old)) continue;
                const vr::EVRApplicationError removed = vr::VRApplications()->RemoveApplicationManifest(utf8(old).c_str());
                Log("register: an earlier build's manifest removed (%d)", int(removed));
                DeleteFileW(old.c_str());
                std::wstring oldFolder = old;
                oldFolder.erase(oldFolder.find_last_of(L'\\'));
                RemoveDirectoryW(oldFolder.c_str()); // Only goes when empty - the 1.x folder, once the app has moved its files.
            }
            const vr::EVRApplicationError added = vr::VRApplications()->AddApplicationManifest(utf8(manifestPath).c_str());
            const vr::EVRApplicationError launch = added == vr::VRApplicationError_None ? vr::VRApplications()->SetApplicationAutoLaunch(AppKey, true)
                                                                                         : added;
            char resolved[MAX_PATH * 3] = {};
            vr::VRApplications()->GetApplicationPropertyString(AppKey, vr::VRApplicationProperty_BinaryPath_String, resolved, sizeof(resolved));
            Log("register: manifest %d, auto-launch %d; SteamVR will start %s", int(added), int(launch), resolved);
            result = launch == vr::VRApplicationError_None ? 0 : 4;
        } else {
            vr::VRApplications()->SetApplicationAutoLaunch(AppKey, false);
            for (const std::wstring& path : {manifestPath, dataManifest, legacyManifest}) {
                if (!exists(path)) continue;
                const vr::EVRApplicationError removed = vr::VRApplications()->RemoveApplicationManifest(utf8(path).c_str());
                Log("unregister: manifest removed (%d)", int(removed));
                DeleteFileW(path.c_str()); // Fails harmlessly on the installer's copy in Program Files; the uninstall takes that one.
            }
        }
        vr::VR_Shutdown();
        return result;
    }

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    SetLogName(L"gaze-mirror-helper");
    if (wcsstr(commandLine, L"--register")) return Register(true);
    if (wcsstr(commandLine, L"--unregister")) return Register(false);
    if (wcsstr(commandLine, L"--vrchat-test")) {
        // Development aid: only the VRChat link, without SteamVR, for half a minute - to poke it from a script.
        VrchatOscLink link;
        RawGazeLink raw;
        if (!link.start()) return 1;
        raw.start(9005);
        Sleep(30000);
        raw.stop();
        link.stop();
        return 0;
    }
    const bool bySteamVR = wcsstr(commandLine, L"--steamvr") != nullptr;
    HANDLE running = CreateMutexW(nullptr, TRUE, L"GazeMirror2.HelperRunning");
    if (running && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("helper: already running - leaving");
        return 0;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, L"GazeMirror2.HelperStop"); // The app can ask it to leave.
    ResetEvent(g_stop);
    SetConsoleCtrlHandler(OnConsoleClose, TRUE);

    vr::EVRInitError initError = vr::VRInitError_None;
    vr::IVRSystem* system = vr::VR_Init(&initError, bySteamVR ? vr::VRApplication_Overlay : vr::VRApplication_Background);
    if (!system && bySteamVR) system = vr::VR_Init(&initError, vr::VRApplication_Background);
    if (!system) {
        Log("helper: SteamVR is not running or refused (%d: %s) - leaving", int(initError), vr::VR_GetVRInitErrorAsEnglishDescription(initError));
        return 0;
    }
    vr::IVRCompositor* compositor = vr::VRCompositor();
    if (!compositor) {
        Log("helper: no compositor - leaving");
        vr::VR_Shutdown();
        return 0;
    }
    Log("helper: joined SteamVR %s (%s)", system->GetRuntimeVersion(), bySteamVR ? "started by SteamVR" : "started by the app");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        Log("helper: no Direct3D 11 device - leaving");
        vr::VR_Shutdown();
        return 0;
    }

    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, ProducerWakeName);
    Pipeline pipeline;
    // VRChat's eye parameters over OSC, for headsets SteamVR gets no gaze from. Runs as long as the helper does.
    VrchatOscLink vrchat;
    vrchat.start();
    // SRanibro's raw gaze, when its "Raw gaze OSC" switch is on: exact angles ahead of the VRChat fallback.
    RawGazeLink rawGaze;
    rawGaze.start(uint16_t(pipeline.settings().rawGazePort));
    GazeCalibration calibration;
    // A calibration that was under way when the last helper went away: say so, rather than count forever.
    if (pipeline.settings().vrchatCalibrated.rfind("running", 0) == 0) pipeline.persist({{"vrchat_calibrated", "failed: the previous run did not finish"}});
    HeadsetMarker marker; // The Placement tab's calibration marker, as an overlay (the layer draws it into OpenXR games).
    Mirror mirror;
    SceneApp scene;
    std::vector<vr::TrackedDevicePose_t> poses(vr::k_unMaxTrackedDeviceCount);
    bool publishing = false;
    bool leaving = false;
    int idleTicks = 0;
    float displayHz = system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    if (!(displayHz > 20.f && displayHz < 500.f)) displayHz = 90.f;

    const auto pumpEvents = [&]() {
        vr::VREvent_t event{};
        while (system->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                Log("helper: SteamVR is closing");
                system->AcknowledgeQuit_Exiting();
                leaving = true;
            } else if (event.eventType == vr::VREvent_SceneApplicationChanged) {
                const SceneApp now = CurrentScene(compositor);
                if (now.pid != scene.pid) {
                    scene = now;
                    Log("helper: the game is now \"%s\" (%s)", scene.program.c_str(), scene.key.c_str());
                    if (publishing) pipeline.stop(), publishing = false; // The next round starts it again with the new name.
                }
            }
        }
    };

    while (!leaving && WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        pumpEvents();
        if (leaving) break;

        if (!publishing) {
            if (scene.pid == 0) scene = CurrentScene(compositor);
            publishing = pipeline.start(device.Get(), ProducerOpenVR, scene.program.c_str(), scene.key.c_str());
            if (!publishing) {
                // Somebody else publishes (an OpenXR game's layer): stand by until told otherwise.
                const HANDLE handles[2] = {g_stop, wake};
                WaitForMultipleObjects(2, handles, FALSE, 500);
                continue;
            }
            Log("helper: publishing for \"%s\"", scene.program.c_str());
        }
        if (pipeline.lost()) {
            // An OpenXR game's layer took the block over: stand by until it is gone.
            Log("helper: an OpenXR game's layer has taken over - standing by");
            pipeline.stop();
            publishing = false;
            if (mirror.view) compositor->ReleaseMirrorTextureD3D11(mirror.view.Get());
            mirror = {};
            continue;
        }
        const bool wanted = pipeline.wanted(); // Also reloads the settings when the app signalled a change.
        // The app asked for the VRChat gaze calibration: the target in the headset, on its own thread.
        if (pipeline.settings().vrchatCalibrate && !calibration.running()) calibration.start(vrchat, pipeline);
        // Nothing to do: sleep until a reader turns up (SteamVR's events are looked at twice a second meanwhile).
        if (!wanted) {
            marker.hide(); // Switched off: no frame runs to take it down otherwise.
            const HANDLE handles[2] = {g_stop, wake};
            WaitForMultipleObjects(2, handles, FALSE, 500);
            if (++idleTicks % 120 == 0 && publishing && pipeline.settings().enabled) {
                // Roughly once a minute while idle: let go of the mirror texture, SteamVR need not keep it for us.
                if (mirror.view) compositor->ReleaseMirrorTextureD3D11(mirror.view.Get());
                mirror = {};
            }
            continue;
        }
        idleTicks = 0;
        if (publishing && pipeline.settings().eye != mirror.eye) {
            // Once per opened mirror: the eye's geometry, so that where a gaze lands can be checked from the log.
            for (int e = 0; e < 2; e++) {
                float l = 0, r = 0, t = 0, b = 0;
                system->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
                const vr::HmdMatrix34_t m = system->GetEyeToHeadTransform(e == 0 ? vr::Eye_Left : vr::Eye_Right);
                const Quat q = Orientation(m);
                Log("helper: %s eye fov tangents left %.3f right %.3f top %.3f bottom %.3f; eye-to-head position (%.4f, %.4f, %.4f) rotation (%.4f, %.4f, %.4f, %.4f)",
                    e == 0 ? "left" : "right", l, r, t, b, m.m[0][3], m.m[1][3], m.m[2][3], q.x, q.y, q.z, q.w);
            }
            if (!OpenMirror(compositor, device.Get(), pipeline.settings().eye, mirror)) {
                WaitForSingleObject(g_stop, 1000);
                continue;
            }
        }

        // Pace with the compositor; a program that does not render is told so - then pace by the display instead.
        const vr::EVRCompositorError paced = compositor->WaitGetPoses(poses.data(), vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        if (paced != vr::VRCompositorError_None) {
            system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, poses.data(), vr::k_unMaxTrackedDeviceCount);
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(1000000.f / displayHz)));
        }
        const vr::TrackedDevicePose_t& head = poses[vr::k_unTrackedDeviceIndex_Hmd];

        FrameInput input;
        SourceImage image;
        image.texture = mirror.texture.Get();
        image.viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // SteamVR's mirror is sRGB: sampled values are linear.
        image.linearLight = true;
        image.width = static_cast<int32_t>(mirror.width);
        image.height = static_cast<int32_t>(mirror.height);
        const int eye = mirror.eye;
        input.images[eye] = &image;
        for (int e = 0; e < 2; e++) {
            EyeView& view = input.views[e];
            float left = 0, right = 0, top = 0, bottom = 0;
            system->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right, &left, &right, &top, &bottom);
            view.width = float(mirror.width);
            view.height = float(mirror.height);
            // OpenVR's raw "top" is the tangent of the edge that lies BELOW the view axis (negative) and its "bottom"
            // the edge above (positive) - the same way its own projection matrix is composed. Read them the other way
            // and the view axis lands too low: on a Crystal Super the horizon sat at 43 % of the picture, the ring at 57 %.
            view.fov = {left, right, bottom, top};
            view.orientation = head.bPoseIsValid ? Orientation(head.mDeviceToAbsoluteTracking) : Quat{0, 0, 0, 1};
            view.valid = true;
            const vr::HmdMatrix34_t eyeToHead = system->GetEyeToHeadTransform(e == 0 ? vr::Eye_Left : vr::Eye_Right);
            input.eyeInHead[e].position = {eyeToHead.m[0][3], eyeToHead.m[1][3], eyeToHead.m[2][3]};
            input.eyeInHead[e].orientation = Orientation(eyeToHead);
        }
        vr::HmdVector2_t ndc[2];
        if (system->GetEyeTrackedFoveationCenter(&ndc[0], &ndc[1])) {
            input.gaze.valid = true;
            input.gaze.hasPerEyeUv = true;
            for (int e = 0; e < 2; e++) {
                input.gaze.uv[e][0] = 0.5f + 0.5f * ndc[e].v[0];
                input.gaze.uv[e][1] = 0.5f - 0.5f * ndc[e].v[1];
            }
        }
        const FrameOutput output = pipeline.frame(input);
        marker.update(output, input, eye);
    }

    marker.hide();
    if (mirror.view) compositor->ReleaseMirrorTextureD3D11(mirror.view.Get());
    mirror = {};
    rawGaze.stop();
    vrchat.stop();
    pipeline.stop();
    vr::VR_Shutdown();
    if (wake) CloseHandle(wake);
    Log("helper: left");
    return 0;
}
