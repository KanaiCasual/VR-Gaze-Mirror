// VR Gaze Mirror 2.0 - OpenXR layer: the functions of whatever comes after us (the next layer or the runtime).
#pragma once

namespace gaze_mirror::layer {

    struct Next {
        XrInstance instance = XR_NULL_HANDLE;
        PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;

        PFN_xrDestroyInstance xrDestroyInstance = nullptr;
        PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
        PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
        PFN_xrCreateSession xrCreateSession = nullptr;
        PFN_xrDestroySession xrDestroySession = nullptr;
        PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
        PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
        PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
        PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
        PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
        PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
        PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
        PFN_xrEndFrame xrEndFrame = nullptr;
        PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
        PFN_xrDestroySpace xrDestroySpace = nullptr;
        PFN_xrLocateSpace xrLocateSpace = nullptr;
        PFN_xrLocateViews xrLocateViews = nullptr;
        PFN_xrStringToPath xrStringToPath = nullptr;
        PFN_xrCreateActionSet xrCreateActionSet = nullptr;
        PFN_xrDestroyActionSet xrDestroyActionSet = nullptr;
        PFN_xrCreateAction xrCreateAction = nullptr;
        PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
        PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
        PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
        PFN_xrSyncActions xrSyncActions = nullptr;
    };

} // namespace gaze_mirror::layer
