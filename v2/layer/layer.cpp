// VR Gaze Mirror 2.0 - the OpenXR API layer.
//
// Written from nothing but the OpenXR headers: the loader hands us the chain, we put ourselves in front of a dozen
// functions and pass everything else straight through. The layer itself only
//   - copies the eye images the mirror reads at the moment the game hands them back (xrReleaseSwapchainImage): OpenXR
//     lets nobody touch an image after that, and some runtimes (Varjo's) take it back at once. Only what is needed is
//     copied: the mirrored eye, the other one too while a snapshot waits, and nothing while nobody reads,
//   - shares the eye tracker with the game and other layers (GazeInput),
//   - at xrEndFrame, hands the core (v2\core) both eyes' images, the head pose and the gaze; the core does the rest,
//   - shows what the settings app asks the headset to show (the calibration marker, the capture countdown, the edge
//     finder) as a see-through layer of its own over the game's frame. The game's images are never drawn into.
// It sits BELOW Quad-Views-Foveated (closer to the runtime), so what it sees is the finished stereo image.
//
// Direct3D 11 games are read as they are. Direct3D 12 games go through Direct3D 11-on-12: a D3D11 device is made on top
// of the game's D3D12 device and queue, every swapchain image (the game's and the overlay's) is wrapped as a D3D11
// texture, and the core, the marker and the readers see nothing different. The wrapped images are acquired around the copy at their hand-back and
// released after it, which is where 11-on-12 puts the resource transitions and orders our work behind the game's.

#include "pch.h"

#include "../core/log.h"
#include "../core/pipeline.h"
#include "gaze_input.h"
#include "marker.h"
#include "panels.h"
#include "next.h"

namespace gaze_mirror::layer {

    namespace {
        constexpr char LayerName[] = "XR_APILAYER_NOVENDOR_gaze_mirror";

        struct SwapchainState {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<ID3D11Texture2D*> images;                             // The game's (D3D11) or the wrapped ones (D3D12).
            std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> wrapped;    // D3D12: what keeps the wrapped images alive.
            std::deque<uint32_t> acquired; // Images are released in the order they were acquired.
            int lastReleased = -1;
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO}; // As the game asked (next cleared).
            // What the mirror reads: the image the game handed back last, copied at its hand-back, on the mirror's device.
            Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
            int copiedIndex = -1;
            // The slices the mirror reads, from the last mirrored frame's layout (bit per array slice); until a layout is
            // known every slice is copied.
            bool layoutKnown = false;
            uint32_t neededSlices = 0;
            // The part of the image the mirror reads (the views' rectangles, only ever grown) and what the copy holds.
            D3D11_BOX neededBox{UINT_MAX, UINT_MAX, 0, 0, 0, 1};
            D3D11_BOX copiedBox{};
            // Another tool's flat panel (a quad layer after the world) shows this swapchain's picture.
            bool isPanel = false;
            uint32_t panelsDrawnSlices = 0; // The copy's slices panels were drawn into (a frame the game repeats has them).
            uint64_t lastReleaseFrame = 0;  // The frame count at its last hand-back (0 = none yet).
            bool loggedCopy = false;
        };

        // The aids shown in the headset - the calibration marker, the capture countdown, the edge finder - while one is up:
        // a projection layer added after the game's, with the game's views, poses and fields of view, one swapchain per
        // view the size of the game's view, see-through (premultiplied alpha) except where an aid is drawn.
        struct OverlayView {
            XrSwapchain swapchain = XR_NULL_HANDLE;
            int32_t width = 0, height = 0;         // The overlay's images.
            int32_t viewWidth = 0, viewHeight = 0; // The game's view they lie over.
            std::vector<ID3D11Texture2D*> images;
            std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> wrapped; // D3D12: the images wrapped for 11-on-12.
        };
        struct Overlay {
            OverlayView views[4];
            uint32_t viewCount = 0;
            int64_t format = 0; // Chosen once a session: 0 not yet, -1 the runtime offers nothing with alpha.
            XrCompositionLayerProjectionView layerViews[4];
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            bool ready = false; // Drawn for this frame: it goes along with it.
        };

        struct State {
            Next next;
            std::string program;
            std::string application;

            std::mutex lock; // Guards the swapchain table; the rest is only touched by the thread that ends frames.
            std::unordered_map<XrSwapchain, SwapchainState> swapchains;

            XrSession session = XR_NULL_HANDLE; // The session we mirror, if any.
            // Direct3D 12 games: the 11-on-12 device the core works on, and its immediate context (for the flush).
            Microsoft::WRL::ComPtr<ID3D11On12Device> on12;
            Microsoft::WRL::ComPtr<ID3D11Device> device11;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate11;
            XrViewConfigurationType viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            bool pipelineReady = false;
            // The game runs on SteamVR's OpenXR runtime: the SteamVR helper gives the picture (with SteamVR's overlays)
            // unless picture_source=game; meanwhile this layer stands by and looks every 90 frames whether to publish.
            bool underSteamVR = false;
            ID3D11Device* mirrorDevice = nullptr;
            int standByFrames = 0;
            Pipeline pipeline;
            Marker marker;
            Panels panels;
            int logPanels = 0;
            int logOtherLayers = 0;
            size_t lastPanelCount = 0;
            uint64_t frameCount = 0; // The session's frames, for how often a panel is handed back.
            GazeInput gaze;
            PFN_xrBeginSession nextBeginSession = nullptr;
            int logFrame = 0;
            int logQuad = 0;
            int logCopy = 0;
            int logLayout = 0;
            // Copies at hand-back are made while anything needs the images (a reader, a snapshot, an aid in the headset);
            // decided at the last xrEndFrame, since a release cannot ask the core again.
            bool imagesNeeded = false;
            std::string lastLayout; // The last frame layout written to the log, to log changes only.
            // How long before the frame's end the game hands its first image back (the longest lately), and when it did
            // this frame: what the guess whether a frame gets through the frame-rate cap goes by.
            std::chrono::steady_clock::duration releaseLead{};
            std::chrono::steady_clock::time_point firstRelease{};
            bool loggedCapSkip = false;
            uint32_t maxLayerCount = 0;
            Overlay overlay;
            int logOverlay = 0;
            int32_t nominalWidth = 0, nominalHeight = 0; // Quad views: the eye image's size at the inner image's sharpness.
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

        void DestroyOverlay();

        // The mirror's copy of a swapchain's image goes, with the renderer's view of it (which would keep it alive).
        // Called with the lock held.
        void ReleaseCopy(SwapchainState& state) {
            if (!state.copy) return;
            g->pipeline.forget({state.copy.Get()});
            g->panels.forget({state.copy.Get()});
            state.copy.Reset();
            state.copiedIndex = -1;
        }

