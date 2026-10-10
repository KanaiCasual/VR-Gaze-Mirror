// VR Gaze Mirror 2.0 - offline test of the OpenXR layer.
//
// Plays both the game and the runtime: loads the layer DLL the way the OpenXR loader does, hands it a fake runtime
// (stub functions), submits frames whose eye image is a test pattern with a BLACK SQUARE exactly where the fake eye
// tracker "looks", and reads the published picture back on a SECOND Direct3D device like a real reader would.
// Each case saves a PNG: the ring must sit on the black square, whatever the crop.
//
//   layer_test.exe <layer dll> <output folder>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <map>
#include <thread>
#include <string>
#include <vector>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>

#include "../core/gaze_grid.h"
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "../protocol/gaze_mirror_protocol.h"

using Microsoft::WRL::ComPtr;
using namespace gaze_mirror;

namespace {

    constexpr char LayerName[] = "XR_APILAYER_NOVENDOR_gaze_mirror";
    constexpr uint32_t ImageWidth = 2000, ImageHeight = 2100;
    constexpr float HalfFov = 0.7853982f;                   // 45 degrees each way.
    constexpr float GazeYaw = 0.1745329f, GazePitch = 0.0872665f; // Looking 10 degrees right, 5 degrees up.

    int g_failures = 0;
    void Check(bool ok, const char* what) {
        printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) g_failures++;
    }

    // ---- the fake runtime ---------------------------------------------------------------------------------------
    std::vector<ID3D11Texture2D*> g_swapchainImages;
    std::vector<ID3D12Resource*> g_swapchainImages12; // The Direct3D 12 case's images (the "game" is D3D12 then).
    bool g_d3d12 = false;
    bool g_noGaze = false; // The runtime has the eye-gaze interface, but never a gaze (SteamVR with a driver that passes none on).
    uint32_t g_acquireCounter = 0;
    uint32_t g_suggestedBindings = 0, g_attachedSets = 0, g_syncedSets = 0;
    bool g_sawEyeGazeExtension = false;
    std::map<std::string, XrPath> g_paths;

    XrResult XRAPI_CALL StubSuccess() { return XR_SUCCESS; }
    XrResult XRAPI_CALL StubGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
        strcpy_s(properties->runtimeName, "Fake runtime");
        properties->runtimeVersion = XR_MAKE_VERSION(1, 2, 3);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
        strcpy_s(properties->systemName, "Fake headset");
        for (auto entry = static_cast<XrBaseOutStructure*>(properties->next); entry; entry = entry->next) {
            if (entry->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
                reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(entry)->supportsEyeGazeInteraction = XR_TRUE;
            }
        }
        return XR_SUCCESS;
    }
    template <typename Handle> XrResult MakeHandle(Handle* handle) {
        static uint64_t next = 0x1000;
        *handle = reinterpret_cast<Handle>(static_cast<uintptr_t>(next++));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) { return MakeHandle(session); }
    XrResult XRAPI_CALL StubCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* swapchain) { return MakeHandle(swapchain); }
    XrResult XRAPI_CALL StubCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set) { return MakeHandle(set); }
    XrResult XRAPI_CALL StubCreateAction(XrActionSet, const XrActionCreateInfo*, XrAction* action) { return MakeHandle(action); }
    XrResult XRAPI_CALL StubCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) { return MakeHandle(space); }
    XrResult XRAPI_CALL StubCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) { return MakeHandle(space); }
    XrResult XRAPI_CALL StubStringToPath(XrInstance, const char* text, XrPath* path) {
        auto found = g_paths.find(text);
        if (found == g_paths.end()) found = g_paths.emplace(text, static_cast<XrPath>(g_paths.size() + 1)).first;
        *path = found->second;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubEnumerateSwapchainImages(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
        *count = static_cast<uint32_t>(g_d3d12 ? g_swapchainImages12.size() : g_swapchainImages.size());
        if (capacity == 0 || !images) return XR_SUCCESS;
        if (g_d3d12) {
            auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            for (uint32_t i = 0; i < *count && i < capacity; i++) d3dImages[i].texture = g_swapchainImages12[i];
        } else {
            auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (uint32_t i = 0; i < *count && i < capacity; i++) d3dImages[i].texture = g_swapchainImages[i];
        }
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
        *index = g_acquireCounter++ % static_cast<uint32_t>(g_d3d12 ? g_swapchainImages12.size() : g_swapchainImages.size());
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubSuggestBindings(XrInstance, const XrInteractionProfileSuggestedBinding* suggested) {
        g_suggestedBindings = suggested->countSuggestedBindings;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubAttach(XrSession, const XrSessionActionSetsAttachInfo* info) {
        g_attachedSets = info->countActionSets;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubSync(XrSession, const XrActionsSyncInfo* info) {
        g_syncedSets = info->countActiveActionSets;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
        // Yaw to the right (about -Y), then pitch up (about +X).
        const float sy = sinf(-GazeYaw / 2), cy = cosf(-GazeYaw / 2), sp = sinf(GazePitch / 2), cp = cosf(GazePitch / 2);
        location->pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
        location->pose.position = {0, 0, 0};
        location->locationFlags = g_noGaze ? 0
                                           : XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                                                 XR_SPACE_LOCATION_POSITION_VALID_BIT;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* state, uint32_t capacity, uint32_t* count, XrView* views) {
        *count = 2;
        state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        for (uint32_t i = 0; i < 2 && i < capacity; i++) {
            views[i].pose = {{0, 0, 0, 1}, {i == 0 ? -0.032f : 0.032f, 0, 0}};
            views[i].fov = {-HalfFov, HalfFov, HalfFov, -HalfFov};
        }
        return XR_SUCCESS;
    }

    XrResult XRAPI_CALL StubGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
        static const std::map<std::string, PFN_xrVoidFunction> table = {
            {"xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction>(StubGetInstanceProperties)},
            {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(StubGetSystemProperties)},
            {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(StubCreateSession)},
            {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(StubCreateSwapchain)},
            {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(StubCreateActionSet)},
            {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(StubCreateAction)},
            {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(StubCreateActionSpace)},
            {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(StubCreateReferenceSpace)},
            {"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(StubStringToPath)},
            {"xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction>(StubEnumerateSwapchainImages)},
            {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(StubAcquireSwapchainImage)},
            {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(StubSuggestBindings)},
            {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(StubAttach)},
            {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(StubSync)},
            {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(StubLocateSpace)},
            {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(StubLocateViews)},
        };
        const auto found = table.find(name);
        *function = found != table.end() ? found->second : reinterpret_cast<PFN_xrVoidFunction>(StubSuccess);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo*, XrInstance* instance) {
        g_sawEyeGazeExtension = false;
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
            if (strcmp(info->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) g_sawEyeGazeExtension = true;
        }
        return MakeHandle(instance);
    }

    // ---- helpers ------------------------------------------------------------------------------------------------
    // Where the fake gaze lands in the eye image.
    void ExpectedGaze(float& u, float& v) {
        u = (tanf(GazeYaw) + tanf(HalfFov)) / (2 * tanf(HalfFov));
        v = (tanf(HalfFov) - tanf(GazePitch) / cosf(GazeYaw)) / (2 * tanf(HalfFov)); // Pitched first, then turned.
    }

    // The test pattern of one slice: a gradient with a grid, and a black square where the fake eye tracker looks.
    void FillPattern(std::vector<uint8_t>& pixels, uint32_t slice) {
        float gazeU, gazeV;
        ExpectedGaze(gazeU, gazeV);
        const int gx = int(gazeU * ImageWidth), gy = int(gazeV * ImageHeight);
        pixels.resize(size_t(ImageWidth) * ImageHeight * 4);
        for (uint32_t y = 0; y < ImageHeight; y++) {
            for (uint32_t x = 0; x < ImageWidth; x++) {
                uint8_t* p = &pixels[(size_t(y) * ImageWidth + x) * 4];
                p[0] = uint8_t(x * 255 / ImageWidth);
                p[1] = uint8_t(y * 255 / ImageHeight);
                p[2] = slice == 0 ? 90 : 230; // The right-eye slice is visibly bluer.
                p[3] = 255;
                if (x % 100 == 0 || y % 100 == 0) p[0] = p[1] = p[2] = 255;
                if (abs(int(x) - gx) <= 6 && abs(int(y) - gy) <= 6) p[0] = p[1] = p[2] = 0;
            }
        }
    }

    // The same pattern as a Direct3D 12 texture: uploaded through a staging buffer on the game's queue, then left in the
    // render-target state, which is how a D3D12 runtime hands swapchain images out.
    ComPtr<ID3D12Resource> MakePattern12(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t arraySize) {
        D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = ImageWidth;
        desc.Height = ImageHeight;
        desc.DepthOrArraySize = uint16_t(arraySize);
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> texture;
        if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))) return nullptr;

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(arraySize);
        std::vector<UINT> rowCounts(arraySize);
        std::vector<UINT64> rowSizes(arraySize);
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, arraySize, 0, footprints.data(), rowCounts.data(), rowSizes.data(), &total);
        D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = total;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return nullptr;
        uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return nullptr;
        std::vector<uint8_t> pixels;
        for (uint32_t slice = 0; slice < arraySize; slice++) {
            FillPattern(pixels, slice);
            for (uint32_t y = 0; y < ImageHeight; y++) {
                memcpy(mapped + footprints[slice].Offset + size_t(y) * footprints[slice].Footprint.RowPitch, &pixels[size_t(y) * ImageWidth * 4], size_t(ImageWidth) * 4);
            }
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        for (uint32_t slice = 0; slice < arraySize; slice++) {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprints[slice];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        queue->Signal(fence.Get(), 1);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, done);
        WaitForSingleObject(done, 10000);
        CloseHandle(done);
        return texture;
    }

    ComPtr<ID3D11Texture2D> MakePattern(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t arraySize, bool sampleable) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = ImageWidth;
        desc.Height = ImageHeight;
        desc.MipLevels = 1;
        desc.ArraySize = arraySize;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; // Like a runtime's swapchain image.
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | (sampleable ? D3D11_BIND_SHADER_RESOURCE : 0);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return nullptr;
        std::vector<uint8_t> pixels;
        for (uint32_t slice = 0; slice < arraySize; slice++) {
            FillPattern(pixels, slice);
            context->UpdateSubresource(texture.Get(), D3D11CalcSubresource(0, slice, 1), nullptr, pixels.data(), ImageWidth * 4, 0);
        }
        return texture;
    }

    bool SavePng(const std::wstring& path, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
                        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(desc.Width, desc.Height)) &&
                        SUCCEEDED(frame->SetPixelFormat(&format)) &&
                        SUCCEEDED(frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height, static_cast<BYTE*>(mapped.pData))) &&
                        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        context->Unmap(staging.Get(), 0);
        return ok;
    }

    template <typename Function> Function Get(PFN_xrGetInstanceProcAddr getter, XrInstance instance, const char* name) {
        PFN_xrVoidFunction function = nullptr;
        getter(instance, name, &function);
        return reinterpret_cast<Function>(function);
    }

    struct Case {
        const char* name;
        const char* settings;
        uint32_t arraySize;
        bool sampleable;
        int eye;
        uint32_t expectWidth, expectHeight;
        bool d3d12 = false; // The game renders with Direct3D 12: the layer must go through 11-on-12.
        bool noGaze = false; // The game's runtime never has a gaze; the helper hands the headset's over (its block is made here).
        bool quad = false;  // Quad views: four views in one image - the wide ones small in its top left corner, the
                            // sharp inner ones (the middle 40 % of the view) in its bottom right quarter.
    };

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: layer_test <layer dll> <output folder>\n");
        return 2;
    }
    const std::wstring folder = argv[2];
    CreateDirectoryW(folder.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring settingsFile = folder + L"\\test-settings.cfg";
    SetEnvironmentVariableW(L"GAZE_MIRROR_SETTINGS_FILE", settingsFile.c_str());
    SetEnvironmentVariableW(L"GAZE_MIRROR_LOG_FILE", (folder + L"\\layer.log").c_str());

    // The thorough VRChat calibration's sums, on made-up eyes: a tracker that reports sines with some cross-talk and
    // trails the target by 120 ms, a little noise, VRChat sending at 50 Hz, two blinks.
    {
        using namespace gaze_mirror::grid;
        printf("case grid-fit\n");
        const std::vector<Waypoint> path = BuildPath(35.f, 35.f, 28.f, 35.f, 12.f, 600.f, 1500.f, 10.f);
        const auto eyes = [](float tanX, float tanY, float& vx, float& vy) {
            const float yaw = atanf(tanX), pitch = atanf(tanY);
            vx = 0.9f * sinf(yaw) + 0.08f * sinf(pitch) * sinf(yaw);
            vy = 0.8f * sinf(pitch) - 0.05f * yaw * yaw;
        };
        std::vector<Record> records;
        uint32_t seed = 12345;
        const auto noise = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return (float(seed >> 8) / float(1u << 24) - 0.5f) * 0.01f;
        };
        const size_t lagSteps = 12;
        for (size_t i = 0; i < path.size(); i++) {
            const Waypoint& w = path[i];
            const Waypoint& seen = path[i >= lagSteps ? i - lagSteps : 0];
            float vx = 0, vy = 0;
            eyes(seen.tanX, seen.tanY, vx, vy);
            const bool blink = (w.timeMs > 20000.f && w.timeMs < 20300.f) || (w.timeMs > 60000.f && w.timeMs < 60250.f);
            records.push_back({w.timeMs, w.tanX, w.tanY, w.paused, blink ? 0.f : vx + noise(), blink ? 0.f : vy + noise(), i % 2 == 0, !blink});
        }
        printf("       the path takes %.0f s\n", path.back().timeMs / 1000.f);
        Check(path.back().timeMs > 60000.f && path.back().timeMs < 150000.f, "the moving target's path takes between one and two and a half minutes");
        Result result;
        Check(Calibrate(records, 10.f, 400, result), "the thorough calibration fits made-up eyes");
        printf("       lag %d ms (true 120), fit %.2f deg over %d samples (%d dropped)\n", result.lagMs, result.errorDeg, result.samples, result.dropped);
        Check(abs(result.lagMs - 120) <= 20, "and finds how far the eyes trail the target");
        float worst = 0.f;
        for (float yaw = -30.f; yaw <= 30.f; yaw += 15.f) {
            for (float pitch = -30.f; pitch <= 24.f; pitch += 12.f) {
                float vx = 0, vy = 0, tx = 0, ty = 0;
                eyes(tanf(yaw * Pi / 180.f), tanf(pitch * Pi / 180.f), vx, vy);
                Lookup(result.grid, vx, vy, tx, ty);
                const float dx = atanf(tx) * 180.f / Pi - yaw, dy = atanf(ty) * 180.f / Pi - pitch;
                worst = std::max(worst, sqrtf(dx * dx + dy * dy));
            }
        }
        printf("       worst error %.2f deg inside the reach\n", worst);
        Check(worst < 1.5f, "the map is within 1.5 degrees everywhere inside the reach");
        const Grid back = FromText(ToText(result.grid));
        Check(back.valid() && back.cols == result.grid.cols && fabsf(back.tx[12] - result.grid.tx[12]) < 1e-4f && fabsf(back.y1 - result.grid.y1) < 1e-4f,
              "the map survives the settings file");
    }

    // A live VR session (a producer publishing, or a reader such as OBS waiting) would be disturbed by this test, and its
    // "no reader" checks could not hold: step aside rather than fail.
    if (HANDLE live = OpenFileMappingW(FILE_MAP_READ, FALSE, FramesMappingName)) {
        const Frames* frames = static_cast<const Frames*>(MapViewOfFile(live, FILE_MAP_READ, 0, 0, sizeof(Frames)));
        bool busy = false;
        if (frames && frames->magic == FramesMagic) {
            if (frames->producerKind != ProducerNone) busy = true;
            for (const Reader& reader : frames->readers) {
                if (reader.pid == 0) continue;
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(reader.pid));
                if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) busy = true;
                if (process) CloseHandle(process);
            }
        }
        if (frames) UnmapViewOfFile(frames);
        CloseHandle(live);
        if (busy) {
            printf("SKIPPED: a VR session is live (a mirror producer or reader is running) - the offline test would disturb it.\n");
            return 0;
        }
    }

    // The settings app's desktop cover sleeps on the two events this test looks at and takes them first - and a hotkey
    // pressed here would switch the user's own cover. With the app running (it holds the event; no producer of this
    // test exists yet), those checks step aside and no key is pressed.
    bool appListening = false;
    if (HANDLE held = OpenEventW(SYNCHRONIZE, FALSE, ProducerChangedName)) {
        appListening = true;
        CloseHandle(held);
        printf("note: the settings app is running - the producers' announcements and the desktop cover's hotkey are not checked (close the app to check them)\n");
    }

    HMODULE library = LoadLibraryW(argv[1]);
    if (!library) {
        printf("the layer DLL could not be loaded (%lu)\n", GetLastError());
        return 2;
    }
    using Negotiate = XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, const char*, XrNegotiateApiLayerRequest*);
    const auto negotiate = reinterpret_cast<Negotiate>(GetProcAddress(library, "xrNegotiateLoaderApiLayerInterface"));
    XrNegotiateLoaderInfo loaderInfo{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo)};
    loaderInfo.minInterfaceVersion = 1;
    loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(XrNegotiateApiLayerRequest)};
    printf("negotiation\n");
    Check(negotiate && XR_SUCCEEDED(negotiate(&loaderInfo, LayerName, &request)), "the layer accepts the loader");
    Check(negotiate && XR_FAILED(negotiate(&loaderInfo, "XR_APILAYER_other", &request)), "and refuses another layer's name");
    if (!request.getInstanceProcAddr || !request.createApiLayerInstance) return 1;

    // The "game" and the "reader" are different Direct3D devices, as in real life.
    ComPtr<ID3D11Device> gameDevice, readerDevice;
    ComPtr<ID3D11DeviceContext> gameContext, readerContext;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gameDevice, nullptr, &gameContext);
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &readerDevice, nullptr, &readerContext);
    if (!gameDevice || !readerDevice) {
        printf("no Direct3D 11 device\n");
        return 2;
    }
    // A Direct3D 12 "game" too: its device and the queue it renders on.
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)))) {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12));
    }
    if (!device12 || !queue12) printf("no Direct3D 12 device: the Direct3D 12 case will fail\n");

    const Case cases[] = {
        {"plain", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\nmirror_eye=left\n", 1, true, 0, 2000, 2100},
        {"gaze-from-helper", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\nmirror_eye=left\n", 1, true, 0, 2000, 2100, false, true},
        {"quad-views", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\nmirror_eye=left\n", 1, true, 0, 2500, 2624, false, false, true},
        {"fixed-centre", "enabled=1\ngaze_source=fixed\nfixed_focus_width=0.5\nfixed_focus_height=0.45\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\nmirror_eye=left\n", 1, true, 0, 2000, 2100},
        {"crop", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=1\ncrop_aspect=16:9\ncrop_height=0.5\ncrop_center_x=0.6\ncrop_center_y=0.45\nmirror_eye=left\n", 1, true, 0, 1866, 1050},
        {"steady-tilt", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=1\ncrop_aspect=16:9\ncrop_height=0.5\ncrop_center_x=0.5\ncrop_center_y=0.5\nstabilize=1\nstabilize_min_cutoff=0.02\nstabilize_beta=0\nstabilize_room=0.1\nmirror_eye=left\n", 1, true, 0, 1866, 1050},
        {"copy-path", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=1\ncrop_aspect=16:9\ncrop_height=0.5\ncrop_center_x=0.6\ncrop_center_y=0.45\nmirror_eye=left\n", 1, false, 0, 1866, 1050},
        {"array-right-eye", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\noutput_max_side=1000\nheadset_marker=1\n", 2, true, 1, 952, 1000},
        {"d3d12-right-eye", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\noutput_max_side=1000\nheadset_marker=1\n", 2, true, 1, 952, 1000, true},
    };

    for (const Case& test : cases) {
        printf("\ncase %s\n", test.name);
        FILE* file = nullptr;
        _wfopen_s(&file, settingsFile.c_str(), L"w");
        fputs(test.settings, file);
        fclose(file);

        XrApiLayerNextInfo nextInfo{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
        strcpy_s(nextInfo.layerName, LayerName);
        nextInfo.nextGetInstanceProcAddr = StubGetInstanceProcAddr;
        nextInfo.nextCreateApiLayerInstance = StubCreateApiLayerInstance;
        XrApiLayerCreateInfo layerInfo{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
        layerInfo.nextInfo = &nextInfo;
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(instanceInfo.applicationInfo.applicationName, "Layer test");
        instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        XrInstance instance = XR_NULL_HANDLE;
        Check(XR_SUCCEEDED(request.createApiLayerInstance(&instanceInfo, &layerInfo, &instance)), "instance is created");
        Check(g_sawEyeGazeExtension, "the eye-gaze extension was added to the game's request");
        const auto get = request.getInstanceProcAddr;

        const auto createSession = Get<PFN_xrCreateSession>(get, instance, "xrCreateSession");
        const auto beginSession = Get<PFN_xrBeginSession>(get, instance, "xrBeginSession");
        const auto createSwapchain = Get<PFN_xrCreateSwapchain>(get, instance, "xrCreateSwapchain");
        const auto enumerateImages = Get<PFN_xrEnumerateSwapchainImages>(get, instance, "xrEnumerateSwapchainImages");
        const auto acquire = Get<PFN_xrAcquireSwapchainImage>(get, instance, "xrAcquireSwapchainImage");
        const auto release = Get<PFN_xrReleaseSwapchainImage>(get, instance, "xrReleaseSwapchainImage");
        const auto attach = Get<PFN_xrAttachSessionActionSets>(get, instance, "xrAttachSessionActionSets");
        const auto sync = Get<PFN_xrSyncActions>(get, instance, "xrSyncActions");
        const auto endFrame = Get<PFN_xrEndFrame>(get, instance, "xrEndFrame");
        const auto destroySwapchain = Get<PFN_xrDestroySwapchain>(get, instance, "xrDestroySwapchain");
        const auto destroySession = Get<PFN_xrDestroySession>(get, instance, "xrDestroySession");
        const auto destroyInstance = Get<PFN_xrDestroyInstance>(get, instance, "xrDestroyInstance");

        g_d3d12 = test.d3d12;
        g_noGaze = test.noGaze;
        // The helper's part, played here: the headset's gaze in the shared block, kept fresh while the case runs.
        std::atomic<bool> handing{test.noGaze};
        std::thread helper;
        HANDLE handedMapping = nullptr;
        ExternalGaze* handedBlock = nullptr;
        if (test.noGaze) {
            handedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ExternalGaze), RuntimeGazeMappingName);
            ExternalGaze* block = handedMapping ? static_cast<ExternalGaze*>(MapViewOfFile(handedMapping, FILE_MAP_WRITE, 0, 0, sizeof(ExternalGaze))) : nullptr;
            handedBlock = block;
            Check(block != nullptr, "the helper's gaze block was made");
            if (block) {
                block->version = ExternalGazeVersion;
                block->structSize = sizeof(ExternalGaze);
                block->magic = ExternalGazeMagic;
                block->source = ExternalGazeSourceOpenXrRuntime;
                strcpy_s(block->writerName, "Test runtime");
                block->writerPid = LONG(GetCurrentProcessId());
                // The same look as the fake eye tracker's: pitched up first, then turned to the right.
                block->leftX = block->rightX = sinf(GazeYaw) * cosf(GazePitch);
                block->leftY = block->rightY = sinf(GazePitch);
                block->leftZ = block->rightZ = -cosf(GazeYaw) * cosf(GazePitch);
                block->leftValid = block->rightValid = 1;
                block->writtenMs = LONGLONG(GetTickCount64());
                block->sequence = 2;
                helper = std::thread([block, &handing] {
                    while (handing) {
                        block->writtenMs = LONGLONG(GetTickCount64());
                        Sleep(20);
                    }
                    block->leftValid = block->rightValid = 0;
                    block->writerPid = 0;
                    UnmapViewOfFile(block);
                });
            }
        }
        ComPtr<ID3D11Texture2D> imageA, imageB;
        ComPtr<ID3D12Resource> image12A, image12B;
        if (test.d3d12) {
            image12A = MakePattern12(device12.Get(), queue12.Get(), test.arraySize);
            image12B = MakePattern12(device12.Get(), queue12.Get(), test.arraySize);
            g_swapchainImages12 = {image12A.Get(), image12B.Get()};
            Check(image12A && image12B, "the Direct3D 12 game's images were made");
            if (!image12A || !image12B) continue;
        } else {
            imageA = MakePattern(gameDevice.Get(), gameContext.Get(), test.arraySize, test.sampleable);
            imageB = MakePattern(gameDevice.Get(), gameContext.Get(), test.arraySize, test.sampleable);
            g_swapchainImages = {imageA.Get(), imageB.Get()};
        }
        IUnknown* watched = test.d3d12 ? static_cast<IUnknown*>(image12A.Get()) : static_cast<IUnknown*>(imageA.Get());
        const ULONG referencesBefore = (watched->AddRef(), watched->Release());

        XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding11.device = gameDevice.Get();
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding12.device = device12.Get();
        binding12.queue = queue12.Get();
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, test.d3d12 ? static_cast<const void*>(&binding12) : static_cast<const void*>(&binding11)};
        sessionInfo.systemId = 1;
        XrSession session = XR_NULL_HANDLE;
        // What the settings app's desktop cover sleeps on: set when a producer starts, stops or names another game.
        HANDLE producerChanged = CreateEventW(nullptr, FALSE, FALSE, ProducerChangedName);
        ResetEvent(producerChanged);
        Check(XR_SUCCEEDED(createSession(instance, &sessionInfo, &session)), "session is created");
        if (!appListening) Check(WaitForSingleObject(producerChanged, 0) == WAIT_OBJECT_0, "the new producer is announced");
        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = test.quad ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        beginSession(session, &beginInfo);

        XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        swapchainInfo.width = ImageWidth;
        swapchainInfo.height = ImageHeight;
        swapchainInfo.arraySize = test.arraySize;
        swapchainInfo.sampleCount = swapchainInfo.faceCount = swapchainInfo.mipCount = 1;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        createSwapchain(session, &swapchainInfo, &swapchain);
        uint32_t imageCount = 0;
        enumerateImages(swapchain, 0, &imageCount, nullptr);
        // The D3D11 and D3D12 image structs have the same shape (type, next, one pointer): one array serves both.
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {test.d3d12 ? XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR : XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        enumerateImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

        // The game's own input set-up: one action set. Ours must travel along.
        XrActionSet gameSet = reinterpret_cast<XrActionSet>(static_cast<uintptr_t>(0x7777));
        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.actionSets = &gameSet;
        attachInfo.countActionSets = 1;
        attach(session, &attachInfo);
        Check(g_attachedSets == 2, "attach: the game's action set + ours reach the runtime");
        Check(g_suggestedBindings == 1, "our eye-gaze binding was suggested");

        // Become a reader, so that the layer has somebody to work for.
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, FramesMappingName);
        Frames* frames = mapping ? static_cast<Frames*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Frames))) : nullptr;
        Check(frames && frames->magic == FramesMagic && frames->producerKind == ProducerOpenXR, "the shared block exists and names an OpenXR producer");
        if (!frames) return 1;
        Check(strcmp(frames->producerProgram, "layer_test.exe") == 0, "it names the game's program");

        XrCompositionLayerProjectionView views[4] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        const float innerHalfFov = atanf(0.4f * tanf(HalfFov)); // The middle 40 % of the view, in tangents.
        for (int i = 0; i < 4; i++) {
            views[i].pose = {{0, 0, 0, 1}, {0, 0, 0}};
            views[i].fov = {-HalfFov, HalfFov, HalfFov, -HalfFov};
            views[i].subImage.swapchain = swapchain;
            views[i].subImage.imageRect = {{0, 0}, {int32_t(ImageWidth), int32_t(ImageHeight)}};
            views[i].subImage.imageArrayIndex = test.arraySize > 1 ? i % 2 : 0;
            if (test.quad && i < 2) views[i].subImage.imageRect = {{0, 0}, {500, 525}};
            if (test.quad && i >= 2) {
                views[i].fov = {-innerHalfFov, innerHalfFov, innerHalfFov, -innerHalfFov};
                views[i].subImage.imageRect = {{1000, 1050}, {1000, 1050}};
            }
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = test.quad ? 4 : 2;
        projection.views = views;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
        XrFrameEndInfo frameEnd{XR_TYPE_FRAME_END_INFO};
        frameEnd.displayTime = 1000;
        frameEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEnd.layerCount = 1;
        frameEnd.layers = layers;

        // No reader yet: nothing may be produced.
        uint32_t index = 0;
        const LONGLONG framesBefore = frames->frameNumber;
        acquire(swapchain, nullptr, &index);
        release(swapchain, nullptr);
        endFrame(session, &frameEnd);
        Check(frames->latestSlot == NoSlot && frames->frameNumber == framesBefore, "without a reader no picture is made");

        int place = -1;
        for (uint32_t i = 0; i < ReaderCount && place < 0; i++) {
            if (InterlockedCompareExchange(&frames->readers[i].pid, static_cast<LONG>(GetCurrentProcessId()), 0) == 0) place = int(i);
        }
        Check(place >= 0, "a free reader place was found");
        if (place < 0) return 1;
        Reader& me = frames->readers[place];
        me.readingSlot = NoSlot;
        me.heartbeatMs = static_cast<LONGLONG>(GetTickCount64());
        wchar_t eventName[64];
        swprintf_s(eventName, ReaderEventFormat, static_cast<unsigned>(place));
        HANDLE frameReady = CreateEventW(nullptr, FALSE, FALSE, eventName);
        ResetEvent(frameReady);

        LONG slots[3] = {};
        for (int i = 0; i < 3; i++) {
            acquire(swapchain, nullptr, &index);
            release(swapchain, nullptr);
            const XrActiveActionSet active{gameSet, XR_NULL_PATH};
            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.activeActionSets = &active;
            syncInfo.countActiveActionSets = 1;
            sync(session, &syncInfo);
            endFrame(session, &frameEnd);
            slots[i] = frames->latestSlot;
        }
        Check(g_syncedSets == 2, "sync: the game's action set + ours reach the runtime");
        Check(frames->frameNumber - framesBefore == 3, "three frames were published");
        Check(WaitForSingleObject(frameReady, 0) == WAIT_OBJECT_0, "the reader's event was set");
        Check(slots[0] != slots[1] && slots[1] != slots[2], "consecutive frames use different slots");
        Check(frames->width == test.expectWidth && frames->height == test.expectHeight, "the picture has the expected size");
        printf("       picture %u x %u, eye %ld, gaze %ld at (%.4f, %.4f)\n", frames->width, frames->height, frames->eye, frames->gazeValid,
               frames->gazeU, frames->gazeV);
        Check(frames->eye == test.eye && frames->gazeValid == 1, "eye and gaze are reported");
        // The fixed centre (no eye tracking): the fake headset's view is symmetric, so straight ahead is the exact middle,
        // wherever the (still valid) eye tracker points; and the block says the runtime gives eye gaze.
        if (test.noGaze) {
            float eu, ev;
            ExpectedGaze(eu, ev);
            Check(fabsf(frames->gazeU - eu) < 0.002f && fabsf(frames->gazeV - ev) < 0.002f, "a game without gaze: the ring is where the helper's gaze points");
            Check(frames->gazeSupport == 3, "and the block says the gaze comes through the helper");
            if (handedBlock) {
                // The game's own gaze appears: it has priority at once, whatever the helper hands over (straight ahead, from here).
                handedBlock->leftX = handedBlock->rightX = 0.f;
                handedBlock->leftY = handedBlock->rightY = 0.f;
                handedBlock->leftZ = handedBlock->rightZ = -1.f;
                handedBlock->sequence += 2; // An odd sequence means "being written".
                const auto oneFrame = [&] {
                    me.heartbeatMs = static_cast<LONGLONG>(GetTickCount64()); // A reader that stays quiet for too long counts as gone.
                    acquire(swapchain, nullptr, &index);
                    release(swapchain, nullptr);
                    const XrActiveActionSet active{gameSet, XR_NULL_PATH}; // The game syncs its actions, as a game does.
                    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
                    syncInfo.activeActionSets = &active;
                    syncInfo.countActiveActionSets = 1;
                    sync(session, &syncInfo);
                    endFrame(session, &frameEnd);
                    printf("       gaze %ld at (%.4f, %.4f), support %ld\n", frames->gazeValid, frames->gazeU, frames->gazeV, frames->gazeSupport);
                };
                g_noGaze = false;
                oneFrame();
                Check(fabsf(frames->gazeU - eu) < 0.002f && fabsf(frames->gazeV - ev) < 0.002f && frames->gazeSupport == 1,
                      "the game's own gaze appears: it has priority at once, and the block says the headset gives it");
                // It goes again: the helper's fills in from the next frame; the block says so after two seconds without.
                g_noGaze = true;
                oneFrame();
                Check(fabsf(frames->gazeU - 0.5f) < 0.002f && fabsf(frames->gazeV - 0.5f) < 0.002f && frames->gazeSupport == 1,
                      "the game's own gaze goes: the helper's fills in at once, and for two seconds the block still counts the game's");
                Sleep(2200);
                oneFrame();
                Check(fabsf(frames->gazeU - 0.5f) < 0.002f && frames->gazeSupport == 3, "after two seconds without, the block says the gaze comes through the helper");
                g_noGaze = false;
                oneFrame();
                Check(fabsf(frames->gazeU - eu) < 0.002f && frames->gazeSupport == 1, "and the game's own takes over again the frame it is back");
                // Back to the case's own state: no gaze from the game, the helper's where the fake eye tracker looks.
                g_noGaze = true;
                handedBlock->leftX = handedBlock->rightX = sinf(GazeYaw) * cosf(GazePitch);
                handedBlock->leftY = handedBlock->rightY = sinf(GazePitch);
                handedBlock->leftZ = handedBlock->rightZ = -cosf(GazeYaw) * cosf(GazePitch);
                handedBlock->sequence += 2;
                oneFrame();
            }
        }
        const bool fixedCentre = strstr(test.settings, "gaze_source=fixed") != nullptr;
        if (fixedCentre) {
            Check(fabsf(frames->gazeU - 0.5f) < 0.002f && fabsf(frames->gazeV - 0.5f) < 0.002f, "fixed centre: the ring sits where straight ahead lands, not where the eyes point");
            Check(frames->gazeSupport == 1, "the block says the headset gives eye gaze");
        }

        // Read it like a reader: on another device, through the shared handle.
        const LONG latest = frames->latestSlot;
        me.readingSlot = latest;
        ComPtr<ID3D11Texture2D> shared;
        const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frames->textureHandle[latest]));
        Check(SUCCEEDED(readerDevice->OpenSharedResource(handle, IID_PPV_ARGS(&shared))), "a second device opens the shared picture");
        if (shared) {
            std::wstring name(test.name, test.name + strlen(test.name));
            Check(SavePng(folder + L"\\" + name + L".png", readerDevice.Get(), readerContext.Get(), shared.Get()), "and saves it as PNG");
        }
        if (shared && test.quad) {
            // Quad views: outside the middle 40 % the picture is the wide image (the pattern's dark top left corner, blown
            // up), inside it the sharp inner image (the pattern's bright bottom right quarter).
            D3D11_TEXTURE2D_DESC desc;
            shared->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(readerDevice->CreateTexture2D(&desc, nullptr, &staging))) {
                readerContext->CopyResource(staging.Get(), shared.Get());
                if (SUCCEEDED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const bool bgra = frames->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
                    const auto redGreen = [&](float u, float v, int& r, int& g) {
                        const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + size_t(v * desc.Height) * mapped.RowPitch + size_t(u * desc.Width) * 4;
                        r = bgra ? p[2] : p[0];
                        g = p[1];
                    };
                    int r = 0, g = 0;
                    redGreen(0.52f, 0.52f, r, g); // Inner image: texture (1550, 1627) of 2000 x 2100.
                    printf("       in the middle: red %d green %d (the inner image has about 197, 197)\n", r, g);
                    Check(abs(r - 197) < 12 && abs(g - 197) < 12, "quad views: the middle of the picture is the sharp inner image");
                    redGreen(0.13f, 0.13f, r, g); // Wide image: texture (65, 68).
                    printf("       near the corner: red %d green %d (the wide image has about 8, 8)\n", r, g);
                    Check(r < 30 && g < 30, "and around it the wide image");
                    readerContext->Unmap(staging.Get(), 0);
                } else {
                    Check(false, "the published picture could be read back");
                }
            }
        }
        if (shared && fixedCentre) {
            // The ring is the sharp area: an ellipse half the image wide (fixed_focus_width) and 45 % of it tall - so its
            // rim passes 500 px to each side of the middle and 472 px below it, and NOT 472 px to the side (a round ring would).
            D3D11_TEXTURE2D_DESC desc;
            shared->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(readerDevice->CreateTexture2D(&desc, nullptr, &staging))) {
                readerContext->CopyResource(staging.Get(), shared.Get());
                if (SUCCEEDED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    const bool bgra = frames->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
                    const auto orange = [&](int x, int y) {
                        const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
                        const int r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
                        return r > 240 && g > 100 && g < 160 && b < 30;
                    };
                    const int cx = ImageWidth / 2, cy = ImageHeight / 2, rx = int(0.25f * ImageWidth), ry = int(0.225f * ImageHeight);
                    Check(orange(cx + rx, cy) && orange(cx - rx, cy) && orange(cx, cy + ry), "the ring is drawn the size of the sharp area (an ellipse)");
                    Check(!orange(cx + ry, cy) && !orange(cx, cy), "and is not a round ring of its height");
                    readerContext->Unmap(staging.Get(), 0);
                } else {
                    Check(false, "the published picture could be read back");
                }
            }
        }
        me.readingSlot = NoSlot;
        shared.Reset();

        // The crop tool's picture: ask through the app's signal block, expect both eyes in the snapshot block.
        {
            HANDLE signalMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"GazeOverlay.SettingsSignal");
            struct Signal { uint32_t magic, version; volatile LONG generation, snapshotRequest, captureKey, countdown, edgeTool; float edgeLeft, edgeRight, edgeTop, edgeBottom; };
            Signal* signal = signalMapping ? static_cast<Signal*>(MapViewOfFile(signalMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Signal))) : nullptr;
            Check(signal != nullptr, "the settings signal block exists");
            if (signal) {
                InterlockedIncrement(&signal->snapshotRequest);
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
                HANDLE snapshotMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"GazeOverlay.MirrorSnapshot");
                struct SnapshotHeader { uint32_t magic, version; volatile LONG generation; uint32_t width, height, fullWidth, fullHeight, cropX, cropY, cropWidth, cropHeight, eye, secondEye; float axisU, axisV; uint32_t axisValid; };
                const SnapshotHeader* snapshot = snapshotMapping ? static_cast<const SnapshotHeader*>(MapViewOfFile(snapshotMapping, FILE_MAP_READ, 0, 0, sizeof(SnapshotHeader))) : nullptr;
                Check(snapshot && snapshot->magic == 0x4E534F47 && snapshot->version >= 2, "the crop tool's picture block was written");
                if (snapshot) {
                    printf("       snapshot %u x %u of %u x %u, eye %u, second eye %u, crop %u,%u %u x %u\n", snapshot->width, snapshot->height,
                           snapshot->fullWidth, snapshot->fullHeight, snapshot->eye, snapshot->secondEye, snapshot->cropX, snapshot->cropY,
                           snapshot->cropWidth, snapshot->cropHeight);
                    Check(snapshot->width <= 640 && snapshot->height <= 640 && snapshot->fullWidth == (test.quad ? 2500u : ImageWidth), "it is a small copy of the whole eye image");
                    Check(snapshot->eye == uint32_t(test.eye) && snapshot->secondEye == uint32_t(1 - test.eye), "both eyes were taken in one frame");
                    // The fake headset's view is symmetric (45 degrees each way): straight ahead is the exact middle.
                    Check(snapshot->version >= 3 && snapshot->axisValid == 1 && fabsf(snapshot->axisU - 0.5f) < 0.001f && fabsf(snapshot->axisV - 0.5f) < 0.001f,
                          "the picture says where straight ahead lands");
                    // Version 4: the ring's place, after both eyes' pixels.
                    struct SnapshotTail { float gazeU, gazeV; uint32_t gazeValid; float gazeRadius; };
                    const size_t tailOffset = 64 + 2 * size_t(640) * 640 * 4;
                    const uint8_t* whole = static_cast<const uint8_t*>(MapViewOfFile(snapshotMapping, FILE_MAP_READ, 0, 0, tailOffset + sizeof(SnapshotTail)));
                    const SnapshotTail* tail = whole ? reinterpret_cast<const SnapshotTail*>(whole + tailOffset) : nullptr;
                    Check(snapshot->version >= 4 && tail && tail->gazeValid == 1 && tail->gazeRadius > 0.04f, "the picture carries the ring's place and size");
                    if (tail && !fixedCentre && strstr(test.settings, "crop_enabled=0")) {
                        float eu, ev;
                        ExpectedGaze(eu, ev);
                        printf("       ring in the picture at (%.4f, %.4f), expected (%.4f, %.4f)\n", tail->gazeU, tail->gazeV, eu, ev);
                        Check(fabsf(tail->gazeU - eu) < 0.01f && fabsf(tail->gazeV - ev) < 0.01f, "and the ring is where the eyes point");
                    }
                    if (whole) UnmapViewOfFile(whole);
                    UnmapViewOfFile(snapshot);
                }
                if (snapshotMapping) CloseHandle(snapshotMapping);
                // The capture countdown and the edge finder: with 7 seconds left and the left bar being set at 12.3 %,
                // the next frame carries a big "7" and the four bars in the headset image.
                Check(signal->version >= 5, "the signal block carries the countdown and edge-finder fields");
                signal->countdown = 7;
                signal->edgeLeft = 0.123f; signal->edgeRight = 0.877f; signal->edgeTop = 0.123f; signal->edgeBottom = 0.877f;
                signal->edgeTool = 1;
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
                signal->countdown = 0;
                signal->edgeTool = 0;
                UnmapViewOfFile(signal);
            }
            if (signalMapping) CloseHandle(signalMapping);
        }

        // The calibration marker: the game's own image now carries the bracket reticle at the gaze point (read back on
        // the D3D11 game device; the D3D12 case exercises the draw into a wrapped image but is not read back here).
        if (strstr(test.settings, "headset_marker=1") && !test.d3d12) {
            D3D11_TEXTURE2D_DESC stagingDesc{};
            imageA->GetDesc(&stagingDesc);
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            gameDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
            ID3D11Texture2D* lastImage = g_swapchainImages[(g_acquireCounter - 1) % g_swapchainImages.size()];
            gameContext->CopyResource(staging.Get(), lastImage);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            bool orangeFound = false;
            if (SUCCEEDED(gameContext->Map(staging.Get(), D3D11CalcSubresource(0, test.eye, 1), D3D11_MAP_READ, 0, &mapped))) {
                float gu, gv;
                ExpectedGaze(gu, gv);
                const int gx = int(gu * ImageWidth), gy = int(gv * ImageHeight);
                const int r = int(0.05f * ImageHeight); // The reticle box's half-size (the ring's radius).
                // The bracket arms run along the box's top edge, from each corner half way in.
                for (int dx = -r - 3; dx <= r + 3 && !orangeFound; dx++) {
                    const int x = gx + dx, y = gy - r;
                    if (x < 0 || x >= int(ImageWidth) || y < 0) continue;
                    const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
                    // R8G8B8A8_UNORM_SRGB swapchain: the marker colour (sRGB 255,128,0) lands as roughly 255,128,0 bytes.
                    if (p[0] > 200 && p[1] > 90 && p[1] < 170 && p[2] < 60 && abs(dx) >= r / 2 - 2) orangeFound = true;
                }
                // The countdown digit: pure white pixels around the middle of the image, off the pattern's grid lines.
                int white = 0;
                const int cx = int(ImageWidth) / 2, cy = int(ImageHeight) / 2, reach = int(0.14f * ImageHeight);
                for (int y = cy - reach; y < cy + reach; y++) {
                    for (int x = cx - reach; x < cx + reach; x++) {
                        if (x % 100 == 0 || y % 100 == 0) continue;
                        const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + size_t(x) * 4;
                        if (p[0] > 235 && p[1] > 235 && p[2] > 235) white++; // 0.9 alpha over the edge finder's black: 243.
                    }
                }
                printf("       %d white pixels in the middle (the countdown digit)\n", white);
                Check(white > 300, "the capture countdown was drawn into the headset's image");
                // The edge finder: left of the left edge (12.3 %) a light block (the edge being set, white at some
                // point of its flash), right of it black where the pattern was, and beyond the right edge (87.7 %) the
                // grey of an edge that stands.
                int lightLeft = 0, blackInside = 0, greyRight = 0;
                const int xLeft = int(0.06f * ImageWidth), xInside = int(0.5f * ImageWidth), xRight = int(0.94f * ImageWidth);
                for (int y = int(ImageHeight) * 3 / 10; y < int(ImageHeight) * 7 / 10; y++) {
                    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    const uint8_t* l = row + size_t(xLeft) * 4;
                    const uint8_t* m = row + size_t(xInside) * 4;
                    const uint8_t* r = row + size_t(xRight) * 4;
                    // The image is sRGB: linear 0.25 comes back as 137, 0.45 (the flash's low) as 180.
                    if (l[0] > 170 && l[0] == l[1] && l[1] == l[2]) lightLeft++;
                    if (m[0] < 8 && m[1] < 8 && m[2] < 8) blackInside++;
                    if (r[0] > 125 && r[0] < 150 && r[0] == r[1] && r[1] == r[2]) greyRight++;
                }
                printf("       edge finder: %d light pixels outside the left edge, %d black inside, %d grey outside the right edge\n", lightLeft, blackInside, greyRight);
                Check(lightLeft > 700 && blackInside > 700 && greyRight > 700, "the edge finder covers the headset's image: a light block outside the edge being set, black inside, grey outside the others");
                gameContext->Unmap(staging.Get(), D3D11CalcSubresource(0, test.eye, 1));
            }
            Check(orangeFound, "the calibration marker was drawn into the headset's image");
        }

        // The edge finder covered one of the game's images: the pattern goes back in, for the checks that read it.
        if (!test.d3d12) {
            std::vector<uint8_t> pixels;
            for (ID3D11Texture2D* image : g_swapchainImages) {
                for (uint32_t slice = 0; slice < test.arraySize; slice++) {
                    FillPattern(pixels, slice);
                    gameContext->UpdateSubresource(image, D3D11CalcSubresource(0, slice, 1), nullptr, pixels.data(), ImageWidth * 4, 0);
                }
            }
        }

        // A game that stops syncing its actions (or never did): after twenty such frames ours is synced from the layer.
        for (int i = 0; i < 24; i++) {
            acquire(swapchain, nullptr, &index);
            release(swapchain, nullptr);
            endFrame(session, &frameEnd);
        }
        Check(g_syncedSets == 1, "a game that does not sync its actions: the eye-gaze action is synced by the layer");
        {
            const XrActiveActionSet active{gameSet, XR_NULL_PATH};
            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.activeActionSets = &active;
            syncInfo.countActiveActionSets = 1;
            sync(session, &syncInfo);
            Check(g_syncedSets == 2, "and when the game syncs again, both travel together");
        }
        // Steadying against a tilted head: the head (the pose the game rendered with) rolls by three degrees from one
        // frame to the next, and the smoothed orientation - a filter as slow as it goes - stays where it was. The box
        // must turn by those three degrees about its middle, so that the picture shows what the level head saw. The
        // pattern does not move with the head here, so the turn shows in it: red rises with x, green with y.
        if (!strcmp(test.name, "steady-tilt")) {
            std::vector<uint8_t> picture;
            uint32_t pictureWidth = 0, pictureHeight = 0, picturePitch = 0;
            const auto readLatest = [&]() {
                const LONG latest = frames->latestSlot;
                me.readingSlot = latest;
                ComPtr<ID3D11Texture2D> shared, staging;
                const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frames->textureHandle[latest]));
                if (FAILED(readerDevice->OpenSharedResource(handle, IID_PPV_ARGS(&shared)))) return false;
                D3D11_TEXTURE2D_DESC desc;
                shared->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(readerDevice->CreateTexture2D(&desc, nullptr, &staging))) return false;
                readerContext->CopyResource(staging.Get(), shared.Get());
                if (FAILED(readerContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
                pictureWidth = desc.Width;
                pictureHeight = desc.Height;
                picturePitch = mapped.RowPitch;
                picture.assign(static_cast<const uint8_t*>(mapped.pData), static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * desc.Height);
                readerContext->Unmap(staging.Get(), 0);
                return true;
            };
            // Red and green at a place of the picture - the nearest pixel that is neither a white grid line nor the ring.
            const bool bgra = frames->dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
            const auto redGreen = [&](float u, float v, int& r, int& g) {
                for (int step = 0; step < 24; step++) {
                    const uint32_t x = std::min(uint32_t(u * pictureWidth) + step, pictureWidth - 1), y = std::min(uint32_t(v * pictureHeight) + step, pictureHeight - 1);
                    const uint8_t* p = &picture[size_t(y) * picturePitch + size_t(x) * 4];
                    r = bgra ? p[2] : p[0];
                    g = p[1];
                    const int b = bgra ? p[0] : p[2];
                    if (b >= 87 && b <= 93) return; // The pattern's own blue (90): not a grid line, not the ring, not something drawn over it.
                }
            };
            const auto frameWith = [&](float rollRadians) {
                for (int i = 0; i < 4; i++) views[i].pose.orientation = {0, 0, sinf(rollRadians / 2), cosf(rollRadians / 2)};
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
            };
            const auto slopes = [&](int& greenAcross, int& redDown) {
                int r0, g0, r1, g1;
                redGreen(0.1f, 0.3f, r0, g0);
                redGreen(0.9f, 0.3f, r1, g1);
                greenAcross = g1 - g0;
                redGreen(0.47f, 0.1f, r0, g0);
                redGreen(0.47f, 0.9f, r1, g1);
                redDown = r1 - r0;
            };
            const float tilt = 3.f * 3.14159265f / 180.f;
            int levelGreen = 99, levelRed = 99, tiltedGreen = 0, tiltedRed = 0, backGreen = 99;
            frameWith(0.f);
            frameWith(0.f);
            const bool readLevel = readLatest();
            if (readLevel) slopes(levelGreen, levelRed);
            frameWith(0.f); // Close in time to the next one: a long gap would make the steadying start afresh.
            frameWith(tilt);
            const bool readTilted = readLatest();
            if (readTilted) slopes(tiltedGreen, tiltedRed);
            // The box is 1866 x 1050 of the 2000 x 2100 image. Turned by three degrees, a row of the picture runs
            // 0.8 x 1866 x sin(3) = 78 image rows downhill between its 10 % and its 90 % mark - 9.5 steps of green - and a
            // column runs 44 image columns to the left between the same marks - 5.6 steps of red.
            printf("       level: green across %d, red down %d; head tilted by 3 degrees: green across %d, red down %d (a turned box gives about +9, -6)\n",
                   levelGreen, levelRed, tiltedGreen, tiltedRed);
            Check(readLevel && abs(levelGreen) <= 1 && abs(levelRed) <= 1, "steadying, head level: the picture is level");
            Check(readTilted && tiltedGreen >= 6 && tiltedGreen <= 13 && tiltedRed <= -3 && tiltedRed >= -9,
                  "steadying, head tilted from one frame to the next: the box turns against the tilt by the same angle");
            // A tilt far larger than the room allows: the box turns as far as the room lets it and stays inside the image.
            frameWith(0.f);
            frameWith(0.f);
            frameWith(25.f * 3.14159265f / 180.f);
            int farGreen = 0, farRed = 0;
            const bool readFar = readLatest();
            if (readFar) slopes(farGreen, farRed);
            // The room is 10 % of the image, which would allow a sine of 0.225; but the box stands 67 columns from the
            // image's sides, and a turned box's corners move out sideways by half its height (525) times the sine: 0.128
            // is as far as it may turn without leaving the image. 0.8 x 1866 x 0.128 = 190 rows = 23 steps of green.
            printf("       head tilted by 25 degrees: green across %d, red down %d (the image's edge allows about 23, -14; 25 degrees unchecked would be 77)\n", farGreen, farRed);
            Check(readFar && farGreen >= 19 && farGreen <= 27 && farRed <= -10 && farRed >= -17, "a tilt beyond what fits: the box turns as far as it can without leaving the image");
            frameWith(0.f);
            (void)backGreen;
            for (int i = 0; i < 4; i++) views[i].pose.orientation = {0, 0, 0, 1};
        }
        // The desktop cover's hotkey: with a key and its modifiers in the app's signal block, that combination is seen by
        // the layer and announced once per press. Really pressed here - Ctrl + Alt + F23, a key no keyboard has - for a moment.
        if (!strcmp(test.name, "plain") && !appListening) {
            struct CoverSignal { uint32_t magic, version; volatile LONG generation, snapshotRequest, captureKey, countdown, edgeTool; float edges[4]; volatile LONG coverKey; };
            HANDLE signalMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"GazeOverlay.SettingsSignal");
            CoverSignal* signal = signalMapping ? static_cast<CoverSignal*>(MapViewOfFile(signalMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CoverSignal))) : nullptr;
            HANDLE coverKey = CreateEventW(nullptr, FALSE, FALSE, CoverKeyName);
            const auto runFrame = [&] {
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
            };
            // The modifiers go down before the key and come up after it; second = 0: one modifier only.
            const auto keys = [](bool down, WORD first = VK_CONTROL, WORD second = VK_MENU) {
                INPUT inputs[3]{};
                WORD order[3]{};
                int count = 0;
                order[count++] = first;
                if (second) order[count++] = second;
                order[count++] = VK_F23;
                for (int i = 0; i < count; i++) {
                    inputs[i].type = INPUT_KEYBOARD;
                    inputs[i].ki.wVk = down ? order[i] : order[count - 1 - i];
                    // The right-hand Ctrl and Alt are extended keys.
                    const bool extended = inputs[i].ki.wVk == VK_RCONTROL || inputs[i].ki.wVk == VK_RMENU;
                    inputs[i].ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
                }
                SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
            };
            const auto otherKeysHeld = [] { return ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0; };
            if (!signal || signal->version < 6) {
                Check(false, "the signal block carries the desktop cover's key");
            } else if (otherKeysHeld()) {
                printf("  note  Shift or the Windows key is held down right now - the cover hotkey check was skipped\n");
            } else {
                ResetEvent(coverKey);
                keys(true);
                runFrame();
                const bool unasked = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false);
                // Asked for with Ctrl alone: Ctrl + Alt + the key is another combination.
                signal->coverKey = VK_F23 | 0x200;
                runFrame();
                keys(true);
                runFrame();
                const bool otherCombination = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false);
                signal->coverKey = VK_F23 | 0x200 | 0x100; // Ctrl + Alt + F23.
                runFrame(); // Key up: armed.
                keys(true);
                runFrame();
                const bool announced = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                runFrame(); // Still held: not announced again.
                const bool repeated = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false);
                runFrame();
                // A side asked for: RCtrl + LAlt + F23 (Ctrl 0x200 with its right key 0x20000, Alt 0x100 with its left key 0x1000).
                signal->coverKey = VK_F23 | 0x200 | 0x20000 | 0x100 | 0x1000;
                runFrame();
                keys(true, VK_LCONTROL, VK_LMENU);
                runFrame();
                const bool otherSide = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false, VK_LCONTROL, VK_LMENU);
                runFrame();
                keys(true, VK_RCONTROL, VK_LMENU);
                runFrame();
                const bool rightSide = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false, VK_RCONTROL, VK_LMENU);
                runFrame();
                // Both keys of a modifier asked for: LCtrl + RCtrl + F23 (Ctrl 0x200 with its left key 0x2000 and its right key 0x20000).
                signal->coverKey = VK_F23 | 0x200 | 0x2000 | 0x20000;
                runFrame();
                keys(true, VK_LCONTROL, 0);
                runFrame();
                const bool oneOfBoth = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false, VK_LCONTROL, 0);
                runFrame();
                keys(true, VK_LCONTROL, VK_RCONTROL);
                runFrame();
                const bool bothSides = WaitForSingleObject(coverKey, 0) == WAIT_OBJECT_0;
                keys(false, VK_LCONTROL, VK_RCONTROL);
                runFrame();
                signal->coverKey = 0;
                if (otherKeysHeld()) {
                    printf("  note  Shift or the Windows key went down during the cover hotkey check - not judged\n");
                } else {
                    Check(!unasked, "with no cover key asked for, the keyboard is not looked at");
                    Check(!otherCombination, "a hotkey with other modifiers than the ones pressed is not that hotkey");
                    Check(announced, "the desktop cover's hotkey, pressed while the game runs, is announced to the app");
                    Check(!repeated, "once per press");
                    Check(!otherSide, "with RCtrl + LAlt asked for, LCtrl + LAlt is not that hotkey");
                    Check(rightSide, "and RCtrl + LAlt is");
                    Check(!oneOfBoth, "with LCtrl + RCtrl asked for, LCtrl alone is not that hotkey");
                    Check(bothSides, "and both Ctrl keys together are");
                }
            }
            if (signal) UnmapViewOfFile(signal);
            if (signalMapping) CloseHandle(signalMapping);
            CloseHandle(coverKey);
        }
        handing = false;
        if (helper.joinable()) helper.join();
        if (handedMapping) CloseHandle(handedMapping);
        g_noGaze = false;
        destroySwapchain(swapchain);
        destroySession(session);
        const ULONG referencesAfter = (watched->AddRef(), watched->Release());
        Check(referencesAfter == referencesBefore, "no reference to the game's images is kept after the session");
        Check(frames->producerKind == ProducerNone && frames->latestSlot == NoSlot, "the producer signs off");
        if (!appListening) Check(WaitForSingleObject(producerChanged, 0) == WAIT_OBJECT_0, "and its leaving is announced");
        CloseHandle(producerChanged);
        InterlockedExchange(&me.pid, 0);
        destroyInstance(instance);
        CloseHandle(frameReady);
        // The block itself stays mapped until the end of the process so that every case sees a used, not a fresh, block.
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
