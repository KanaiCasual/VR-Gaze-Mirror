// VR Gaze Mirror 2.0 - core: the producer side of the shared format (protocol/gaze_mirror_protocol.h).
// Owns the shared block and the slot textures; tells the readers when a frame is ready; knows whether anybody wants
// one at all (a few memory reads per frame, and never a timer).
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "../protocol/gaze_mirror_protocol.h"

namespace gaze_mirror {

    class Publisher {
      public:
        ~Publisher();

        bool start(ID3D11Device* device, LONG producerKind, const char* program, const char* application);
        void stop();
        // The process that publishes right now, if it is alive and not this one; 0 otherwise. A look at the shared
        // block, nothing more: for a producer that stands by and wants to know whether it is worth trying.
        struct Elsewhere {
            LONG pid = 0;  // 0: nobody else.
            LONG kind = 0;
            LONG gazeSupport = 0;
            LONGLONG frameNumber = 0;
        };
        static Elsewhere LiveProducerElsewhere();
        bool started() const {
            return _frames != nullptr;
        }

        // Is there anybody to make a picture for?
        bool wanted();

        // A slot to draw the next frame into, of this size (textures are made on demand). Null when every slot is in
        // use by a reader right now - then skip the frame rather than wait.
        struct Slot {
            int index = -1;
            ID3D11Texture2D* texture = nullptr;
            ID3D11RenderTargetView* target = nullptr;
        };
        Slot acquire(uint32_t width, uint32_t height);

        // The frame in that slot is finished (on the GPU's queue): tell the readers.
        void publish(const Slot& slot, int eye, bool gazeValid, float gazeU, float gazeV);
        // What the producer knows about the headset's eye tracking (Frames::gazeSupport).
        void setGazeSupport(LONG support) {
            if (_frames) _frames->gazeSupport = support;
        }

        // Another producer took the block over (an OpenXR game beats the OpenVR helper).
        bool lost() const {
            return _frames && _frames->producerPid != static_cast<LONG>(GetCurrentProcessId());
        }
        // The game changed (OpenVR helper): tell the readers.
        void rename(const char* program, const char* application);

        uint32_t width() const {
            return _width;
        }
        uint32_t height() const {
            return _height;
        }

      private:
        struct SlotState {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        };
        void resize(uint32_t width, uint32_t height);

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        SlotState _slots[SlotCount];
        uint32_t _width = 0, _height = 0;
        HANDLE _mapping = nullptr;
        Frames* _frames = nullptr;
        HANDLE _readerEvents[ReaderCount] = {};
        HANDLE _producerWake = nullptr;
        ULONGLONG _lastPing[ReaderCount] = {};
        int _unansweredPings[ReaderCount] = {};
        int _logProblem = 0;
    };

} // namespace gaze_mirror
