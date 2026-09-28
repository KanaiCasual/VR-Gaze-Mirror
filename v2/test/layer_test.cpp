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
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
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
        *count = static_cast<uint32_t>(g_swapchainImages.size());
        if (capacity == 0) return XR_SUCCESS;
        auto* d3dImages = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < *count && i < capacity; i++) d3dImages[i].texture = g_swapchainImages[i];
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL StubAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
        *index = g_acquireCounter++ % static_cast<uint32_t>(g_swapchainImages.size());
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
        location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
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
        float gazeU, gazeV;
        ExpectedGaze(gazeU, gazeV);
        const int gx = int(gazeU * ImageWidth), gy = int(gazeV * ImageHeight);
        std::vector<uint8_t> pixels(size_t(ImageWidth) * ImageHeight * 4);
        for (uint32_t slice = 0; slice < arraySize; slice++) {
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

    const Case cases[] = {
        {"plain", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\nmirror_eye=left\n", 1, true, 0, 2000, 2100},
        {"crop", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=1\ncrop_aspect=16:9\ncrop_height=0.5\ncrop_center_x=0.6\ncrop_center_y=0.45\nmirror_eye=left\n", 1, true, 0, 1866, 1050},
        {"copy-path", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=1\ncrop_aspect=16:9\ncrop_height=0.5\ncrop_center_x=0.6\ncrop_center_y=0.45\nmirror_eye=left\n", 1, false, 0, 1866, 1050},
        {"array-right-eye", "enabled=1\nstyle=ring\nblend=normal\nradius=0.05\nthickness=0.006\nfeather=0.001\nopacity=1\nshadow_opacity=0\ncolor=255,128,0\nfilter=simple\nsmoothing_ms=0\nfade_ms=0\ndwell_ms=0\ntail_space=screen\ncrop_enabled=0\noutput_max_side=1000\nheadset_marker=1\n", 2, true, 1, 952, 1000},
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

        ComPtr<ID3D11Texture2D> imageA = MakePattern(gameDevice.Get(), gameContext.Get(), test.arraySize, test.sampleable);
        ComPtr<ID3D11Texture2D> imageB = MakePattern(gameDevice.Get(), gameContext.Get(), test.arraySize, test.sampleable);
        g_swapchainImages = {imageA.Get(), imageB.Get()};
        const ULONG referencesBefore = (imageA->AddRef(), imageA->Release());

        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = gameDevice.Get();
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding};
        sessionInfo.systemId = 1;
        XrSession session = XR_NULL_HANDLE;
        Check(XR_SUCCEEDED(createSession(instance, &sessionInfo, &session)), "session is created");
        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
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
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
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

        XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int i = 0; i < 2; i++) {
            views[i].pose = {{0, 0, 0, 1}, {0, 0, 0}};
            views[i].fov = {-HalfFov, HalfFov, HalfFov, -HalfFov};
            views[i].subImage.swapchain = swapchain;
            views[i].subImage.imageRect = {{0, 0}, {int32_t(ImageWidth), int32_t(ImageHeight)}};
            views[i].subImage.imageArrayIndex = test.arraySize > 1 ? i : 0;
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 2;
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
        me.readingSlot = NoSlot;
        shared.Reset();

        // The crop tool's picture: ask through the app's signal block, expect both eyes in the snapshot block.
        {
            HANDLE signalMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"GazeOverlay.SettingsSignal");
            struct Signal { uint32_t magic, version; volatile LONG generation, snapshotRequest, captureKey, countdown; };
            Signal* signal = signalMapping ? static_cast<Signal*>(MapViewOfFile(signalMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Signal))) : nullptr;
            Check(signal != nullptr, "the settings signal block exists");
            if (signal) {
                InterlockedIncrement(&signal->snapshotRequest);
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
                HANDLE snapshotMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"GazeOverlay.MirrorSnapshot");
                struct SnapshotHeader { uint32_t magic, version; volatile LONG generation; uint32_t width, height, fullWidth, fullHeight, cropX, cropY, cropWidth, cropHeight, eye, secondEye; };
                const SnapshotHeader* snapshot = snapshotMapping ? static_cast<const SnapshotHeader*>(MapViewOfFile(snapshotMapping, FILE_MAP_READ, 0, 0, sizeof(SnapshotHeader))) : nullptr;
                Check(snapshot && snapshot->magic == 0x4E534F47 && snapshot->version == 2, "the crop tool's picture block was written");
                if (snapshot) {
                    printf("       snapshot %u x %u of %u x %u, eye %u, second eye %u, crop %u,%u %u x %u\n", snapshot->width, snapshot->height,
                           snapshot->fullWidth, snapshot->fullHeight, snapshot->eye, snapshot->secondEye, snapshot->cropX, snapshot->cropY,
                           snapshot->cropWidth, snapshot->cropHeight);
                    Check(snapshot->width <= 640 && snapshot->height <= 640 && snapshot->fullWidth == ImageWidth, "it is a small copy of the whole eye image");
                    Check(snapshot->eye == uint32_t(test.eye) && snapshot->secondEye == uint32_t(1 - test.eye), "both eyes were taken in one frame");
                    UnmapViewOfFile(snapshot);
                }
                if (snapshotMapping) CloseHandle(snapshotMapping);
                // The capture countdown: with 7 seconds left, the next frame carries a big "7" in the headset image.
                Check(signal->version >= 4, "the signal block carries the countdown field");
                signal->countdown = 7;
                acquire(swapchain, nullptr, &index);
                release(swapchain, nullptr);
                endFrame(session, &frameEnd);
                signal->countdown = 0;
                UnmapViewOfFile(signal);
            }
            if (signalMapping) CloseHandle(signalMapping);
        }

        // The calibration marker (last case): the game's own image now carries the bracket reticle at the gaze point.
        if (strstr(test.settings, "headset_marker=1")) {
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
                        if (p[0] > 245 && p[1] > 245 && p[2] > 245) white++;
                    }
                }
                printf("       %d white pixels in the middle (the countdown digit)\n", white);
                Check(white > 300, "the capture countdown was drawn into the headset's image");
                gameContext->Unmap(staging.Get(), D3D11CalcSubresource(0, test.eye, 1));
            }
            Check(orangeFound, "the calibration marker was drawn into the headset's image");
        }

        destroySwapchain(swapchain);
        destroySession(session);
        const ULONG referencesAfter = (imageA->AddRef(), imageA->Release());
        Check(referencesAfter == referencesBefore, "no reference to the game's images is kept after the session");
        Check(frames->producerKind == ProducerNone && frames->latestSlot == NoSlot, "the producer signs off");
        InterlockedExchange(&me.pid, 0);
        destroyInstance(instance);
        CloseHandle(frameReady);
        // The block itself stays mapped until the end of the process so that every case sees a used, not a fresh, block.
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
