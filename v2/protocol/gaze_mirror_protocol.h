// VR Gaze Mirror 2.0 - the one format every producer and every reader speaks.
//
// Producers make the finished mirror picture (cropped, steadied, ring drawn) and publish it here:
//   - the OpenXR layer, inside an OpenXR game;
//   - the OpenVR helper, next to an OpenVR game (reads SteamVR's own mirror).
// Readers only show it: the OBS plugin, the mirror window, the dev viewer. A reader never needs to know which kind
// of game is running.
//
// How a frame travels:
//   1. The producer draws into one of SlotCount shared textures - never the one it published last, never one a
//      reader has marked as "reading".
//   2. It writes latestSlot / frameNumber and sets the event of every live reader.
//   3. A reader wakes, marks the slot, uses the texture on its own device, clears the mark.
// Nothing here runs on a timer. With no live reader the producer does no work at all.
//
// Whoever starts first creates the mapping (create-or-open); it is zero-filled by Windows, and the creator stamps
// magic/version/structSize. All fields are plain integers and floats so that C, C++ and C# can share it.

#pragma once

#include <cstddef>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace gaze_mirror {

    constexpr wchar_t FramesMappingName[] = L"GazeMirror2.Frames";
    constexpr wchar_t ReaderEventFormat[] = L"GazeMirror2.FrameReady.%u"; // One auto-reset event per reader slot.
    // Set by a reader that takes a place and by a producer that stops: a producer with nothing to do sleeps on it
    // instead of looking at the block on a timer (auto-reset).
    constexpr wchar_t ProducerWakeName[] = L"GazeMirror2.ProducerWake";

    constexpr uint32_t FramesMagic = 0x46324D47; // 'GM2F'
    constexpr uint32_t FramesVersion = 1;
    constexpr uint32_t SlotCount = 4;
    constexpr uint32_t ReaderCount = 4;
    constexpr LONG NoSlot = -1;

    constexpr LONG ProducerNone = 0;
    constexpr LONG ProducerOpenXR = 1;
    constexpr LONG ProducerOpenVR = 2;

    // A reader is "live" while its heartbeat is younger than this. It writes the heartbeat whenever its event wakes
    // it; a producer that finds a registered reader with an old heartbeat sets the event once ("are you there?")
    // before it concludes anything.
    constexpr ULONGLONG ReaderLiveMs = 2000;

    struct Reader {
        volatile LONG pid;             // 0 = free. Claim with InterlockedCompareExchange(&pid, myPid, 0).
        volatile LONG readingSlot;     // NoSlot, or the slot this reader is using right now.
        volatile LONGLONG heartbeatMs; // GetTickCount64() of the reader's last wake.
        volatile LONG wantedFps;       // 0 = every frame. Reserved: readers skip frames themselves for now.
        LONG reserved;
    };

    struct Frames {
        uint32_t magic;
        uint32_t version;
        uint32_t structSize;

        volatile LONG producerKind; // ProducerNone while nobody publishes.
        volatile LONG producerPid;
        volatile LONG generation;   // Bumped whenever the textures below were (re)made: readers reopen them.

        uint32_t width;
        uint32_t height;
        uint32_t dxgiFormat;          // DXGI_FORMAT of the shared textures (always a typed, non-sRGB 8-bit format).
        uint32_t adapterLuidLow;      // The graphics card the textures live on; a reader on another one cannot open them.
        int32_t adapterLuidHigh;
        uint32_t reserved0;
        uint64_t textureHandle[SlotCount]; // IDXGIResource::GetSharedHandle values (system-wide, not per process).

        volatile LONG latestSlot;       // NoSlot until the first frame.
        volatile LONG eye;              // 0 left, 1 right - the eye the picture shows.
        volatile LONGLONG frameNumber;  // Counts published frames; a reader compares it with the last one it showed.

        // Where the viewer looks, in the published picture (0..1, origin top left) - already drawn as the ring;
        // published too, so that a reader could show it differently one day.
        volatile LONG gazeValid;
        float gazeU;
        float gazeV;
        uint32_t reserved1;

        // The game the picture comes from: its program file ("DCS.exe") and the name it gave to OpenXR / its SteamVR
        // application key. UTF-8, zero-terminated. Written before producerKind is set; per-game crop profiles and the
        // app's status display go by these.
        char producerProgram[64];
        char producerApplication[128];

        Reader readers[ReaderCount];
    };

    // The settings app reads this block from C# by offset: keep them where they are (or bump FramesVersion).
    static_assert(offsetof(Frames, producerKind) == 12 && offsetof(Frames, width) == 24 && offsetof(Frames, height) == 28, "layout");
    static_assert(offsetof(Frames, eye) == 84 && offsetof(Frames, gazeValid) == 96 && offsetof(Frames, gazeU) == 100, "layout");
    static_assert(offsetof(Frames, producerProgram) == 112 && offsetof(Frames, producerApplication) == 176, "layout");
    static_assert(offsetof(Frames, readers) == 304 && sizeof(Frames) == 400, "layout");

    // ---- Eye gaze from outside the headset's own interfaces.
    //
    // Some headsets' software never feeds SteamVR's eye-tracking input or OpenXR's eye-gaze extension, but does drive
    // a VRChat avatar's eye parameters (through VRCFaceTracking, EyeTrackVR, ALVR, ...). VRChat sends those out over
    // OSC; the OpenVR helper receives them (openvr-helper/vrchat_osc.h) and writes them here, and a producer that has
    // no gaze of its own (or is told to prefer this) reads it. Single writer; a sequence lock (odd while writing)
    // lets a reader notice a torn read and read again. Age is judged by writtenMs against GetTickCount64().
    //
    // The gaze values are the avatar's: per eye, x right and y up, -1..1, whose reach depends on the software that
    // drives the avatar (hence the vrchat_scale setting).

    constexpr wchar_t ExternalGazeMappingName[] = L"GazeMirror2.ExternalGaze";
    constexpr uint32_t ExternalGazeMagic = 0x58324D47; // 'GM2X'
    constexpr uint32_t ExternalGazeVersion = 1;
    constexpr LONG ExternalGazeSourceVrchatOsc = 2;
    // A second block of the same layout, "GazeMirror2.RawGaze": the eye tracker's raw direction per eye (unit vector,
    // x right, y up, forward -z - the z fields below), as SRanibro sends it over OSC. Exact, full range, no calibration.
    constexpr wchar_t RawGazeMappingName[] = L"GazeMirror2.RawGaze";
    constexpr LONG ExternalGazeSourceSranibroRaw = 3;
    constexpr ULONGLONG ExternalGazeFreshMs = 250; // Older than this = the writer stopped; the gaze counts as lost.

    struct ExternalGaze {
        uint32_t magic;
        uint32_t version;
        uint32_t structSize;
        uint32_t reserved0;
        volatile LONGLONG sequence;  // Odd while the writer is inside a write.
        volatile LONGLONG writtenMs; // GetTickCount64() (Environment.TickCount64) at the last write.
        volatile LONG writerPid;     // 0 once the writer has left.
        LONG source;                 // ExternalGazeSourceVrchatOsc.
        LONG leftValid;
        LONG rightValid;
        float leftX, leftY;          // Left eye gaze.
        float rightX, rightY;        // Right eye gaze.
        float leftOpenness, rightOpenness; // 0 closed .. 1 open.
        float leftPupilMm, rightPupilMm;
        char writerName[32];         // UTF-8, zero-terminated: "VRChat OSC".
        float leftZ, rightZ;         // The raw block only: the third component of each eye's direction.
        uint8_t reserved[400 - 120];
    };

    // The module (C#) and the app write and read this block by offset: keep them where they are (or bump the version).
    static_assert(offsetof(ExternalGaze, sequence) == 16 && offsetof(ExternalGaze, writtenMs) == 24 && offsetof(ExternalGaze, writerPid) == 32, "layout");
    static_assert(offsetof(ExternalGaze, leftValid) == 40 && offsetof(ExternalGaze, leftX) == 48 && offsetof(ExternalGaze, rightX) == 56, "layout");
    static_assert(offsetof(ExternalGaze, leftOpenness) == 64 && offsetof(ExternalGaze, leftPupilMm) == 72 && offsetof(ExternalGaze, writerName) == 80, "layout");
    static_assert(offsetof(ExternalGaze, leftZ) == 112 && offsetof(ExternalGaze, rightZ) == 116, "layout");
    static_assert(sizeof(ExternalGaze) == 400, "layout");

} // namespace gaze_mirror
