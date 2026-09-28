#include "pch.h"

#include "log.h"
#include "snapshot.h"

namespace gaze_mirror {

    SnapshotService::~SnapshotService() {
        if (_block) UnmapViewOfFile(_block);
        if (_mapping) CloseHandle(_mapping);
    }

    bool SnapshotService::wanted(SettingsSource::Signal* signal) {
        if (!signal) return false;
        if (!_seenRequestInitialised) {
            _seenRequestInitialised = true;
            _seenRequest = signal->snapshotRequest;
        }
        bool pressed = false;
        const LONG captureKey = signal->captureKey;
        if (captureKey > 0 && captureKey < 256) {
            const bool down = (GetAsyncKeyState(static_cast<int>(captureKey)) & 0x8000) != 0;
            if (!down) {
                _keyReady = true;
            } else if (_keyReady) {
                pressed = true;
                _keyReady = false;
                signal->captureKey = 0;
            }
        } else {
            _keyReady = false;
        }
        const LONG request = signal->snapshotRequest;
        if (!pressed && request == _seenRequest) return false;
        _seenRequest = request;
        if (pressed) _failed = false; // Asked for in person: worth another try.
        return !_failed;
    }

    void SnapshotService::take(Renderer& renderer, const SourceImage* const images[2], const EyeView views[2], int mirroredEye, const CropRect& placed,
                               bool ringValid, const float ringPixels[2], float ringRadius) {
        _failed = true; // Until everything worked: a failure is not retried on every request for ever.
        if (!_mapping) {
            _mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SnapshotBlock), SnapshotName);
            if (_mapping) _block = static_cast<SnapshotBlock*>(MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SnapshotBlock)));
            if (!_block) {
                Log("snapshot: the block could not be created (%lu)", GetLastError());
                return;
            }
        }
        const SourceImage* first = images[mirroredEye] ? images[mirroredEye] : images[1 - mirroredEye];
        const int firstEye = images[mirroredEye] ? mirroredEye : 1 - mirroredEye;
        if (!first) return;
        uint32_t width = 0, height = 0;
        if (!renderer.snapshot(*first, SnapshotMaxSide, _pixels, width, height)) return;
        memcpy(_block->pixels, _pixels.data(), _pixels.size());
        _block->magic = SnapshotMagic;
        _block->version = 4;
        // The ring, if it was in the picture's eye when the picture was taken.
        if (ringValid && firstEye == mirroredEye && first->width > 0 && first->height > 0) {
            _block->gazeU = ringPixels[0] / float(first->width);
            _block->gazeV = ringPixels[1] / float(first->height);
            _block->gazeRadius = ringRadius;
            _block->gazeValid = 1;
        } else {
            _block->gazeU = _block->gazeV = 0.5f;
            _block->gazeRadius = 0.f;
            _block->gazeValid = 0;
        }
        // Straight ahead in this eye's image: the view axis, from the projection's tangents (the image spans left..right
        // and up..down in tangent space, and the axis is at tangent 0 on both).
        const EyeView& view = views[firstEye];
        const float spanX = view.fov.right - view.fov.left, spanY = view.fov.up - view.fov.down;
        if (view.valid && spanX > 1e-4f && spanY > 1e-4f) {
            _block->axisU = -view.fov.left / spanX;
            _block->axisV = view.fov.up / spanY;
            _block->axisValid = 1;
        } else {
            _block->axisU = _block->axisV = 0.5f;
            _block->axisValid = 0;
        }
        _block->width = width;
        _block->height = height;
        _block->fullWidth = static_cast<uint32_t>(first->width);
        _block->fullHeight = static_cast<uint32_t>(first->height);
        _block->cropX = static_cast<uint32_t>(placed.x);
        _block->cropY = static_cast<uint32_t>(placed.y);
        _block->cropWidth = static_cast<uint32_t>(placed.width);
        _block->cropHeight = static_cast<uint32_t>(placed.height);
        _block->eye = static_cast<uint32_t>(firstEye);
        _block->secondEye = SnapshotNoEye;
        const SourceImage* second = images[1 - firstEye];
        if (second) {
            uint32_t width2 = 0, height2 = 0;
            if (renderer.snapshot(*second, SnapshotMaxSide, _pixels, width2, height2) && width2 == width && height2 == height) {
                memcpy(_block->secondPixels, _pixels.data(), _pixels.size());
                _block->secondEye = static_cast<uint32_t>(1 - firstEye);
            }
        }
        InterlockedIncrement(&_block->generation);
        _failed = false;
        Log("snapshot: %u x %u of %d x %d, eye %d%s", width, height, first->width, first->height, firstEye, second ? " + the other eye" : "");
    }

} // namespace gaze_mirror
