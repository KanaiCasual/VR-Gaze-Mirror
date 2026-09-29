#include "pch.h"

#include "../core/log.h"
#include "gaze_input.h"

namespace gaze_mirror::layer {

    namespace {
        constexpr int FramesBeforeAttachingOnOurOwn = 300;
        const XrPosef Identity = {{0, 0, 0, 1}, {0, 0, 0}};
    } // namespace

    void GazeInput::onInstance(const Next* next, bool extensionEnabled) {
        _next = next;
        _extension = extensionEnabled;
        Log("gaze: XR_EXT_eye_gaze_interaction is %s", extensionEnabled ? "part of this instance" : "NOT available - no ring");
    }

    void GazeInput::onSessionCreated(XrSession session, XrSystemId systemId) {
        _session = session;
        _attached = _attachFailed = _selfAttached = _gameSyncs = false;
        _framesWithoutAttach = 0;
        _framesSinceGameSync = 0;
        _syncingOurselves = false;
        _logLocate = 0;
        _supported = false;
        if (!_extension) return;

        XrSystemEyeGazeInteractionPropertiesEXT eyeProperties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eyeProperties};
        const XrResult result = _next->xrGetSystemProperties(_next->instance, systemId, &properties);
        _supported = XR_SUCCEEDED(result) && eyeProperties.supportsEyeGazeInteraction;
        Log("gaze: system \"%s\" %s eye gaze", XR_SUCCEEDED(result) ? properties.systemName : "?",
            _supported ? "supports" : "does NOT support");
        if (!_supported || !createActions()) return;

        XrActionSpaceCreateInfo actionSpace{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        actionSpace.action = _action;
        actionSpace.poseInActionSpace = Identity;
        XrReferenceSpaceCreateInfo viewSpace{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewSpace.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewSpace.poseInReferenceSpace = Identity;
        const XrResult first = _next->xrCreateActionSpace(session, &actionSpace, &_gazeSpace);
        const XrResult second = _next->xrCreateReferenceSpace(session, &viewSpace, &_viewSpace);
        if (XR_FAILED(first) || XR_FAILED(second)) {
            Log("gaze: spaces could not be created (%d, %d)", first, second);
            _supported = false;
        }
    }

    void GazeInput::onSessionDestroyed() {
        // The spaces die with the session; the action set belongs to the instance and is used again.
        _gazeSpace = _viewSpace = XR_NULL_HANDLE;
        _session = XR_NULL_HANDLE;
        _attached = _selfAttached = false;
    }

    bool GazeInput::createActions() {
        if (_action != XR_NULL_HANDLE) return true;
        _next->xrStringToPath(_next->instance, "/interaction_profiles/ext/eye_gaze_interaction", &_profilePath);
        _next->xrStringToPath(_next->instance, "/user/eyes_ext/input/gaze_ext/pose", &_gazePath);

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "gaze_mirror");
        strcpy_s(setInfo.localizedActionSetName, "Gaze Mirror");
        XrResult result = _next->xrCreateActionSet(_next->instance, &setInfo, &_actionSet);
        if (XR_FAILED(result)) {
            Log("gaze: action set could not be created (%d)", result);
            return false;
        }
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(actionInfo.actionName, "eye_gaze");
        strcpy_s(actionInfo.localizedActionName, "Eye gaze");
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        result = _next->xrCreateAction(_actionSet, &actionInfo, &_action);
        if (XR_FAILED(result)) {
            Log("gaze: action could not be created (%d)", result);
            _next->xrDestroyActionSet(_actionSet);
            _actionSet = XR_NULL_HANDLE;
            _action = XR_NULL_HANDLE;
            return false;
        }
        return true;
    }

