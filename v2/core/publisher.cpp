#include "pch.h"

#include "log.h"
#include "publisher.h"

using Microsoft::WRL::ComPtr;

namespace gaze_mirror {

    namespace {
        constexpr DXGI_FORMAT OutputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    Publisher::~Publisher() {
        stop();
    }

    Publisher::Elsewhere Publisher::LiveProducerElsewhere() {
        Elsewhere other;
        const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, FramesMappingName);
        if (!mapping) return other;
        if (const Frames* frames = static_cast<const Frames*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Frames)))) {
            if (frames->magic == FramesMagic && frames->producerKind != ProducerNone && frames->producerPid != static_cast<LONG>(GetCurrentProcessId())) {
                const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(frames->producerPid));
                if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                    other.pid = frames->producerPid;
                    other.kind = frames->producerKind;
                    other.gazeSupport = frames->gazeSupport;
                    other.frameNumber = frames->frameNumber;
                }
                if (process) CloseHandle(process);
            }
            UnmapViewOfFile(frames);
        }
        CloseHandle(mapping);
        return other;
    }

    bool Publisher::start(ID3D11Device* device, LONG producerKind, const char* program, const char* application, bool yieldToOpenVR, bool takeOver) {
        stop();
        _device = device;
        _mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Frames), FramesMappingName);
        if (_mapping) _frames = static_cast<Frames*>(MapViewOfFile(_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Frames)));
        if (!_frames) {
            Log("publisher: the shared block could not be created (%lu)", GetLastError());
            stop();
            return false;
        }
        if (_frames->magic == FramesMagic && _frames->version != FramesVersion) {
            Log("publisher: another component uses format version %u, this one %u - no mirror", _frames->version, FramesVersion);
            stop();
            return false;
        }
        if (_frames->producerKind != ProducerNone && _frames->producerPid != static_cast<LONG>(GetCurrentProcessId())) {
            // Somebody else publishes. An OpenXR game's layer beats the OpenVR helper - unless the picture is to come through
            // SteamVR's compositor (with its overlays), when it yields; a take-over beats anybody; otherwise first come.
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_frames->producerPid));
            const bool alive = process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            if (process) CloseHandle(process);
            const bool layerOverHelper = producerKind == ProducerOpenXR && _frames->producerKind == ProducerOpenVR && !yieldToOpenVR;
            if (alive && takeOver) Log("publisher: taking the picture over from producer %ld (kind %ld)", _frames->producerPid, _frames->producerKind);
            if (alive && !takeOver && !layerOverHelper) {
                Log("publisher: producer %ld (kind %ld) is already publishing - standing by", _frames->producerPid, _frames->producerKind);
                stop();
                return false;
            }
        }
        for (uint32_t i = 0; i < ReaderCount; i++) {
            wchar_t name[64];
            swprintf_s(name, ReaderEventFormat, i);
            _readerEvents[i] = CreateEventW(nullptr, FALSE, FALSE, name);
        }
        _producerWake = CreateEventW(nullptr, FALSE, FALSE, ProducerWakeName);
        _producerChanged = CreateEventW(nullptr, FALSE, FALSE, ProducerChangedName);
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) adapter->GetDesc(&adapterDesc);

        _frames->magic = FramesMagic;
        _frames->version = FramesVersion;
        _frames->structSize = sizeof(Frames);
        _frames->latestSlot = NoSlot;
        _frames->width = _frames->height = 0;
        _frames->gazeValid = 0;
        _frames->gazeSupport = 0;
        _frames->dxgiFormat = OutputFormat;
        _frames->adapterLuidLow = adapterDesc.AdapterLuid.LowPart;
        _frames->adapterLuidHigh = adapterDesc.AdapterLuid.HighPart;
        for (auto& handle : _frames->textureHandle) handle = 0;
        strncpy_s(_frames->producerProgram, program ? program : "", _TRUNCATE);
        strncpy_s(_frames->producerApplication, application ? application : "", _TRUNCATE);
        _frames->producerPid = static_cast<LONG>(GetCurrentProcessId());
        InterlockedIncrement(&_frames->generation);
        _frames->producerKind = producerKind;
        // Anyone waiting learns that there is a producer now.
        for (uint32_t i = 0; i < ReaderCount; i++) {
            if (_frames->readers[i].pid != 0 && _readerEvents[i]) SetEvent(_readerEvents[i]);
        }
        if (_producerChanged) SetEvent(_producerChanged);
        Log("publisher: ready on \"%ls\" as %s", adapterDesc.Description, producerKind == ProducerOpenXR ? "OpenXR" : "OpenVR");
        return true;
    }

    void Publisher::stop() {
        if (_frames) {
            if (_frames->producerPid == static_cast<LONG>(GetCurrentProcessId())) {
                _frames->producerKind = ProducerNone;
                _frames->producerPid = 0;
                _frames->latestSlot = NoSlot;
                for (auto& handle : _frames->textureHandle) handle = 0;
                InterlockedIncrement(&_frames->generation);
                for (uint32_t i = 0; i < ReaderCount; i++) {
                    if (_frames->readers[i].pid != 0 && _readerEvents[i]) SetEvent(_readerEvents[i]);
                }
                if (_producerWake) SetEvent(_producerWake); // A stand-by producer may take over.
                if (_producerChanged) SetEvent(_producerChanged);
            }
            UnmapViewOfFile(_frames);
            _frames = nullptr;
        }
        if (_mapping) CloseHandle(_mapping);
        _mapping = nullptr;
        if (_producerWake) CloseHandle(_producerWake);
        _producerWake = nullptr;
        if (_producerChanged) CloseHandle(_producerChanged);
        _producerChanged = nullptr;
        for (auto& event : _readerEvents) {
            if (event) CloseHandle(event);
            event = nullptr;
        }
        for (auto& slot : _slots) slot = {};
        _width = _height = 0;
        _device.Reset();
    }

    void Publisher::rename(const char* program, const char* application) {
        if (!_frames || lost()) return;
        strncpy_s(_frames->producerProgram, program ? program : "", _TRUNCATE);
        strncpy_s(_frames->producerApplication, application ? application : "", _TRUNCATE);
        InterlockedIncrement(&_frames->generation);
        for (uint32_t i = 0; i < ReaderCount; i++) {
            if (_frames->readers[i].pid != 0 && _readerEvents[i]) SetEvent(_readerEvents[i]);
        }
        if (_producerChanged) SetEvent(_producerChanged);
    }

    bool Publisher::wanted() {
        if (!_frames) return false;
        const ULONGLONG now = GetTickCount64();
        bool any = false;
        for (uint32_t i = 0; i < ReaderCount; i++) {
            Reader& reader = _frames->readers[i];
            const LONG pid = reader.pid;
            if (pid == 0) continue;
            if (now - static_cast<ULONGLONG>(reader.heartbeatMs) < ReaderLiveMs) {
                _unansweredPings[i] = 0;
                any = true;
                continue;
            }
            // Registered but quiet - it may simply be waiting for us. Ask, at most once a second.
            if (now - _lastPing[i] < 1000) continue;
            _lastPing[i] = now;
            if (_unansweredPings[i] >= 3) {
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
                const bool gone = !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
                if (process) CloseHandle(process);
                if (gone) {
                    Log("publisher: reader %u (process %ld) is gone - place freed", i, pid);
                    reader.readingSlot = NoSlot;
                    InterlockedCompareExchange(&reader.pid, 0, pid);
                    _unansweredPings[i] = 0;
                    continue;
                }
            }
            _unansweredPings[i]++;
            if (_readerEvents[i]) SetEvent(_readerEvents[i]);
        }
        return any;
    }

    void Publisher::resize(uint32_t width, uint32_t height) {
        for (auto& slot : _slots) slot = {};
        for (auto& handle : _frames->textureHandle) handle = 0;
        _frames->latestSlot = NoSlot;
        _width = width;
        _height = height;
        _frames->width = width;
        _frames->height = height;
        InterlockedIncrement(&_frames->generation);
        Log("publisher: picture is now %u x %u", width, height);
    }

    Publisher::Slot Publisher::acquire(uint32_t width, uint32_t height) {
        Slot none;
        if (!_frames || width < 2 || height < 2) return none;
        if (width != _width || height != _height) resize(width, height);

        const LONG latest = _frames->latestSlot;
        int chosen = -1, unused = -1;
        for (int i = 0; i < static_cast<int>(SlotCount); i++) {
            if (i == latest) continue;
            bool busy = false;
            for (const Reader& reader : _frames->readers) {
                if (reader.pid != 0 && reader.readingSlot == i) busy = true;
            }
            if (busy) continue;
            if (_slots[i].texture) {
                chosen = i; // Prefer one that exists: a slot nobody ever needs is never made.
                break;
            }
            if (unused < 0) unused = i;
        }
        if (chosen < 0) chosen = unused;
        if (chosen < 0) return none;

        SlotState& state = _slots[chosen];
        if (!state.texture) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = OutputFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource> resource;
            HANDLE handle = nullptr;
            if (FAILED(_device->CreateTexture2D(&desc, nullptr, &state.texture)) ||
                FAILED(_device->CreateRenderTargetView(state.texture.Get(), nullptr, &state.target)) || FAILED(state.texture.As(&resource)) ||
                FAILED(resource->GetSharedHandle(&handle))) {
                LogFewTimes(_logProblem, 3, "publisher: shared texture %d (%u x %u) could not be made", chosen, width, height);
                state = {};
                return none;
            }
            _frames->textureHandle[chosen] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
            InterlockedIncrement(&_frames->generation);
            Log("publisher: shared texture %d made (%u x %u)", chosen, width, height);
        }
        Slot slot;
        slot.index = chosen;
        slot.texture = state.texture.Get();
        slot.target = state.target.Get();
        return slot;
    }

    void Publisher::publish(const Slot& slot, int eye, bool gazeValid, float gazeU, float gazeV) {
        if (!_frames || slot.index < 0) return;
        _frames->eye = eye;
        _frames->gazeU = gazeU;
        _frames->gazeV = gazeV;
        _frames->gazeValid = gazeValid ? 1 : 0;
        _frames->latestSlot = slot.index;
        InterlockedIncrement64(&_frames->frameNumber);
        const ULONGLONG now = GetTickCount64();
        for (uint32_t i = 0; i < ReaderCount; i++) {
            const Reader& reader = _frames->readers[i];
            if (reader.pid != 0 && _readerEvents[i] && now - static_cast<ULONGLONG>(reader.heartbeatMs) < ReaderLiveMs) SetEvent(_readerEvents[i]);
        }
    }

} // namespace gaze_mirror
