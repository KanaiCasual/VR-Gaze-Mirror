// VR Gaze Mirror 2.0 - OpenXR layer: eye gaze through the standard extension XR_EXT_eye_gaze_interaction.
//
// An OpenXR session accepts its input set-up exactly once (xrAttachSessionActionSets), and the suggested bindings of an
// interaction profile are replaced by whoever suggests last. So this never makes calls of its own where the game - or
// a layer above us, such as Quad-Views-Foveated - makes them: it adds its one action to THEIR calls on the way through.
// Only when nobody has attached anything after a good while does it attach on its own.
// Whatever goes wrong here, the game's own call is passed on unchanged: gaze is lost, the game's input never is.
#pragma once

#include "next.h"

namespace gaze_mirror::layer {

    using gaze_mirror::Log;
    using gaze_mirror::LogFewTimes;

    struct GazeSample {
        bool valid = false;
        XrPosef pose{}; // In VIEW space (the head): where the gaze starts and which way it points (-Z of the pose).
    };

    class GazeInput {
      public:
        // extensionEnabled: XR_EXT_eye_gaze_interaction is part of this instance.
        void onInstance(const Next* next, bool extensionEnabled);
        void onSessionCreated(XrSession session, XrSystemId systemId);
        void onSessionDestroyed();

        // The three calls that are passed through us.
        XrResult suggestBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested);
        XrResult attachActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info);
        XrResult syncActions(XrSession session, const XrActionsSyncInfo* info);

        // Once per frame, while somebody wants the picture.
        GazeSample locate(XrTime time);
        XrSpace viewSpace() const {
            return _viewSpace;
        }
        // The runtime says this system gives eye gaze (asked when the session was made).
        bool supported() const {
            return _supported;
        }

      private:
        bool createActions();
        void attachOnOurOwn();

        const Next* _next = nullptr;
        bool _extension = false;
        bool _supported = false;
        XrSession _session = XR_NULL_HANDLE;

        XrActionSet _actionSet = XR_NULL_HANDLE;
        XrAction _action = XR_NULL_HANDLE;
        XrPath _profilePath = XR_NULL_PATH;
        XrPath _gazePath = XR_NULL_PATH;
        XrSpace _gazeSpace = XR_NULL_HANDLE;
        XrSpace _viewSpace = XR_NULL_HANDLE;

        bool _suggested = false;    // Our binding is part of a suggestion the runtime has accepted.
        bool _attached = false;     // Our action set is attached to the session.
        bool _attachFailed = false; // Somebody attached without us: no gaze in this session.
        bool _selfAttached = false;
        bool _gameSyncs = false;    // xrSyncActions passes through us, so our set is synced along.
        int _framesWithoutAttach = 0;
        int _logLocate = 0;
    };

} // namespace gaze_mirror::layer
