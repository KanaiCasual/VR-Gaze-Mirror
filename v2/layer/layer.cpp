// VR Gaze Mirror 2.0 - the OpenXR API layer.
//
// Written from nothing but the OpenXR headers: the loader hands us the chain, we put ourselves in front of a dozen
// functions and pass everything else straight through. The layer itself only
//   - remembers which swapchain image the game released last (no copy is made),
//   - shares the eye tracker with the game and other layers (GazeInput),
//   - at xrEndFrame, hands the core (v2\core) both eyes' images, the head pose and the gaze; the core does the rest,
//   - draws the calibration marker into the headset's images when the settings app asks for it.
// It sits BELOW Quad-Views-Foveated (closer to the runtime), so what it sees is the finished stereo image.

#include "pch.h"

#include "../core/log.h"
#include "../core/pipeline.h"
#include "gaze_input.h"
#include "marker.h"
#include "next.h"

namespace gaze_mirror::layer {

    namespace {
        constexpr char LayerName[] = "XR_APILAYER_NOVENDOR_gaze_mirror";

        struct SwapchainState {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<ID3D11Texture2D*> images;
            std::deque<uint32_t> acquired; // Images are released in the order they were acquired.
            int lastReleased = -1;
        };

        struct State {
            Next next;
            std::string program;
            std::string application;

            std::mutex lock; // Guards the swapchain table; the rest is only touched by the thread that ends frames.
            std::unordered_map<XrSwapchain, SwapchainState> swapchains;

            XrSession session = XR_NULL_HANDLE; // The Direct3D 11 session we mirror, if any.
            XrViewConfigurationType viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            bool pipelineReady = false;
            Pipeline pipeline;
            Marker marker;
            GazeInput gaze;
            PFN_xrBeginSession nextBeginSession = nullptr;
            int logFrame = 0;
        };
        State* g = nullptr;

        bool IsLinearLight(DXGI_FORMAT format) {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
            case DXGI_FORMAT_R11G11B10_FLOAT:
                return true;
            default:
                return false;
            }
        }

        // ---- the functions we stand in front of ---------------------------------------------------------------
        XrResult XRAPI_CALL OnDestroyInstance(XrInstance instance) {
            Log("xrDestroyInstance");
            const PFN_xrDestroyInstance destroy = g->next.xrDestroyInstance;
            g->pipeline.stop();
            g->marker.stop();
            delete g;
            g = nullptr;
            return destroy(instance);
        }

