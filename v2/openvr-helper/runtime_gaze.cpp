#ifndef NOMINMAX
#define NOMINMAX
#endif
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "runtime_gaze.h"

#include "../core/log.h"

using Microsoft::WRL::ComPtr;

namespace gaze_mirror {

    struct RuntimeGaze::Calls {
        PFN_xrGetInstanceProcAddr get = nullptr;
        PFN_xrDestroyInstance destroyInstance = nullptr;
        PFN_xrDestroySession destroySession = nullptr;
        PFN_xrSyncActions syncActions = nullptr;
        PFN_xrLocateSpace locateSpace = nullptr;
        PFN_xrPollEvent pollEvent = nullptr;
        PFN_xrConvertWin32PerformanceCounterToTimeKHR toTime = nullptr;
    };

    namespace {

        // The active runtime's name and library, from its manifest (HKLM\SOFTWARE\Khronos\OpenXR\1, ActiveRuntime).
        bool ActiveRuntime(std::string& name, std::wstring& library) {
            wchar_t manifest[MAX_PATH] = {};
            DWORD size = sizeof(manifest);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, manifest, &size) != ERROR_SUCCESS) return false;
            std::ifstream file(manifest);
            if (!file) return false;
            std::stringstream text;
            text << file.rdbuf();
            const std::string json = text.str();
            const auto value = [&](const char* key) -> std::string {
                const size_t at = json.find(std::string("\"") + key + "\"");
                if (at == std::string::npos) return "";
                const size_t colon = json.find(':', at);
                if (colon == std::string::npos) return "";
                const size_t open = json.find('"', colon + 1);
                const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
                if (close == std::string::npos) return "";
                std::string out;
                for (size_t i = open + 1; i < close; i++) {
                    if (json[i] == '\\' && i + 1 < close) i++;
                    out += json[i];
                }
                return out;
            };
            name = value("name");
            const std::string path = value("library_path");
            if (path.empty()) return false;
            std::wstring wide(path.begin(), path.end());
            if (wide.size() > 1 && wide[1] == L':') {
                library = wide;
            } else {
                std::wstring folder = manifest;
                folder = folder.substr(0, folder.find_last_of(L"\\/") + 1);
                if (wide.rfind(L".\\", 0) == 0 || wide.rfind(L"./", 0) == 0) wide = wide.substr(2);
                library = folder + wide;
            }
            return true;
        }

        bool IsSteamVR(const std::string& name, const std::wstring& library) {
            std::wstring lower = library;
            for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
            return name.find("SteamVR") != std::string::npos || lower.find(L"steamvr") != std::wstring::npos || lower.find(L"vrclient") != std::wstring::npos;
        }

    } // namespace

    RuntimeGaze::~RuntimeGaze() {
        stop();
        delete _calls;
    }

    bool RuntimeGaze::start() {
        if (_session) return true;
        std::wstring library;
        if (!ActiveRuntime(_name, library)) {
            Log("runtime gaze: no active OpenXR runtime is set on this PC");
            return false;
        }
        if (IsSteamVR(_name, library)) {
            Log("runtime gaze: the active OpenXR runtime is SteamVR's own (\"%s\") - its gaze is the one SteamVR gives", _name.c_str());
            return false;
        }
        if (!_module) _module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!_module) {
            Log("runtime gaze: \"%s\" could not be loaded (%lu)", _name.c_str(), GetLastError());
            return false;
        }
        const auto negotiate = reinterpret_cast<XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*)>(
            GetProcAddress(static_cast<HMODULE>(_module), "xrNegotiateLoaderRuntimeInterface"));
        XrNegotiateLoaderInfo loaderInfo{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo)};
        loaderInfo.minInterfaceVersion = 1;
        loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest)};
        if (!negotiate || XR_FAILED(negotiate(&loaderInfo, &request)) || !request.getInstanceProcAddr) {
            Log("runtime gaze: \"%s\" did not negotiate", _name.c_str());
            return false;
        }
        if (!_calls) _calls = new Calls;
        _calls->get = request.getInstanceProcAddr;

        // What the runtime offers: all three are needed.
        PFN_xrVoidFunction function = nullptr;
        _calls->get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &function);
        const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(function);
        uint32_t count = 0;
        if (!enumerate || XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) return false;
        std::vector<XrExtensionProperties> offered(count, {XR_TYPE_EXTENSION_PROPERTIES});
        enumerate(nullptr, count, &count, offered.data());
        const char* wanted[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
        for (const char* name : wanted) {
            bool found = false;
            for (const XrExtensionProperties& e : offered) found = found || strcmp(e.extensionName, name) == 0;
            if (!found) {
                Log("runtime gaze: \"%s\" does not offer %s - no gaze this way", _name.c_str(), name);
                return false;
            }
        }

        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(instanceInfo.applicationInfo.applicationName, "VR Gaze Mirror (gaze only)");
        strcpy_s(instanceInfo.applicationInfo.engineName, "none");
        instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        instanceInfo.enabledExtensionCount = 3;
        instanceInfo.enabledExtensionNames = wanted;
        _calls->get(XR_NULL_HANDLE, "xrCreateInstance", &function);
        XrInstance instance = XR_NULL_HANDLE;
        XrResult result = function ? reinterpret_cast<PFN_xrCreateInstance>(function)(&instanceInfo, &instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
        if (XR_FAILED(result)) {
            Log("runtime gaze: \"%s\" made no instance (%d)", _name.c_str(), result);
            return false;
        }
        bool complete = true;
        const auto call = [&](const char* name) {
            PFN_xrVoidFunction f = nullptr;
            if (XR_FAILED(_calls->get(instance, name, &f)) || !f) complete = false;
            return f;
        };
        const auto getSystem = reinterpret_cast<PFN_xrGetSystem>(call("xrGetSystem"));
        const auto getSystemProperties = reinterpret_cast<PFN_xrGetSystemProperties>(call("xrGetSystemProperties"));
        const auto getRequirements = reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(call("xrGetD3D11GraphicsRequirementsKHR"));
        const auto createSession = reinterpret_cast<PFN_xrCreateSession>(call("xrCreateSession"));
        const auto createActionSet = reinterpret_cast<PFN_xrCreateActionSet>(call("xrCreateActionSet"));
        const auto createAction = reinterpret_cast<PFN_xrCreateAction>(call("xrCreateAction"));
        const auto stringToPath = reinterpret_cast<PFN_xrStringToPath>(call("xrStringToPath"));
        const auto suggest = reinterpret_cast<PFN_xrSuggestInteractionProfileBindings>(call("xrSuggestInteractionProfileBindings"));
        const auto attach = reinterpret_cast<PFN_xrAttachSessionActionSets>(call("xrAttachSessionActionSets"));
        const auto createActionSpace = reinterpret_cast<PFN_xrCreateActionSpace>(call("xrCreateActionSpace"));
        const auto createReferenceSpace = reinterpret_cast<PFN_xrCreateReferenceSpace>(call("xrCreateReferenceSpace"));
        _calls->destroyInstance = reinterpret_cast<PFN_xrDestroyInstance>(call("xrDestroyInstance"));
        _calls->destroySession = reinterpret_cast<PFN_xrDestroySession>(call("xrDestroySession"));
        _calls->syncActions = reinterpret_cast<PFN_xrSyncActions>(call("xrSyncActions"));
        _calls->locateSpace = reinterpret_cast<PFN_xrLocateSpace>(call("xrLocateSpace"));
        _calls->pollEvent = reinterpret_cast<PFN_xrPollEvent>(call("xrPollEvent"));
        _calls->toTime = reinterpret_cast<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(call("xrConvertWin32PerformanceCounterToTimeKHR"));
        const auto fail = [&](const char* what, XrResult code) {
            Log("runtime gaze: \"%s\" - %s (%d); no gaze this way", _name.c_str(), what, code);
            if (_calls->destroyInstance) _calls->destroyInstance(instance);
            return false;
        };
        if (!complete) return fail("a function is missing", XR_ERROR_FUNCTION_UNSUPPORTED);

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        result = getSystem(instance, &systemInfo, &system);
        if (XR_FAILED(result)) return fail("it has no headset", result);
        XrSystemEyeGazeInteractionPropertiesEXT eyeProperties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eyeProperties};
        result = getSystemProperties(instance, system, &properties);
        if (XR_FAILED(result) || !eyeProperties.supportsEyeGazeInteraction) return fail("the headset has no eye gaze", result);

        // A session wants a graphics device, though nothing is ever drawn: one on the card the runtime names.
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        result = getRequirements(instance, system, &requirements);
        if (XR_FAILED(result)) return fail("no graphics requirements", result);
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter, chosen;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) chosen = adapter;
            }
        }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        _device.Reset();
        if (FAILED(D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &_device,
                                     nullptr, nullptr))) {
            return fail("no Direct3D 11 device for the session", XR_ERROR_RUNTIME_FAILURE);
        }
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = _device.Get();
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding};
        sessionInfo.systemId = system;
        XrSession session = XR_NULL_HANDLE;
        result = createSession(instance, &sessionInfo, &session);
        if (XR_FAILED(result)) return fail("it will not have a second program", result);

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "gaze_mirror");
        strcpy_s(setInfo.localizedActionSetName, "VR Gaze Mirror");
        XrActionSet actionSet = XR_NULL_HANDLE;
        XrAction action = XR_NULL_HANDLE;
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(actionInfo.actionName, "eye_gaze");
        strcpy_s(actionInfo.localizedActionName, "Eye gaze");
        XrPath profile = XR_NULL_PATH, gazePath = XR_NULL_PATH;
        XrSpace gazeSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
        const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
        result = createActionSet(instance, &setInfo, &actionSet);
        if (XR_SUCCEEDED(result)) result = createAction(actionSet, &actionInfo, &action);
        if (XR_SUCCEEDED(result)) result = stringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile);
        if (XR_SUCCEEDED(result)) result = stringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath);
        if (XR_SUCCEEDED(result)) {
            const XrActionSuggestedBinding suggested{action, gazePath};
            XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestion.interactionProfile = profile;
            suggestion.suggestedBindings = &suggested;
            suggestion.countSuggestedBindings = 1;
            result = suggest(instance, &suggestion);
        }
        if (XR_SUCCEEDED(result)) {
            XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attachInfo.actionSets = &actionSet;
            attachInfo.countActionSets = 1;
            result = attach(session, &attachInfo);
        }
        if (XR_SUCCEEDED(result)) {
            XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            spaceInfo.action = action;
            spaceInfo.poseInActionSpace = identity;
            result = createActionSpace(session, &spaceInfo, &gazeSpace);
        }
        if (XR_SUCCEEDED(result)) {
            XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            viewInfo.poseInReferenceSpace = identity;
            result = createReferenceSpace(session, &viewInfo, &viewSpace);
        }
        if (XR_FAILED(result)) {
            _calls->destroySession(session);
            return fail("the eye-gaze action could not be set up", result);
        }

        _instance = reinterpret_cast<uint64_t>(instance);
        _session = reinterpret_cast<uint64_t>(session);
        _actionSet = reinterpret_cast<uint64_t>(actionSet);
        _action = reinterpret_cast<uint64_t>(action);
        _gazeSpace = reinterpret_cast<uint64_t>(gazeSpace);
        _viewSpace = reinterpret_cast<uint64_t>(viewSpace);
        _logSample = _logLost = 0;
        Log("runtime gaze: a dormant session with \"%s\" (system \"%s\") - the headset's own eye gaze, read through OpenXR", _name.c_str(), properties.systemName);
        return true;
    }

    void RuntimeGaze::stop() {
        if (_calls && _session) {
            _calls->destroySession(reinterpret_cast<XrSession>(_session));
            Log("runtime gaze: the dormant session is closed");
        }
        if (_calls && _instance) _calls->destroyInstance(reinterpret_cast<XrInstance>(_instance));
        _instance = _session = _actionSet = _action = _gazeSpace = _viewSpace = 0;
        _device.Reset();
        // The runtime's library stays loaded: unloading a runtime is not something runtimes are written for.
    }

    GazeFrame RuntimeGaze::sample() {
        GazeFrame gaze;
        if (!_calls || !_session) return gaze;
        const XrInstance instance = reinterpret_cast<XrInstance>(_instance);
        const XrSession session = reinterpret_cast<XrSession>(_session);

        // The runtime's events are taken off its queue; if it ends the session, this source ends with it.
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (_calls->pollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrSessionState state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state;
                if (state == XR_SESSION_STATE_LOSS_PENDING || state == XR_SESSION_STATE_EXITING) {
                    Log("runtime gaze: \"%s\" ends the session (state %d)", _name.c_str(), int(state));
                    stop();
                    return gaze;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                Log("runtime gaze: \"%s\" is going away", _name.c_str());
                stop();
                return gaze;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        // The sync says "not focused" for a session that never runs; the runtime keeps the action alive all the same.
        const XrActiveActionSet active{reinterpret_cast<XrActionSet>(_actionSet), XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.activeActionSets = &active;
        syncInfo.countActiveActionSets = 1;
        _calls->syncActions(session, &syncInfo);

        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        XrTime time = 0;
        if (XR_FAILED(_calls->toTime(instance, &counter, &time))) return gaze;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult located = _calls->locateSpace(reinterpret_cast<XrSpace>(_gazeSpace), reinterpret_cast<XrSpace>(_viewSpace), time, &location);
        if (XR_FAILED(located) || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            LogFewTimes(_logLost, 3, "runtime gaze: no gaze right now (locate %d, flags 0x%llx)", located, static_cast<unsigned long long>(location.locationFlags));
            return gaze;
        }
        const Quat q{location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w};
        gaze.direction = Rotate(Normalize(q), {0, 0, -1});
        if (!std::isfinite(gaze.direction.x) || !std::isfinite(gaze.direction.y) || !std::isfinite(gaze.direction.z) || gaze.direction.z > -0.2f) return gaze;
        if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) gaze.origin = {location.pose.position.x, location.pose.position.y, location.pose.position.z};
        gaze.valid = true;
        gaze.hasRay = true;
        LogFewTimes(_logSample, 3, "runtime gaze: %.1f degrees right, %.1f up", atan2f(gaze.direction.x, -gaze.direction.z) * 57.29578f,
                    atan2f(gaze.direction.y, sqrtf(gaze.direction.x * gaze.direction.x + gaze.direction.z * gaze.direction.z)) * 57.29578f);
        return gaze;
    }

} // namespace gaze_mirror
