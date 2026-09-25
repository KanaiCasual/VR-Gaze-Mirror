#include "pch.h"

#include "external_gaze.h"

#include <cstring>

namespace gaze_mirror {

    ExternalGazeReader::~ExternalGazeReader() {
        close();
    }

    bool ExternalGazeReader::open() {
        _mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, _name);
        if (!_mapping) return false;
        _block = static_cast<volatile ExternalGaze*>(MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, sizeof(ExternalGaze)));
        if (!_block) {
            CloseHandle(_mapping);
            _mapping = nullptr;
            return false;
        }
        if (_block->magic != ExternalGazeMagic || _block->version != ExternalGazeVersion || _block->structSize != sizeof(ExternalGaze)) {
            // Not stamped yet, or another version: try again later.
            close();
            return false;
        }
        return true;
    }

    void ExternalGazeReader::close() {
        if (_block) UnmapViewOfFile(const_cast<ExternalGaze*>(_block));
        if (_mapping) CloseHandle(_mapping);
        _block = nullptr;
        _mapping = nullptr;
    }

    ExternalGazeSample ExternalGazeReader::read() {
        ExternalGazeSample sample;
        if (!_block) {
            // No block: look for one every so often (a few times a second at game rate), not every frame.
            if (--_retryIn > 0) return sample;
            _retryIn = 30;
            if (!open()) return sample;
        }
        for (int attempt = 0; attempt < 4; attempt++) {
            const LONGLONG before = _block->sequence;
            if (before & 1) continue; // Being written right now.
            MemoryBarrier();
            const LONGLONG writtenMs = _block->writtenMs;
            const LONG pid = _block->writerPid;
            const LONG leftValid = _block->leftValid;
            const LONG rightValid = _block->rightValid;
            sample.left[0] = _block->leftX;
            sample.left[1] = _block->leftY;
            sample.left[2] = _block->leftZ;
            sample.right[0] = _block->rightX;
            sample.right[1] = _block->rightY;
            sample.right[2] = _block->rightZ;
            sample.leftOpenness = _block->leftOpenness;
            sample.rightOpenness = _block->rightOpenness;
            for (int i = 0; i < 31; i++) sample.writer[i] = _block->writerName[i];
            MemoryBarrier();
            if (_block->sequence != before) continue; // Torn: read again.
            const ULONGLONG now = GetTickCount64();
            const bool fresh = pid != 0 && writtenMs > 0 && now >= static_cast<ULONGLONG>(writtenMs) && now - static_cast<ULONGLONG>(writtenMs) <= ExternalGazeFreshMs;
            sample.leftValid = fresh && leftValid != 0;
            sample.rightValid = fresh && rightValid != 0;
            sample.valid = sample.leftValid || sample.rightValid;
            return sample;
        }
        return sample;
    }

} // namespace gaze_mirror