        // ---- the functions we stand in front of ---------------------------------------------------------------
        XrResult XRAPI_CALL OnDestroyInstance(XrInstance instance) {
            Log("xrDestroyInstance");
            const PFN_xrDestroyInstance destroy = g->next.xrDestroyInstance;
            g->pipeline.stop();
            g->marker.stop();
            g->panels.stop();
            delete g;
            g = nullptr;
            return destroy(instance);
        }

        XrResult XRAPI_CALL OnCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
            ID3D11Device* device = nullptr;
            const char* api = "Direct3D 11";
            g->on12.Reset();
            g->device11.Reset();
            g->immediate11.Reset();
            for (auto entry = static_cast<const XrBaseInStructure*>(createInfo ? createInfo->next : nullptr); entry; entry = entry->next) {
                if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry)->device;
                } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                    const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                    IUnknown* queues[] = {binding->queue};
                    Microsoft::WRL::ComPtr<ID3D11Device> device11;
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate11;
                    const HRESULT made = D3D11On12CreateDevice(binding->device, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0,
                                                               &device11, &immediate11, nullptr);
                    if (SUCCEEDED(made) && SUCCEEDED(device11.As(&g->on12))) {
                        g->device11 = device11;
                        g->immediate11 = immediate11;
                        device = device11.Get();
                        api = "Direct3D 12, through 11-on-12";
                    } else {
                        Log("session: Direct3D 12 game, but no 11-on-12 device could be made (0x%08lx) - no mirror", static_cast<unsigned long>(made));
                    }
                } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR || entry->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                    Log("session: this game does not use Direct3D (binding type %d) - not supported yet, no mirror", int(entry->type));
                }
            }
            const XrResult result = g->next.xrCreateSession(instance, createInfo, session);
            if (XR_FAILED(result) || !device) return result;

            if (g->session != XR_NULL_HANDLE) Log("session: a second session while one is mirrored - the new one takes over");
            g->session = *session;
            g->logFrame = 0;
            g->logQuad = 0;
            g->logCopy = 0;
            g->logLayout = 0;
            g->imagesNeeded = false;
            g->lastLayout.clear();
            g->overlay.format = 0;
            g->logOverlay = 0;
            g->releaseLead = {};
            g->firstRelease = {};
            g->loggedCapSkip = false;
            XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
            if (XR_SUCCEEDED(g->next.xrGetSystemProperties(instance, createInfo->systemId, &system))) {
                g->maxLayerCount = system.graphicsProperties.maxLayerCount;
                Log("session: system \"%s\" (vendor %u), up to %u layers a frame, swapchain images up to %u x %u", system.systemName, system.vendorId,
                    system.graphicsProperties.maxLayerCount, system.graphicsProperties.maxSwapchainImageWidth, system.graphicsProperties.maxSwapchainImageHeight);
            }
            g->nominalWidth = g->nominalHeight = 0;
            g->mirrorDevice = device;
            g->standByFrames = 0;
            g->pipelineReady = g->pipeline.start(device, ProducerOpenXR, g->program.c_str(), g->application.c_str(), g->underSteamVR);
            g->marker.start(device);
            g->panels.start(device);
            g->lastPanelCount = 0;
            g->logPanels = 0;
            g->logOtherLayers = 0;
            g->frameCount = 0;
            g->gaze.onSessionCreated(*session, createInfo->systemId);
            Log("session: created (%s), mirror %s", api, g->pipelineReady ? "ready" : "NOT available");
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
                DestroyOverlay(); // Its swapchains belong to the session.
                g->pipeline.stop(); // Before the runtime frees its images: we hold views of them.
                g->marker.stop();
                g->panels.stop();
                g->pipelineReady = false;
                g->mirrorDevice = nullptr;
                {
                    std::lock_guard<std::mutex> guard(g->lock);
                    for (auto& entry : g->swapchains) entry.second.wrapped.clear();
                }
                if (g->on12 && g->immediate11) {
                    g->immediate11->ClearState();
                    g->immediate11->Flush();
                }
                g->on12.Reset();
                g->immediate11.Reset();
                g->device11.Reset();
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
                state.info = *createInfo;
                state.info.next = nullptr;
                Log("swapchain %p: %u x %u, %u slice(s), %u sample(s), %u mip(s), format %lld, usage 0x%llx, flags 0x%llx", static_cast<void*>(*swapchain),
                    createInfo->width, createInfo->height, createInfo->arraySize, createInfo->sampleCount, createInfo->mipCount,
                    static_cast<long long>(createInfo->format), static_cast<unsigned long long>(createInfo->usageFlags),
                    static_cast<unsigned long long>(createInfo->createFlags));
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
                    g->marker.forget(found->second.images);
                    ReleaseCopy(found->second);
                    g->swapchains.erase(found);
                    // Direct3D 12: the wrapped images are only really let go - and with them the game's own - once the
                    // 11-on-12 context has dropped what it still has bound and flushed (it is ours alone, never the game's).
                    if (g->on12 && g->immediate11) {
                        g->immediate11->ClearState();
                        g->immediate11->Flush();
                    }
                }
            }
            return g->next.xrDestroySwapchain(swapchain);
        }

        // A D3D12 image as a D3D11 texture on the 11-on-12 device. Swapchain images are handed out and back in the
        // render-target state (the OpenXR rule for D3D12 swapchains), so that is the state in and out. A depth or otherwise
        // non-renderable image is wrapped read-only (it is never drawn into anyway).
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Wrap(ID3D12Resource* texture) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> wrapped;
            if (!texture) return wrapped;
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (SUCCEEDED(g->on12->CreateWrappedResource(texture, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, IID_PPV_ARGS(&wrapped)))) {
                return wrapped;
            }
            flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g->on12->CreateWrappedResource(texture, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, IID_PPV_ARGS(&wrapped)))) {
                wrapped.Reset();
            }
            return wrapped;
        }

        // What the runtime's images are: for the log, so that a runtime that hands out something unusual shows it.
        void LogImages(XrSwapchain swapchain, ID3D11Texture2D* texture, uint32_t count) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed;
            const bool lock = SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&keyed)));
            const char* shared = (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)                                     ? ", shared with a keyed mutex"
                                 : (desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) ? ", shared"
                                                                                                                          : "";
            Log("swapchain %p: %u Direct3D 11 images, %u x %u x %u, format %d, %u sample(s), bind 0x%x, misc 0x%x%s%s", static_cast<void*>(swapchain), count,
                desc.Width, desc.Height, desc.ArraySize, int(desc.Format), desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags, shared,
                lock ? ", has a keyed mutex" : "");
        }
        void LogImages12(XrSwapchain swapchain, ID3D12Resource* resource, uint32_t count) {
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            D3D12_HEAP_PROPERTIES heap{};
            D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
            const bool placed = SUCCEEDED(resource->GetHeapProperties(&heap, &heapFlags));
            Log("swapchain %p: %u Direct3D 12 images, %llu x %u x %u, format %d, %u sample(s), flags 0x%x, heap %s0x%x%s", static_cast<void*>(swapchain), count,
                static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize, int(desc.Format), desc.SampleDesc.Count, unsigned(desc.Flags),
                placed ? "flags " : "unknown ", unsigned(heapFlags), (heapFlags & D3D12_HEAP_FLAG_SHARED) ? ", shared" : "");
        }

        XrResult XRAPI_CALL OnEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
            const XrResult result = g->next.xrEnumerateSwapchainImages(swapchain, capacity, count, images);
            if (XR_FAILED(result) || capacity == 0 || !images || !count) return result;
            if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end()) {
                    const auto* d3dImages = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
                    found->second.images.clear();
                    for (uint32_t i = 0; i < *count && i < capacity; i++) found->second.images.push_back(d3dImages[i].texture);
                    if (!found->second.images.empty() && found->second.images[0]) LogImages(swapchain, found->second.images[0], *count);
                }
            } else if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR && g->on12) {
                // Each D3D12 image becomes a D3D11 texture on the 11-on-12 device.
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end()) {
                    const auto* d3dImages = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(images);
                    if (*count > 0 && d3dImages[0].texture) LogImages12(swapchain, d3dImages[0].texture, *count);
                    found->second.images.clear();
                    found->second.wrapped.clear();
                    int failed = 0;
                    for (uint32_t i = 0; i < *count && i < capacity; i++) {
                        const Microsoft::WRL::ComPtr<ID3D11Texture2D> wrapped = Wrap(d3dImages[i].texture);
                        if (!wrapped) failed++;
                        found->second.images.push_back(wrapped.Get());
                        found->second.wrapped.push_back(wrapped);
                    }
                    if (failed) Log("swapchain: %d of %u Direct3D 12 images could not be wrapped for 11-on-12 - those frames are skipped", failed, *count);
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

        // The mirror's copy of an image the game is handing back: made now, while the image is still the game's. Only the
        // slices the mirror reads (all of them until a frame has shown which).
        void CopyAtHandBack(SwapchainState& state, uint32_t index) {
            if (index >= state.images.size() || !state.images[index] || !g->mirrorDevice) return;
            if (state.info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) return; // Never mirrored.
            ID3D11Texture2D* source = state.images[index];
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            const uint32_t all = desc.ArraySize >= 32 ? ~0u : (1u << desc.ArraySize) - 1;
            const uint32_t slices = state.layoutKnown ? (state.neededSlices & all) : all;
            if (!slices) return; // The mirror reads nothing of this swapchain (the other eye's, say).
            // Only the views' rectangle once the layout is known (a multisampled image only whole).
            D3D11_BOX box{0, 0, 0, desc.Width, desc.Height, 1};
            const D3D11_BOX& needed = state.neededBox;
            if (state.layoutKnown && desc.SampleDesc.Count == 1 && needed.left < needed.right && needed.top < needed.bottom) {
                box.left = std::min(needed.left, desc.Width);
                box.top = std::min(needed.top, desc.Height);
                box.right = std::min(needed.right, desc.Width);
                box.bottom = std::min(needed.bottom, desc.Height);
            }
            if (box.left >= box.right || box.top >= box.bottom) return;
            const bool whole = box.left == 0 && box.top == 0 && box.right == desc.Width && box.bottom == desc.Height;
            if (!state.copy) {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.MiscFlags = 0;
                HRESULT made = g->mirrorDevice->CreateTexture2D(&copyDesc, nullptr, &state.copy);
                if (FAILED(made)) {
                    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    made = g->mirrorDevice->CreateTexture2D(&copyDesc, nullptr, &state.copy);
                }
                if (FAILED(made)) {
                    LogFewTimes(g->logCopy, 5, "copy: no texture to copy a %u x %u image (format %d) into (0x%08lx) - that swapchain is not mirrored", desc.Width,
                                desc.Height, int(desc.Format), static_cast<unsigned long>(made));
                    return;
                }
            }
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            g->mirrorDevice->GetImmediateContext(&context);
            ID3D11Resource* wrapped = source;
            if (g->on12) g->on12->AcquireWrappedResources(&wrapped, 1);
            for (uint32_t slice = 0; slice < desc.ArraySize && slice < 32; slice++) {
                if (!(slices & (1u << slice))) continue;
                for (uint32_t mip = 0; mip < desc.MipLevels; mip++) {
                    const UINT subresource = D3D11CalcSubresource(mip, slice, desc.MipLevels);
                    if (mip == 0 && !whole) context->CopySubresourceRegion(state.copy.Get(), subresource, box.left, box.top, 0, source, subresource, &box);
                    else context->CopySubresourceRegion(state.copy.Get(), subresource, 0, 0, 0, source, subresource, nullptr);
                }
            }
            if (g->on12) {
                g->on12->ReleaseWrappedResources(&wrapped, 1);
                context->Flush();
            }
            state.copiedIndex = static_cast<int>(index);
            state.copiedBox = box;
            state.panelsDrawnSlices = 0;
            if (!state.loggedCopy) {
                state.loggedCopy = true;
                Log("copy: images of this swapchain are copied at their hand-back (slices 0x%x of %u, %u,%u to %u,%u of %u x %u)", slices, desc.ArraySize, box.left,
                    box.top, box.right, box.bottom, desc.Width, desc.Height);
            }
        }

        // Whether the image being handed back is worth copying: for a snapshot or an aid in the headset always; for a reader
        // only if the frame gets through the frame-rate cap. The frame's end decides that, after the hand-back: the guess
        // takes the frame to end as long after this as the game's hand-backs have come before their frame's end lately.
        bool WantCopy() {
            const auto now = std::chrono::steady_clock::now();
            if (g->firstRelease.time_since_epoch().count() == 0) g->firstRelease = now;
            if (g->pipeline.snapshotDue() || g->pipeline.shownInHeadset()) return true;
            if (!g->imagesNeeded && !g->pipeline.readerLive()) return false;
            if (g->pipeline.renderDueAt(now + g->releaseLead + std::chrono::milliseconds(1))) return true;
            if (!g->loggedCapSkip) {
                g->loggedCapSkip = true;
                Log("copy: the frames the frame-rate cap leaves out are not copied");
            }
            return false;
        }

        XrResult XRAPI_CALL OnReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
            if (g->pipelineReady) {
                std::lock_guard<std::mutex> guard(g->lock);
                const auto found = g->swapchains.find(swapchain);
                if (found != g->swapchains.end() && !found->second.acquired.empty()) {
                    // A panel is handed back when its picture changes, maybe seldom, and then shows that picture for as
                    // long as it likes: copied whenever anybody watches, whatever the frame-rate cap - and, if it is not
                    // handed back every frame or two, even while nobody watches, so the copy is there when somebody starts.
                    SwapchainState& state = found->second;
                    const bool seldom = state.lastReleaseFrame == 0 || g->frameCount - state.lastReleaseFrame > 2;
                    const bool wanted = state.isPanel ? (seldom || g->imagesNeeded || g->pipeline.readerLive()) : WantCopy();
                    state.lastReleaseFrame = g->frameCount;
                    if (wanted) CopyAtHandBack(state, state.acquired.front());
                }
            }
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

        // A picture as the game (or a tool) submitted it - the copy made at its hand-back - or false: an eye's image, or a
        // panel's. `expected`: a missing copy is worth a line in the log (the other eye is only copied while a snapshot
        // waits for it).
        bool ImageOf(const XrSwapchainSubImage& sub, SourceImage& image, bool expected) {
            std::lock_guard<std::mutex> guard(g->lock);
            const auto found = g->swapchains.find(sub.swapchain);
            if (found == g->swapchains.end() || found->second.lastReleased < 0 || size_t(found->second.lastReleased) >= found->second.images.size()) return false;
            const SwapchainState& state = found->second;
            const uint32_t slice = sub.imageArrayIndex;
            const bool sliceCopied = !state.layoutKnown || (slice < 32 && (state.neededSlices & (1u << slice)));
            const XrRect2Di& rect = sub.imageRect;
            const D3D11_BOX& box = state.copiedBox;
            const bool rectCopied = rect.offset.x >= 0 && rect.offset.y >= 0 && UINT(rect.offset.x) >= box.left && UINT(rect.offset.y) >= box.top &&
                                    UINT(rect.offset.x + rect.extent.width) <= box.right && UINT(rect.offset.y + rect.extent.height) <= box.bottom;
            if (!state.copy || state.copiedIndex != state.lastReleased || !sliceCopied || !rectCopied) {
                if (expected) {
                    LogFewTimes(g->logCopy, 5, "frame: the image handed back (swapchain %p, image %d, slice %u) was not copied at its hand-back - not mirrored this frame",
                                static_cast<void*>(sub.swapchain), state.lastReleased, slice);
                }
                return false;
            }
            image.texture = state.copy.Get();
            image.viewFormat = found->second.format;
            image.linearLight = IsLinearLight(found->second.format);
            image.arraySlice = sub.imageArrayIndex;
            image.x = sub.imageRect.offset.x;
            image.y = sub.imageRect.offset.y;
            image.width = sub.imageRect.extent.width;
            image.height = sub.imageRect.extent.height;
            return image.width >= 64 && image.height >= 64;
        }

        // The frame's layout, each time it changes: the layers, and for the world's projection where each view's picture lies.
        void LogLayout(const XrFrameEndInfo* frame, const XrCompositionLayerProjection* projection) {
            char line[1024];
            int at = snprintf(line, sizeof(line), "%u layer(s):", frame->layerCount);
            for (uint32_t i = 0; i < frame->layerCount && at < int(sizeof(line)) - 64; i++) {
                at += snprintf(line + at, sizeof(line) - at, " %d", frame->layers[i] ? int(frame->layers[i]->type) : 0);
            }
            for (uint32_t i = 0; i < projection->viewCount && i < 4 && at < int(sizeof(line)) - 160; i++) {
                const XrCompositionLayerProjectionView& view = projection->views[i];
                at += snprintf(line + at, sizeof(line) - at, "; view %u: swapchain %p slice %u at %d,%d %dx%d, fov %.1f %.1f %.1f %.1f", i,
                               static_cast<void*>(view.subImage.swapchain), view.subImage.imageArrayIndex, view.subImage.imageRect.offset.x,
                               view.subImage.imageRect.offset.y, view.subImage.imageRect.extent.width, view.subImage.imageRect.extent.height,
                               view.fov.angleLeft * 57.29578f, view.fov.angleRight * 57.29578f, view.fov.angleUp * 57.29578f, view.fov.angleDown * 57.29578f);
            }
            if (g->lastLayout != line) {
                g->lastLayout = line;
                LogFewTimes(g->logLayout, 10, "frame: layout %s", line);
            }
        }

        void DestroyOverlay() {
            Overlay& o = g->overlay;
            if (o.viewCount == 0) return;
            for (uint32_t i = 0; i < o.viewCount; i++) {
                g->marker.forget(o.views[i].images);
                o.views[i].images.clear();
                o.views[i].wrapped.clear();
            }
            if (g->on12 && g->immediate11) { // Let go of the wrapped images for real (see OnDestroySwapchain).
                g->immediate11->ClearState();
                g->immediate11->Flush();
            }
            for (uint32_t i = 0; i < o.viewCount; i++) {
                if (o.views[i].swapchain != XR_NULL_HANDLE) g->next.xrDestroySwapchain(o.views[i].swapchain);
                o.views[i] = OverlayView{};
            }
            o.viewCount = 0;
            Log("overlay: gone");
        }

        // The overlay's format: one with alpha the runtime offers, sRGB first.
        int64_t OverlayFormat() {
            Overlay& o = g->overlay;
            if (o.format != 0) return o.format;
            std::vector<int64_t> offered;
            uint32_t count = 0;
            if (XR_SUCCEEDED(g->next.xrEnumerateSwapchainFormats(g->session, 0, &count, nullptr)) && count > 0 && count < 1024) {
                offered.resize(count);
                if (XR_FAILED(g->next.xrEnumerateSwapchainFormats(g->session, count, &count, offered.data()))) count = 0;
                offered.resize(std::min<size_t>(count, offered.size()));
            }
            const int64_t wanted[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT};
            o.format = -1;
            for (const int64_t format : wanted) {
                if (std::find(offered.begin(), offered.end(), format) != offered.end()) {
                    o.format = format;
                    break;
                }
            }
            if (o.format < 0) Log("overlay: none of the runtime's %zu swapchain formats has alpha - nothing can be shown in the headset", offered.size());
            return o.format;
        }

        // One swapchain per view of the game's projection, each the size of its view - half of it from 2000 pixels tall up
        // (the runtime scales the layer to the view, and the aids are bold) - made again when the views' sizes change.
        bool MakeOverlay(const XrCompositionLayerProjection* projection) {
            Overlay& o = g->overlay;
            const uint32_t viewCount = std::min(projection->viewCount, 4u);
            bool same = o.viewCount == viewCount;
            for (uint32_t i = 0; i < viewCount && same; i++) {
                same = o.views[i].viewWidth == projection->views[i].subImage.imageRect.extent.width &&
                       o.views[i].viewHeight == projection->views[i].subImage.imageRect.extent.height;
            }
            if (same) return true;
            DestroyOverlay();
            const int64_t format = OverlayFormat();
            if (format < 0) return false;
            for (uint32_t i = 0; i < viewCount; i++) {
                OverlayView& view = o.views[i];
                o.viewCount = i + 1; // What DestroyOverlay cleans up if this one fails.
                const XrExtent2Di extent = projection->views[i].subImage.imageRect.extent;
                const int32_t divide = extent.height >= 2000 ? 2 : 1;
                XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                info.format = format;
                info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
                info.width = uint32_t(std::max(extent.width / divide, 1));
                info.height = uint32_t(std::max(extent.height / divide, 1));
                const XrResult made = g->next.xrCreateSwapchain(g->session, &info, &view.swapchain);
                uint32_t count = 0;
                if (XR_FAILED(made) || XR_FAILED(g->next.xrEnumerateSwapchainImages(view.swapchain, 0, &count, nullptr)) || count == 0) {
                    Log("overlay: no swapchain for view %u (%u x %u, format %lld): %d - nothing is shown in the headset", i, info.width, info.height,
                        static_cast<long long>(format), int(made));
                    if (XR_FAILED(made)) view.swapchain = XR_NULL_HANDLE;
                    DestroyOverlay();
                    return false;
                }
                view.width = int32_t(info.width);
                view.height = int32_t(info.height);
                view.viewWidth = extent.width;
                view.viewHeight = extent.height;
                if (g->on12) {
                    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                    if (XR_SUCCEEDED(g->next.xrEnumerateSwapchainImages(view.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
                        for (uint32_t k = 0; k < count; k++) {
                            view.wrapped.push_back(Wrap(images[k].texture));
                            view.images.push_back(view.wrapped.back().Get());
                        }
                    }
                } else {
                    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                    if (XR_SUCCEEDED(g->next.xrEnumerateSwapchainImages(view.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
                        for (uint32_t k = 0; k < count; k++) view.images.push_back(images[k].texture);
                    }
                }
                if (view.images.empty() || std::find(view.images.begin(), view.images.end(), nullptr) != view.images.end()) {
                    Log("overlay: the images of view %u's swapchain cannot be used - nothing is shown in the headset", i);
                    DestroyOverlay();
                    return false;
                }
            }
            Log("overlay: %u view(s), %d x %d for view 0 (its view %d x %d), format %lld, %zu images each", viewCount, o.views[0].width, o.views[0].height,
                o.views[0].viewWidth, o.views[0].viewHeight, static_cast<long long>(format), o.views[0].images.size());
            return true;
        }

        // Draws the aids into the overlay - each view's image cleared, then the edge finder, the marker, the countdown -
        // and readies its layer for this frame. With quad views the inner views get them too, through their window into
        // the wide view (the runtime shows the inner view where it lies, so the wide one alone would be hidden there).
        // The edge finder covers the view: the mirrored eye gets the blocks, as that is the image the crop applies to;
        // the other eye goes black, so nothing of the game pulls the eye meanwhile.
        void DrawOverlay(const XrFrameEndInfo* frame, const XrCompositionLayerProjection* projection, const FrameOutput& output, const float windows[2][4],
                         const bool haveWindow[2]) {
            if (g->maxLayerCount > 0 && frame->layerCount >= g->maxLayerCount) {
                LogFewTimes(g->logOverlay, 3, "overlay: the frame has %u layers, the most this runtime takes - nothing is shown in the headset", frame->layerCount);
                return;
            }
            if (!MakeOverlay(projection)) return;
            Overlay& o = g->overlay;
            for (uint32_t i = 0; i < o.viewCount; i++) {
                OverlayView& view = o.views[i];
                uint32_t index = 0;
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = XR_INFINITE_DURATION;
                XrResult result = g->next.xrAcquireSwapchainImage(view.swapchain, nullptr, &index);
                if (XR_SUCCEEDED(result)) result = g->next.xrWaitSwapchainImage(view.swapchain, &wait);
                if (XR_FAILED(result) || index >= view.images.size()) {
                    LogFewTimes(g->logOverlay, 3, "overlay: view %u's image could not be taken (%d) - nothing is shown in the headset this frame", i, int(result));
                    return;
                }
                SourceImage image;
                image.texture = view.images[index];
                image.viewFormat = static_cast<DXGI_FORMAT>(o.format);
                image.linearLight = IsLinearLight(image.viewFormat);
                image.width = view.width;
                image.height = view.height;
                const int eye = int(i % 2);
                const float* window = i < 2 ? nullptr : haveWindow[eye] ? windows[eye] : nullptr;
                const bool drawn = i < 2 || window; // An inner view without a usable window stays clear.
                ID3D11Resource* wrapped = image.texture;
                if (g->on12) g->on12->AcquireWrappedResources(&wrapped, 1);
                g->marker.clear(image);
                if (drawn && output.edgeBar > 0) g->marker.drawEdges(image, output.edges, eye == output.mirroredEye ? output.edgeBar : 0, window);
                if (drawn && output.marker) g->marker.draw(image, output.markerNdc[eye], output.markerRadius, output.markerColor, window);
                if (drawn && output.countdownSeconds > 0) g->marker.drawCountdown(image, output.countdownSeconds, output.countdownNdc[eye], output.countdownHeight[eye], window);
                if (g->on12) {
                    g->on12->ReleaseWrappedResources(&wrapped, 1);
                    g->immediate11->Flush(); // Down the game's queue before the runtime gets the image.
                }
                g->next.xrReleaseSwapchainImage(view.swapchain, nullptr);
                XrCompositionLayerProjectionView& layerView = o.layerViews[i];
                layerView = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                layerView.pose = projection->views[i].pose;
                layerView.fov = projection->views[i].fov;
                layerView.subImage.swapchain = view.swapchain;
                layerView.subImage.imageRect = {{0, 0}, {view.width, view.height}};
            }
            o.layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            o.layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // Premultiplied: no UNPREMULTIPLIED bit.
            o.layer.space = projection->space;
            o.layer.viewCount = o.viewCount;
            o.layer.views = o.layerViews;
            o.ready = true;
        }

        // Whether panels are still to be drawn into this sub-image's copy (and from now on, not): once per copy, so a frame
        // the game hands over again, unchanged, does not get them twice.
        bool FirstDraw(const XrSwapchainSubImage& sub) {
            std::lock_guard<std::mutex> guard(g->lock);
            const auto found = g->swapchains.find(sub.swapchain);
            if (found == g->swapchains.end() || sub.imageArrayIndex >= 32) return false;
            const uint32_t bit = 1u << sub.imageArrayIndex;
            if (found->second.panelsDrawnSlices & bit) return false;
            found->second.panelsDrawnSlices |= bit;
            return true;
        }

        // The world: the frame's first projection layer (its views 0 and 1 are the eyes), and where it lies in the frame.
        const XrCompositionLayerProjection* WorldOf(const XrFrameEndInfo* frame, uint32_t* index = nullptr) {
            for (uint32_t i = 0; i < frame->layerCount; i++) {
                if (frame->layers[i] && frame->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    if (index) *index = i;
                    return reinterpret_cast<const XrCompositionLayerProjection*>(frame->layers[i]);
                }
            }
            return nullptr;
        }

        // Each picture a panel layer shows.
        template <typename Visit> void ForEachSubImage(const XrCompositionLayerBaseHeader* layer, Visit visit) {
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                visit(reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage);
            } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
                visit(reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->subImage);
            } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                for (uint32_t i = 0; i < projection->viewCount && i < 4; i++) visit(projection->views[i].subImage);
            }
        }

        // What other tools (or the game) lay over the world - the layers after it that the mirror draws in: flat panels
        // (OpenKneeboard's kneeboards, say), curved ones, and further projection layers. Layers before the world lie
        // behind it. Every frame, mirrored or not, so the hand-backs know which pictures are panels.
        std::vector<const XrCompositionLayerBaseHeader*> PanelLayers(const XrFrameEndInfo* frame) {
            std::vector<const XrCompositionLayerBaseHeader*> panels;
            uint32_t worldIndex = 0;
            if (g->pipeline.settings().showPanels && WorldOf(frame, &worldIndex)) {
                for (uint32_t i = worldIndex + 1; i < frame->layerCount; i++) {
                    const XrCompositionLayerBaseHeader* layer = frame->layers[i];
                    if (!layer) continue;
                    if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD || layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR ||
                        layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                        panels.push_back(layer);
                    } else {
                        // Cube and equirect layers (skies, 360 video) and the like: seen, not drawn.
                        LogFewTimes(g->logOtherLayers, 3, "panels: a layer of type %d over the world is not drawn into the mirror", int(layer->type));
                    }
                }
            }
            std::lock_guard<std::mutex> guard(g->lock);
            for (auto& entry : g->swapchains) entry.second.isPanel = false;
            for (const XrCompositionLayerBaseHeader* layer : panels) {
                ForEachSubImage(layer, [](const XrSwapchainSubImage& sub) {
                    const auto found = g->swapchains.find(sub.swapchain);
                    if (found != g->swapchains.end()) found->second.isPanel = true;
                });
            }
            return panels;
        }

        // A panel's picture (its copy), the whole sub-image of it as texture coordinates; false while there is no copy.
        bool PictureOf(const XrSwapchainSubImage& sub, Panel& panel) {
            SourceImage picture;
            if (!ImageOf(sub, picture, false)) return false;
            D3D11_TEXTURE2D_DESC desc{};
            picture.texture->GetDesc(&desc);
            panel.texture = picture.texture;
            panel.arraySlice = picture.arraySlice;
            panel.viewFormat = picture.viewFormat;
            panel.uv[0] = float(picture.x) / float(desc.Width);
            panel.uv[1] = float(picture.y) / float(desc.Height);
            panel.uv[2] = float(picture.x + picture.width) / float(desc.Width);
            panel.uv[3] = float(picture.y + picture.height) / float(desc.Height);
            return true;
        }

        // The pictures a panel layer puts in front of one eye: its corners from the layer's own frame (`local` in its space)
        // into the world's space, then into the eye's view.
        void PanelsOf(const XrCompositionLayerBaseHeader* layer, int eye, const XrFrameEndInfo* frame, const XrCompositionLayerProjection* world,
                      std::vector<Panel>& out) {
            XrSpace space = XR_NULL_HANDLE;
            XrEyeVisibility visibility = XR_EYE_VISIBILITY_BOTH;
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                space = reinterpret_cast<const XrCompositionLayerQuad*>(layer)->space;
                visibility = reinterpret_cast<const XrCompositionLayerQuad*>(layer)->eyeVisibility;
            } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
                space = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->space;
                visibility = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->eyeVisibility;
            } else {
                space = reinterpret_cast<const XrCompositionLayerProjection*>(layer)->space;
            }
            if ((visibility == XR_EYE_VISIBILITY_LEFT && eye != 0) || (visibility == XR_EYE_VISIBILITY_RIGHT && eye != 1)) return;
            // The layer's space in the world's space (the same space, mostly).
            XrPosef place{{0, 0, 0, 1}, {0, 0, 0}};
            if (space != world->space) {
                XrSpaceLocation located{XR_TYPE_SPACE_LOCATION};
                const XrSpaceLocationFlags both = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                if (XR_FAILED(g->next.xrLocateSpace(space, world->space, frame->displayTime, &located)) || (located.locationFlags & both) != both) return;
                place = located.pose;
            }
            const XrPosef& eyePose = world->views[eye].pose;
            const auto turnOf = [](const XrQuaternionf& q) { return Quat{q.x, q.y, q.z, q.w}; };
            const auto inEye = [&](const XrPosef& local, const Vec3& point) {
                const Vec3 a = Rotate(turnOf(local.orientation), point);
                const Vec3 b = Rotate(turnOf(place.orientation), {local.position.x + a.x, local.position.y + a.y, local.position.z + a.z});
                const Vec3 inWorld{place.position.x + b.x, place.position.y + b.y, place.position.z + b.z};
                return InverseRotate(turnOf(eyePose.orientation), {inWorld.x - eyePose.position.x, inWorld.y - eyePose.position.y, inWorld.z - eyePose.position.z});
            };
            Panel panel;
            panel.sourceAlpha = (layer->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            panel.premultiplied = (layer->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) == 0;
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                if (!PictureOf(quad->subImage, panel)) return;
                const float w = quad->size.width * 0.5f, h = quad->size.height * 0.5f;
                panel.corners[0] = inEye(quad->pose, {-w, h, 0.f});
                panel.corners[1] = inEye(quad->pose, {w, h, 0.f});
                panel.corners[2] = inEye(quad->pose, {-w, -h, 0.f});
                panel.corners[3] = inEye(quad->pose, {w, -h, 0.f});
                out.push_back(panel);
            } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
                // A curved panel: the inside of a cylinder about the pose's y axis, centred straight ahead (-z), as strips
                // of at most about 5.6 degrees, each drawn flat.
                const auto* cylinder = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
                if (!PictureOf(cylinder->subImage, panel) || cylinder->radius <= 0.f || cylinder->aspectRatio <= 0.f) return;
                const float angle = std::clamp(cylinder->centralAngle, 0.001f, 6.2831853f);
                const float h = cylinder->radius * angle / cylinder->aspectRatio * 0.5f;
                const int strips = std::clamp(int(ceilf(angle / 0.0981748f)), 1, 64);
                const float u0 = panel.uv[0], u1 = panel.uv[2];
                for (int k = 0; k < strips; k++) {
                    const float from = -angle * 0.5f + angle * float(k) / float(strips), to = -angle * 0.5f + angle * float(k + 1) / float(strips);
                    const float r = cylinder->radius;
                    Panel strip = panel;
                    strip.uv[0] = u0 + (u1 - u0) * float(k) / float(strips);
                    strip.uv[2] = u0 + (u1 - u0) * float(k + 1) / float(strips);
                    strip.corners[0] = inEye(cylinder->pose, {r * sinf(from), h, -r * cosf(from)});
                    strip.corners[1] = inEye(cylinder->pose, {r * sinf(to), h, -r * cosf(to)});
                    strip.corners[2] = inEye(cylinder->pose, {r * sinf(from), -h, -r * cosf(from)});
                    strip.corners[3] = inEye(cylinder->pose, {r * sinf(to), -h, -r * cosf(to)});
                    out.push_back(strip);
                }
            } else {
                // A further projection layer: each of its views for this eye (wide, then inner with quad views) as the
                // picture a metre ahead of that view, as wide as its field of view.
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                for (uint32_t i = uint32_t(eye); i < projection->viewCount && i < 4; i += 2) {
                    const XrCompositionLayerProjectionView& view = projection->views[i];
                    Panel picture = panel;
                    if (!PictureOf(view.subImage, picture)) continue;
                    const float l = tanf(view.fov.angleLeft), r = tanf(view.fov.angleRight), u = tanf(view.fov.angleUp), d = tanf(view.fov.angleDown);
                    picture.corners[0] = inEye(view.pose, {l, u, -1.f});
                    picture.corners[1] = inEye(view.pose, {r, u, -1.f});
                    picture.corners[2] = inEye(view.pose, {l, d, -1.f});
                    picture.corners[3] = inEye(view.pose, {r, d, -1.f});
                    out.push_back(picture);
                }
            }
        }

        // Draws the panel layers into the eyes' copies, in the frame's order, where the headset shows them; with quad views
        // into the inner image as well.
        void DrawPanels(const XrFrameEndInfo* frame, const XrCompositionLayerProjection* world, const std::vector<const XrCompositionLayerBaseHeader*>& layers,
                        SourceImage images[2], SourceImage inner[2], const FrameInput& input) {
            size_t drawn = 0;
            std::vector<Panel> panels;
            for (int eye = 0; eye < 2; eye++) {
                if (!input.images[eye] || !FirstDraw(world->views[eye].subImage)) continue;
                const bool innerToo = images[eye].focus && FirstDraw(world->views[2 + eye].subImage);
                for (const XrCompositionLayerBaseHeader* layer : layers) {
                    panels.clear();
                    PanelsOf(layer, eye, frame, world, panels);
                    bool any = false;
                    for (const Panel& panel : panels) {
                        any = g->panels.draw(images[eye], input.views[eye].fov, panel) || any;
                        if (innerToo) g->panels.draw(inner[eye], input.views[eye].fov, panel, images[eye].focusWindow);
                    }
                    if (any && eye == g->pipeline.settings().eye) drawn++;
                }
            }
            if (drawn != g->lastPanelCount) {
                g->lastPanelCount = drawn;
                LogFewTimes(g->logPanels, 10, "panels: %zu of %zu layers over the world drawn into the mirrored eye", drawn, layers.size());
            }
            if (g->on12) g->immediate11->Flush();
        }

        void MirrorFrame(const XrFrameEndInfo* frame, const std::vector<const XrCompositionLayerBaseHeader*>& panelLayers) {
            const XrCompositionLayerProjection* projection = WorldOf(frame);
            if (!projection || projection->viewCount < 2) return;

            // Quad views (Varjo's scheme; Pimax's runtime offers it too): views 0 and 1 are the eyes' wide, coarse images,
            // 2 and 3 the sharp inner ones the runtime lays over part of them. The mirror lays them the same way.
            const bool quad = g->viewConfiguration == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && projection->viewCount >= 4;
            const int mirrored = g->pipeline.settings().eye == 0 ? 0 : 1;
            const bool snapshotDue = g->pipeline.snapshotDue();
            LogLayout(frame, projection);
            // What the next hand-backs copy: the slices of the views the mirror reads - the mirrored eye's (wide and inner),
            // and the other eye's while a snapshot is due.
            {
                std::lock_guard<std::mutex> guard(g->lock);
                for (auto& entry : g->swapchains) {
                    entry.second.layoutKnown = true;
                    entry.second.neededSlices = 0;
                }
                const auto need = [](SwapchainState& state, const XrSwapchainSubImage& sub) {
                    if (sub.imageArrayIndex < 32) state.neededSlices |= 1u << sub.imageArrayIndex;
                    const XrRect2Di& rect = sub.imageRect;
                    D3D11_BOX& box = state.neededBox;
                    box.left = std::min(box.left, UINT(std::max(rect.offset.x, 0)));
                    box.top = std::min(box.top, UINT(std::max(rect.offset.y, 0)));
                    box.right = std::max(box.right, UINT(std::max(rect.offset.x + rect.extent.width, 0)));
                    box.bottom = std::max(box.bottom, UINT(std::max(rect.offset.y + rect.extent.height, 0)));
                };
                for (uint32_t i = 0; i < projection->viewCount && i < 4; i++) {
                    const int eyeOfView = int(i % 2);
                    const auto found = g->swapchains.find(projection->views[i].subImage.swapchain);
                    if (found == g->swapchains.end() || (eyeOfView != mirrored && !snapshotDue)) continue;
                    need(found->second, projection->views[i].subImage);
                }
                for (const XrCompositionLayerBaseHeader* layer : panelLayers) {
                    ForEachSubImage(layer, [&](const XrSwapchainSubImage& sub) {
                        const auto found = g->swapchains.find(sub.swapchain);
                        if (found != g->swapchains.end()) need(found->second, sub);
                    });
                }
                for (auto& entry : g->swapchains) {
                    if (!entry.second.neededSlices) ReleaseCopy(entry.second); // Copied before the layout was known, not read.
                }
            }
            FrameInput input;
            SourceImage images[2], inner[2];
            float windows[2][4] = {}; // Quad views: where each eye's inner view lies in its wide one.
            bool haveWindow[2] = {false, false};
            for (int eye = 0; eye < 2; eye++) {
                const XrCompositionLayerProjectionView& view = projection->views[eye];
                if (ImageOf(view.subImage, images[eye], eye == mirrored)) input.images[eye] = &images[eye];
                EyeView& v = input.views[eye];
                v.width = float(view.subImage.imageRect.extent.width);
                v.height = float(view.subImage.imageRect.extent.height);
                v.fov = {tanf(view.fov.angleLeft), tanf(view.fov.angleRight), tanf(view.fov.angleUp), tanf(view.fov.angleDown)};
                v.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w};
                v.valid = v.width >= 64.f && v.height >= 64.f; // The view's geometry, copied image or not (the aids need it).
                if (!quad) continue;
                const XrCompositionLayerProjectionView& focus = projection->views[2 + eye];
                const FovTan in{tanf(focus.fov.angleLeft), tanf(focus.fov.angleRight), tanf(focus.fov.angleUp), tanf(focus.fov.angleDown)};
                const float spanX = v.fov.right - v.fov.left, spanY = v.fov.up - v.fov.down;
                const float sameWay = fabsf(view.pose.orientation.x * focus.pose.orientation.x + view.pose.orientation.y * focus.pose.orientation.y +
                                            view.pose.orientation.z * focus.pose.orientation.z + view.pose.orientation.w * focus.pose.orientation.w);
                const bool usable = spanX >= 1e-4f && spanY >= 1e-4f && in.right - in.left >= 1e-4f && in.up - in.down >= 1e-4f && sameWay >= 0.9999f;
                if (usable) {
                    windows[eye][0] = (in.left - v.fov.left) / spanX;
                    windows[eye][1] = (v.fov.up - in.up) / spanY;
                    windows[eye][2] = (in.right - v.fov.left) / spanX;
                    windows[eye][3] = (v.fov.up - in.down) / spanY;
                    haveWindow[eye] = true;
                }
                if (!input.images[eye]) continue;
                if (!usable || !ImageOf(focus.subImage, inner[eye], eye == mirrored)) {
                    LogFewTimes(g->logQuad, 3, "frame: quad views, but the %s eye's inner image cannot be used - the wide image alone is mirrored", eye == 0 ? "left" : "right");
                    continue;
                }
                SourceImage& image = images[eye];
                image.focus = &inner[eye];
                for (int i = 0; i < 4; i++) image.focusWindow[i] = windows[eye][i];
                // The eye image's size at the inner image's sharpness - what the crop and the published picture go by.
                // Kept for the session once found (the inner image may move with the eyes and shrink at the edges; the
                // picture must not change size with it), unless it changes by more than a fifth.
                const float acrossShare = image.focusWindow[2] - image.focusWindow[0], downShare = image.focusWindow[3] - image.focusWindow[1];
                const int32_t wantWidth = std::clamp(int32_t(lroundf(float(inner[eye].width) / acrossShare)) & ~1, image.width, 16384);
                const int32_t wantHeight = std::clamp(int32_t(lroundf(float(inner[eye].height) / downShare)) & ~1, image.height, 16384);
                if (g->nominalWidth == 0 || abs(wantWidth - g->nominalWidth) * 5 > g->nominalWidth || abs(wantHeight - g->nominalHeight) * 5 > g->nominalHeight) {
                    g->nominalWidth = wantWidth;
                    g->nominalHeight = wantHeight;
                    Log("frame: quad views - wide image %d x %d, inner image %d x %d over %.0f %% x %.0f %% of it: the eye image counts as %d x %d", image.width,
                        image.height, inner[eye].width, inner[eye].height, acrossShare * 100.f, downShare * 100.f, wantWidth, wantHeight);
                }
            }
            if (quad && g->nominalWidth > 0) {
                for (int eye = 0; eye < 2; eye++) {
                    if (!input.images[eye]) continue;
                    images[eye].nominalWidth = g->nominalWidth;
                    images[eye].nominalHeight = g->nominalHeight;
                    input.views[eye].width = float(g->nominalWidth);
                    input.views[eye].height = float(g->nominalHeight);
                }
            }
            if (!input.images[mirrored]) g->pipeline.renderMissed();
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

            if (!panelLayers.empty() || g->lastPanelCount) DrawPanels(frame, projection, panelLayers, images, inner, input);

            input.gazeSupport = g->gaze.supported() ? 1 : 2;
            // A snapshot needs both eyes; until the hand-backs copy the other one too, it waits.
            input.snapshotLater = snapshotDue && !input.images[1 - mirrored];
            const FrameOutput output = g->pipeline.frame(input);
            if (g->on12) g->immediate11->Flush(); // Our work on the copies goes down the game's queue now.
            if (output.edgeBar > 0 || output.marker || output.countdownSeconds > 0) DrawOverlay(frame, projection, output, windows, haveWindow);
        }

        XrResult XRAPI_CALL OnEndFrame(XrSession session, const XrFrameEndInfo* frame) {
            if (session == g->session && frame && g->mirrorDevice) {
                if (g->pipelineReady && g->pipeline.lost()) {
                    // The SteamVR helper took the picture over (it comes through SteamVR's compositor): stand by.
                    Log("frame: the SteamVR helper gives the picture from here - the layer stands by");
                    g->pipeline.stop();
                    g->pipelineReady = false;
                    g->standByFrames = 0;
                } else if (!g->pipelineReady && ++g->standByFrames >= 90) {
                    // Standing by: about once a second of the game's frames, publish again if nobody does any more, or if
                    // the game's own picture is wanted now.
                    g->standByFrames = 0;
                    if (g->pipeline.layerShouldPublish(g->underSteamVR)) {
                        g->pipelineReady = g->pipeline.start(g->mirrorDevice, ProducerOpenXR, g->program.c_str(), g->application.c_str(), g->underSteamVR);
                        if (g->pipelineReady) Log("frame: the layer gives the picture from here");
                    }
                }
            }
            if (session != g->session) return g->next.xrEndFrame(session, frame);
            if (g->firstRelease.time_since_epoch().count() != 0) {
                // The longest lately: a longer lead counts at once, a shorter one only slowly.
                const auto lead = std::chrono::steady_clock::now() - g->firstRelease;
                g->releaseLead = std::max(lead, g->releaseLead - g->releaseLead / 64);
                g->firstRelease = {};
            }
            g->overlay.ready = false;
            if (g->pipelineReady && frame) {
                g->frameCount++;
                const std::vector<const XrCompositionLayerBaseHeader*> panelLayers = PanelLayers(frame);
                const bool due = g->pipeline.wanted();
                g->imagesNeeded = g->pipeline.imagesNeeded();
                if (due) MirrorFrame(frame, panelLayers);
            } else {
                g->imagesNeeded = false;
            }
            if (!g->imagesNeeded && !(g->pipelineReady && g->pipeline.readerLive())) {
                // Nobody needs the images: the copies go, and come back with the first hand-back that is needed.
                std::lock_guard<std::mutex> guard(g->lock);
                bool any = false;
                for (auto& entry : g->swapchains) {
                    if (entry.second.isPanel) continue; // Kept: the panel may not be handed back again for a while.
                    any = any || entry.second.copy;
                    ReleaseCopy(entry.second);
                }
                if (any) Log("copy: nobody needs the eye images - the copies are let go");
            }
            if (!g->overlay.ready) {
                DestroyOverlay(); // Nothing to show (any more): its images go too.
                return g->next.xrEndFrame(session, frame);
            }
            // The aids' layer goes on top of the game's.
            std::vector<const XrCompositionLayerBaseHeader*> layers(frame->layers, frame->layers + frame->layerCount);
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g->overlay.layer));
            XrFrameEndInfo withAids = *frame;
            withAids.layers = layers.data();
            withAids.layerCount = static_cast<uint32_t>(layers.size());
            return g->next.xrEndFrame(session, &withAids);
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
                Resolve(*state, "xrEnumerateSwapchainFormats", next.xrEnumerateSwapchainFormats) &
                Resolve(*state, "xrWaitSwapchainImage", next.xrWaitSwapchainImage) &
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
            state->underSteamVR = strstr(properties.runtimeName, "SteamVR") != nullptr;
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
