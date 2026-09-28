// VR Gaze Mirror 2.0 - core: the settings app's crop-tool picture ("GazeOverlay.MirrorSnapshot", format v2 of
// 1.x, unchanged so the app works as it is). Made only when the app asks (a counter in its signal block) or when the
// armed capture key is pressed - the key is looked at here, inside the game, and only while armed. Both eyes are taken
// in the SAME frame: a producer that sees both images needs no second frame.
#pragma once

#include "renderer.h"
#include "settings.h"

namespace gaze_mirror {

    constexpr wchar_t SnapshotName[] = L"GazeOverlay.MirrorSnapshot";
    constexpr uint32_t SnapshotMagic = 0x4E534F47; // 'GOSN'
    constexpr uint32_t SnapshotMaxSide = 640;
    constexpr uint32_t SnapshotNoEye = 0xFFFFFFFF;

    struct SnapshotBlock {
        uint32_t magic;
        uint32_t version;
        volatile LONG generation;
        uint32_t width, height;
        uint32_t fullWidth, fullHeight;
        uint32_t cropX, cropY, cropWidth, cropHeight;
        uint32_t eye;
        uint32_t secondEye;
        // Version 3: where straight ahead lands in the first eye's image (fractions, from the headset's own projection),
        // and whether that is known. The app marks the picture there: the true middle of what the headset shows.
        float axisU, axisV;
        uint32_t axisValid;
        uint8_t pixels[SnapshotMaxSide * SnapshotMaxSide * 4];
        uint8_t secondPixels[SnapshotMaxSide * SnapshotMaxSide * 4];
    };

    class SnapshotService {
      public:
        ~SnapshotService();

        // Once per frame, cheap. True when a picture is wanted this frame.
        bool wanted(SettingsSource::Signal* signal);

        // Takes it. images[eye] may be null (then only the other eye is taken).
        void take(Renderer& renderer, const SourceImage* const images[2], const EyeView views[2], int mirroredEye, const CropRect& placed);

      private:
        HANDLE _mapping = nullptr;
        SnapshotBlock* _block = nullptr;
        LONG _seenRequest = 0;
        bool _seenRequestInitialised = false;
        bool _keyReady = false;
        bool _failed = false;
        std::vector<uint8_t> _pixels;
    };

} // namespace gaze_mirror