    XrResult GazeInput::suggestBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
        if (_action == XR_NULL_HANDLE || !suggested || suggested->interactionProfile != _profilePath) {
            return _next->xrSuggestInteractionProfileBindings(instance, suggested);
        }
        // Somebody else uses the eye tracker too: our binding travels with theirs, or it would replace theirs.
        std::vector<XrActionSuggestedBinding> bindings(suggested->suggestedBindings,
                                                       suggested->suggestedBindings + suggested->countSuggestedBindings);
        bindings.push_back({_action, _gazePath});
        XrInteractionProfileSuggestedBinding merged = *suggested;
        merged.suggestedBindings = bindings.data();
        merged.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        const XrResult result = _next->xrSuggestInteractionProfileBindings(instance, &merged);
        if (XR_SUCCEEDED(result)) {
            _suggested = true;
            Log("gaze: binding added to another component's eye-gaze suggestion (%u of theirs)", suggested->countSuggestedBindings);
            return result;
        }
        Log("gaze: the combined suggestion was refused (%d) - passing on the original", result);
        return _next->xrSuggestInteractionProfileBindings(instance, suggested);
    }

    XrResult GazeInput::attachActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
        if (_action == XR_NULL_HANDLE || !_supported || !info || session != _session || _attached) {
            return _next->xrAttachSessionActionSets(session, info);
        }
        if (_selfAttached) {
            Log("gaze: WARNING - the game attaches its input late, after this layer attached on its own; its call will be refused");
            return _next->xrAttachSessionActionSets(session, info);
        }
        if (!_suggested) {
            const XrActionSuggestedBinding binding{_action, _gazePath};
            XrInteractionProfileSuggestedBinding own{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            own.interactionProfile = _profilePath;
            own.suggestedBindings = &binding;
            own.countSuggestedBindings = 1;
            const XrResult result = _next->xrSuggestInteractionProfileBindings(_next->instance, &own);
            if (XR_FAILED(result)) {
                Log("gaze: our suggestion was refused (%d) - no gaze in this session", result);
                _attachFailed = true;
                return _next->xrAttachSessionActionSets(session, info);
            }
            _suggested = true;
        }
        std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
        sets.push_back(_actionSet);
        XrSessionActionSetsAttachInfo merged = *info;
        merged.actionSets = sets.data();
        merged.countActionSets = static_cast<uint32_t>(sets.size());
        const XrResult result = _next->xrAttachSessionActionSets(session, &merged);
        if (XR_SUCCEEDED(result)) {
            _attached = true;
            Log("gaze: attached together with %u action set(s) of the game", info->countActionSets);
            return result;
        }
        Log("gaze: the combined attach was refused (%d) - passing on the original, no gaze in this session", result);
        _attachFailed = true;
        return _next->xrAttachSessionActionSets(session, info);
    }

    XrResult GazeInput::syncActions(XrSession session, const XrActionsSyncInfo* info) {
        if (!_attached || _selfAttached || !info || session != _session) {
            return _next->xrSyncActions(session, info);
        }
        _gameSyncs = true;
        _framesSinceGameSync = 0;
        if (_syncingOurselves) {
            _syncingOurselves = false;
            Log("gaze: the game syncs its actions again - ours travel along");
        }
        std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
        sets.push_back({_actionSet, XR_NULL_PATH});
        XrActionsSyncInfo merged = *info;
        merged.activeActionSets = sets.data();
        merged.countActiveActionSets = static_cast<uint32_t>(sets.size());
        const XrResult result = _next->xrSyncActions(session, &merged);
        if (XR_FAILED(result)) return _next->xrSyncActions(session, info);
        return result;
    }

    void GazeInput::attachOnOurOwn() {
        const XrActionSuggestedBinding binding{_action, _gazePath};
        XrInteractionProfileSuggestedBinding own{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        own.interactionProfile = _profilePath;
        own.suggestedBindings = &binding;
        own.countSuggestedBindings = 1;
        XrResult result = _suggested ? XR_SUCCESS : _next->xrSuggestInteractionProfileBindings(_next->instance, &own);
        if (XR_SUCCEEDED(result)) {
            _suggested = true;
            XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.actionSets = &_actionSet;
            attach.countActionSets = 1;
            result = _next->xrAttachSessionActionSets(_session, &attach);
        }
        if (XR_SUCCEEDED(result)) {
            _attached = _selfAttached = true;
            Log("gaze: the game set up no input of its own in %d frames - attached on our own", FramesBeforeAttachingOnOurOwn);
        } else {
            _attachFailed = true;
            Log("gaze: attaching on our own was refused (%d) - no gaze in this session", result);
        }
    }

    GazeSample GazeInput::locate(XrTime time) {
        GazeSample sample;
        if (!_supported || _action == XR_NULL_HANDLE || _gazeSpace == XR_NULL_HANDLE || _attachFailed) return sample;
        if (!_attached) {
            if (++_framesWithoutAttach > FramesBeforeAttachingOnOurOwn) attachOnOurOwn();
            return sample;
        }
        // A game may attach its action sets and then not sync them - a sim driven by wheel and pedals has no use for
        // them. Ours, attached along with them, then goes unsynced too, and a runtime that takes the rule seriously
        // (SteamVR's) has no gaze for an action that is not synced. After twenty frames without the game's sync, ours
        // is synced from here; the game's own sets are left out of that, which costs a game that does not read them
        // nothing, and one that starts syncing again takes over at once.
        constexpr int FramesBeforeSyncingOurselves = 20;
        if (!_selfAttached && !_syncingOurselves && ++_framesSinceGameSync > FramesBeforeSyncingOurselves) {
            _syncingOurselves = true;
            Log("gaze: the game does not sync its actions - the eye-gaze action is synced from here");
        }
        if (_selfAttached || _syncingOurselves) {
            // Nobody else syncs for us.
            const XrActiveActionSet active{_actionSet, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.activeActionSets = &active;
            sync.countActiveActionSets = 1;
            _next->xrSyncActions(_session, &sync);
        }
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult result = _next->xrLocateSpace(_gazeSpace, _viewSpace, time, &location);
        constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        sample.valid = XR_SUCCEEDED(result) && (location.locationFlags & needed) == needed;
        sample.pose = location.pose;
        if (!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) sample.pose.position = {0, 0, 0};
        LogFewTimes(_logLocate, 5, "gaze: locate -> %d, flags 0x%llx, direction quaternion (%.3f, %.3f, %.3f, %.3f)", result,
                    static_cast<unsigned long long>(location.locationFlags), location.pose.orientation.x,
                    location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w);
        return sample;
    }

} // namespace gaze_mirror::layer