        XrResult XRAPI_CALL OnCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
            ID3D11Device* device = nullptr;
            for (auto entry = static_cast<const XrBaseInStructure*>(createInfo ? createInfo->next : nullptr); entry; entry = entry->next) {
                if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry)->device;
                } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR || entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
                           entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                    Log("session: this game does not use Direct3D 11 (binding type %d) - not supported yet, no mirror", int(entry->type));
                }
            }
            const XrResult result = g->next.xrCreateSession(instance, createInfo, session);
            if (XR_FAILED(result) || !device) return result;

            if (g->session != XR_NULL_HANDLE) Log("session: a second session while one is mirrored - the new one takes over");
            g->session = *session;
            g->logFrame = 0;
            g->pipelineReady = g->pipeline.start(device, ProducerOpenXR, g->program.c_str(), g->application.c_str());
            g->marker.start(device);
            g->gaze.onSessionCreated(*session, createInfo->systemId);
            Log("session: created (Direct3D 11), mirror %s", g->pipelineReady ? "ready" : "NOT available");
            return result;
        }

        XrResult XRAPI_CALL OnBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
            if (session == g->session && beginInfo) {
                g->viewConfiguration = beginInfo->primaryViewConfigurationType;
                Log("session: begins with view configuration %d", int(g->viewConfiguration));
            }
            return g->nextBeginSession(session, beginInfo);
        }

        XrResult XRAPI_CALL OnDestroySession(XrSession session) {
            if (session == g->session) {
                Log("session: destroyed");
                g->pipeline.stop(); // Before the runtime frees its images: we hold views of them.
                g->marker.stop();
                g->pipelineReady = false;
                g->gaze.onSessionDestroyed();
                g->session = XR_NULL_HANDLE;
            }
            return g->next.xrDestroySession(session);
        }

        XrResult XRAPI_CALL OnCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
            const XrResult result = g->next.xrCreateSwapchain(session, createInfo, swapchain);
            if (XR_SUCCEEDED(result) && session == g->session && createInfo) {
                SwapchainState state;
                state.format = static_cast<DXGI_FORMAT>(createInfo->format);
                std::lock_guard<std::mutex> guard(g->lock);
                g->swapchains[*swapchain] = std::move(state);
            }
            return result;
        }

        XrResult XRAPI_CALL OnDestroySwapchain(XrSwapchain swapchain) {
            {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end()) {
                    g->pipeline.forget(found->second.images);
                    g->swapchains.erase(found);
                }
            }
            return g->next.xrDestroySwapchain(swapchain);
        }

        XrResult XRAPI_CALL OnEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
            const XrResult result = g->next.xrEnumerateSwapchainImages(swapchain, capacity, count, images);
            if (XR_SUCCEEDED(result) && capacity > 0 && images && count && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end()) {
                    const auto* d3dImages = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
                    found->second.images.clear();
                    for (uint32_t i = 0; i < *count && i < capacity; i++) found->second.images.push_back(d3dImages[i].texture);
                }
            }
            return result;
        }

        XrResult XRAPI_CALL OnAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
            const XrResult result = g->next.xrAcquireSwapchainImage(swapchain, info, index);
            if (XR_SUCCEEDED(result) && index) {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end()) found->second.acquired.push_back(*index);
            }
            return result;
        }

        XrResult XRAPI_CALL OnReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
            const XrResult result = g->next.xrReleaseSwapchainImage(swapchain, info);
            if (XR_SUCCEEDED(result)) {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end() && !found->second.acquired.empty()) {
                    found->second.lastReleased = static_cast<int>(found->second.acquired.front());
                    found->second.acquired.pop_front();
                }
            }
            return result;
        }

        // The eye's image as the game submitted it, or false.
        bool ImageOf(const XrCompositionLayerProjectionView& view, SourceImage& image) {
            std::lock_guard<std::mutex> guard(g->lock);
            const auto found = g->swapchains.find(view.subImage.swapchain);
            if (found == g->swapchains.end() || found->second.lastReleased < 0 || size_t(found->second.lastReleased) >= found->second.images.size()) return false;
            image.texture = found->second.images[found->second.lastReleased];
            image.viewFormat = found->second.format;
            image.linearLight = IsLinearLight(found->second.format);
            image.arraySlice = view.subImage.imageArrayIndex;
            image.x = view.subImage.imageRect.offset.x;
            image.y = view.subImage.imageRect.offset.y;
            image.width = view.subImage.imageRect.extent.width;
            image.height = view.subImage.imageRect.extent.height;
            return image.width >= 64 && image.height >= 64;
        }

        void MirrorFrame(const XrFrameEndInfo* frame) {
            // The first projection layer is the world; its views[0] and [1] are the eyes.
            const XrCompositionLayerProjection* projection = nullptr;
            for (uint32_t i = 0; i < frame->layerCount && !projection; i++) {
                if (frame->layers[i] && frame->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    projection = reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[i]);
                }
            }
            if (!projection || projection->viewCount < 2) return;

            FrameInput input;
            SourceImage images[2];
            for (int eye = 0; eye < 2; eye++) {
                const XrCompositionLayerProjectionView& view = projection->views[eye];
                if (ImageOf(view, images[eye])) input.images[eye] = &images[eye];
                EyeView& v = input.views[eye];
                v.width = float(view.subImage.imageRect.extent.width);
                v.height = float(view.subImage.imageRect.extent.height);
                v.fov = {tanf(view.fov.angleLeft), tanf(view.fov.angleRight), tanf(view.fov.angleUp), tanf(view.fov.angleDown)};
                v.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w};
                v.valid = input.images[eye] != nullptr;
            }
            if (!input.images[0] && !input.images[1]) {
                LogFewTimes(g->logFrame, 3, "frame: the eyes' swapchains are unknown or have no released image yet");
                return;
            }

            // Where the eyes sit in the head (some headsets' displays are turned outwards), and where the gaze points.
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = g->viewConfiguration;
            locate.displayTime = frame->displayTime;
            locate.space = g->gaze.viewSpace();
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            XrView views[4] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t viewCount = 0;
            if (locate.space != XR_NULL_HANDLE && XR_SUCCEEDED(g->next.xrLocateViews(g->session, &locate, &viewState, 4, &viewCount, views)) &&
                viewCount >= 2 && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                for (int eye = 0; eye < 2; eye++) {
                    input.eyeInHead[eye].position = {views[eye].pose.position.x, views[eye].pose.position.y, views[eye].pose.position.z};
                    input.eyeInHead[eye].orientation = {views[eye].pose.orientation.x, views[eye].pose.orientation.y, views[eye].pose.orientation.z,
                                                        views[eye].pose.orientation.w};
                }
                const GazeSample gaze = g->gaze.locate(frame->displayTime);
                if (gaze.valid) {
                    input.gaze.valid = true;
                    input.gaze.hasRay = true;
                    input.gaze.origin = {gaze.pose.position.x, gaze.pose.position.y, gaze.pose.position.z};
                    const Quat q{gaze.pose.orientation.x, gaze.pose.orientation.y, gaze.pose.orientation.z, gaze.pose.orientation.w};
                    input.gaze.direction = Rotate(q, {0, 0, -1});
                }
            }

            const FrameOutput output = g->pipeline.frame(input);
            if (output.marker) {
                for (int eye = 0; eye < 2; eye++) {
                    if (input.images[eye]) g->marker.draw(*input.images[eye], output.markerNdc[eye], output.markerRadius, output.markerColor);
                }
            }
            if (output.countdownSeconds > 0) {
                for (int eye = 0; eye < 2; eye++) {
                    if (input.images[eye]) g->marker.drawCountdown(*input.images[eye], output.countdownSeconds);
                }
            }
        }

        XrResult XRAPI_CALL OnEndFrame(XrSession session, const XrFrameEndInfo* frame) {
            if (session == g->session && g->pipelineReady && frame && g->pipeline.wanted()) MirrorFrame(frame);
            return g->next.xrEndFrame(session, frame);
        }

        XrResult XRAPI_CALL OnSuggestBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
            return g->gaze.suggestBindings(instance, suggested);
        }
        XrResult XRAPI_CALL OnAttachActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
            return g->gaze.attachActionSets(session, info);
        }
        XrResult XRAPI_CALL OnSyncActions(XrSession session, const XrActionsSyncInfo* info) {
            return g->gaze.syncActions(session, info);
        }

        XrResult XRAPI_CALL OnGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

        struct Override {
            const char* name;
            PFN_xrVoidFunction function;
        };
        const Override Overrides[] = {
            {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(OnGetInstanceProcAddr)},
            {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(OnDestroyInstance)},
            {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(OnCreateSession)},
            {"xrBeginSession", reinterpret_cast<PFN_xrVoidFunction>(OnBeginSession)},
            {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(OnDestroySession)},
            {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(OnCreateSwapchain)},
            {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(OnDestroySwapchain)},
            {"xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction>(OnEnumerateSwapchainImages)},
            {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(OnAcquireSwapchainImage)},
            {"xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(OnReleaseSwapchainImage)},
            {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(OnEndFrame)},
            {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(OnSuggestBindings)},
            {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(OnAttachActionSets)},
            {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(OnSyncActions)},
        };

        XrResult XRAPI_CALL OnGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
            if (!g || !name || !function) return XR_ERROR_HANDLE_INVALID;
            for (const Override& entry : Overrides) {
                if (strcmp(name, entry.name) == 0) {
                    *function = entry.function;
                    return XR_SUCCESS;
                }
            }
            return g->next.getInstanceProcAddr(instance, name, function);
        }

        template <typename Function>
        bool Resolve(State& state, const char* name, Function& target) {
            const XrResult result = state.next.getInstanceProcAddr(state.next.instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&target));
            if (XR_FAILED(result) || !target) {
                Log("instance: %s is not available (%d)", name, result);
                return false;
            }
            return true;
        }

        std::string ProgramName() {
            wchar_t path[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring name(path, length);
            const size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name.erase(0, slash + 1);
            char utf8[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
            return utf8;
        }

        XrResult XRAPI_CALL CreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
            if (!info || !instance || !layerInfo || layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
                !layerInfo->nextInfo || layerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
                strcmp(layerInfo->nextInfo->layerName, LayerName) != 0 || !layerInfo->nextInfo->nextGetInstanceProcAddr ||
                !layerInfo->nextInfo->nextCreateApiLayerInstance) {
                Log("instance: the loader handed over something unexpected - layer not started");
                return XR_ERROR_INITIALIZATION_FAILED;
            }

            // Ask for the eye tracker, unless the game (or a layer above us) already did.
            std::vector<const char*> extensions(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
            bool haveEyeGaze = false;
            for (const char* extension : extensions) {
                if (strcmp(extension, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) haveEyeGaze = true;
            }
            const bool added = !haveEyeGaze;
            if (added) extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
            XrInstanceCreateInfo chainInfo = *info;
            chainInfo.enabledExtensionNames = extensions.data();
            chainInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            XrApiLayerCreateInfo chainLayerInfo = *layerInfo;
            chainLayerInfo.nextInfo = layerInfo->nextInfo->next;

            XrResult result = layerInfo->nextInfo->nextCreateApiLayerInstance(&chainInfo, &chainLayerInfo, instance);
            bool eyeGaze = XR_SUCCEEDED(result);
            if (XR_FAILED(result) && added) {
                // No eye tracker in this runtime: start the game exactly as it asked.
                Log("instance: with the eye-gaze extension the runtime said %d - trying without", result);
                chainLayerInfo = *layerInfo;
                chainLayerInfo.nextInfo = layerInfo->nextInfo->next;
                result = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chainLayerInfo, instance);
                eyeGaze = false;
            }
            if (XR_FAILED(result)) return result;

            auto state = std::make_unique<State>();
            state->next.instance = *instance;
            state->next.getInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
            Next& next = state->next;
            const bool complete =
                Resolve(*state, "xrDestroyInstance", next.xrDestroyInstance) & Resolve(*state, "xrGetInstanceProperties", next.xrGetInstanceProperties) &
                Resolve(*state, "xrGetSystemProperties", next.xrGetSystemProperties) & Resolve(*state, "xrCreateSession", next.xrCreateSession) &
                Resolve(*state, "xrBeginSession", state->nextBeginSession) & Resolve(*state, "xrDestroySession", next.xrDestroySession) &
                Resolve(*state, "xrCreateSwapchain", next.xrCreateSwapchain) & Resolve(*state, "xrDestroySwapchain", next.xrDestroySwapchain) &
                Resolve(*state, "xrEnumerateSwapchainImages", next.xrEnumerateSwapchainImages) &
                Resolve(*state, "xrAcquireSwapchainImage", next.xrAcquireSwapchainImage) &
                Resolve(*state, "xrReleaseSwapchainImage", next.xrReleaseSwapchainImage) & Resolve(*state, "xrEndFrame", next.xrEndFrame) &
                Resolve(*state, "xrCreateReferenceSpace", next.xrCreateReferenceSpace) & Resolve(*state, "xrDestroySpace", next.xrDestroySpace) &
                Resolve(*state, "xrLocateSpace", next.xrLocateSpace) & Resolve(*state, "xrLocateViews", next.xrLocateViews) &
                Resolve(*state, "xrStringToPath", next.xrStringToPath) & Resolve(*state, "xrCreateActionSet", next.xrCreateActionSet) &
                Resolve(*state, "xrDestroyActionSet", next.xrDestroyActionSet) & Resolve(*state, "xrCreateAction", next.xrCreateAction) &
                Resolve(*state, "xrCreateActionSpace", next.xrCreateActionSpace) &
                Resolve(*state, "xrSuggestInteractionProfileBindings", next.xrSuggestInteractionProfileBindings) &
                Resolve(*state, "xrAttachSessionActionSets", next.xrAttachSessionActionSets) & Resolve(*state, "xrSyncActions", next.xrSyncActions);
            if (!complete) {
                Log("instance: the runtime lacks functions this layer needs - stepping aside");
                if (next.xrDestroyInstance) next.xrDestroyInstance(*instance);
                return XR_ERROR_INITIALIZATION_FAILED;
            }

            state->program = ProgramName();
            state->application = info->applicationInfo.applicationName;
            XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
            next.xrGetInstanceProperties(*instance, &properties);
            Log("instance: %s (\"%s\") on runtime \"%s\" %u.%u.%u", state->program.c_str(), state->application.c_str(), properties.runtimeName,
                XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
            for (auto other = layerInfo->nextInfo->next; other; other = other->next) Log("instance: below us: %s", other->layerName);

            delete g;
            g = state.release();
            g->gaze.onInstance(&g->next, eyeGaze);
            return XR_SUCCESS;
        }
    } // namespace

} // namespace gaze_mirror::layer

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                                         const char* layerName,
                                                                                         XrNegotiateApiLayerRequest* request) {
    using namespace gaze_mirror::layer;
    gaze_mirror::SetLogName(L"gaze-mirror-layer");
    if (!loaderInfo || !layerName || !request || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST || request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest) || strcmp(layerName, LayerName) != 0 ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION || loaderInfo->minApiVersion > XR_CURRENT_API_VERSION ||
        loaderInfo->maxApiVersion < XR_API_VERSION_1_0) {
        Log("negotiate: refused (unexpected structures, name or versions)");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(OnGetInstanceProcAddr);
    request->createApiLayerInstance = reinterpret_cast<PFN_xrCreateApiLayerInstance>(CreateApiLayerInstance);
    return XR_SUCCESS;
}
