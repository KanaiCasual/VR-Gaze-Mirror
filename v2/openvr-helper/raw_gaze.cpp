#include "raw_gaze.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <ws2tcpip.h>

#include "../core/log.h"
#include "osc.h"

namespace gaze_mirror {

    RawGazeLink::~RawGazeLink() {
        stop();
    }

    bool RawGazeLink::start(uint16_t port) {
        if (_running) return true;
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Log("raw gaze: Winsock is not available");
            return false;
        }
        _winsock = true;
        _port = port;

        _mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ExternalGaze), RawGazeMappingName);
        if (_mapping) _block = static_cast<volatile ExternalGaze*>(MapViewOfFile(_mapping, FILE_MAP_WRITE, 0, 0, sizeof(ExternalGaze)));
        if (!_block) {
            Log("raw gaze: the shared block could not be created");
            stop();
            return false;
        }
        if (_block->magic != ExternalGazeMagic) {
            _block->version = ExternalGazeVersion;
            _block->structSize = sizeof(ExternalGaze);
            MemoryBarrier();
            _block->magic = ExternalGazeMagic;
        }
        _block->source = ExternalGazeSourceSranibroRaw;
        const char name[] = "SRanibro raw gaze";
        for (size_t i = 0; i < sizeof(name); i++) _block->writerName[i] = name[i];
        _block->writerPid = LONG(GetCurrentProcessId());

        _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(port);
        if (_socket == INVALID_SOCKET || bind(_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            Log("raw gaze: 127.0.0.1:%u could not be opened (%d) - another program has it; raw_gaze_port in the settings picks another", unsigned(port), WSAGetLastError());
            stop();
            return false;
        }
        _running = true;
        _thread = std::thread([this] { run(); });
        Log("raw gaze: listening on 127.0.0.1:%u for SRanibro's raw gaze (its \"Raw gaze OSC\" switch)", unsigned(port));
        return true;
    }

    void RawGazeLink::stop() {
        _running = false;
        if (_socket != INVALID_SOCKET) closesocket(_socket); // Wakes the thread blocked on it.
        _socket = INVALID_SOCKET;
        if (_thread.joinable()) _thread.join();
        if (_block) {
            _block->leftValid = 0;
            _block->rightValid = 0;
            _block->writerPid = 0;
            UnmapViewOfFile(const_cast<ExternalGaze*>(_block));
        }
        if (_mapping) CloseHandle(_mapping);
        _block = nullptr;
        _mapping = nullptr;
        if (_winsock) WSACleanup();
        _winsock = false;
    }

    void RawGazeLink::run() {
        uint8_t packet[4096];
        while (_running) {
            const int n = recv(_socket, reinterpret_cast<char*>(packet), sizeof(packet), 0);
            if (n <= 0) {
                if (!_running) break;
                continue;
            }
            handlePacket(packet, n);
            publish();
        }
    }

    void RawGazeLink::handlePacket(const uint8_t* data, int length) {
        if (length >= 16 && memcmp(data, "#bundle", 8) == 0) {
            int pos = 16; // "#bundle\0" and the 8-byte time tag.
            while (pos + 4 <= length) {
                const int size = osc::ReadInt(data + pos);
                pos += 4;
                if (size < 0 || pos + size > length) break;
                handlePacket(data + pos, size);
                pos += size;
            }
            return;
        }
        handleMessage(data, length);
    }

    void RawGazeLink::handleMessage(const uint8_t* data, int length) {
        std::vector<double> numbers;
        const std::string address = osc::ReadMessage(data, length, numbers);
        if (address.empty()) return;
        if (_addresses.size() < 20) {
            bool known = false;
            for (const std::string& s : _addresses) if (s == address) { known = true; break; }
            if (!known) {
                _addresses.push_back(address);
                Log("raw gaze: receiving %s (%zu numbers)", address.c_str(), numbers.size());
            }
        }
        // The eye is the last part of the address: ".../left" or ".../right" (whatever comes before it).
        const size_t slash = address.find_last_of('/');
        const std::string which = address.substr(slash + 1);
        Eye* eye = which == "left" ? &_left : which == "right" ? &_right : nullptr;
        if (!eye || numbers.size() < 3) return;
        // x y z valid openness timestamp - the last three are optional.
        float dir[3] = {float(numbers[0]), float(numbers[1]), float(numbers[2])};
        const float length2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
        const bool valid = numbers.size() < 4 || numbers[3] >= 0.5;
        if (!std::isfinite(length2) || length2 < 1e-6f) {
            eye->valid = false;
            eye->seen = true;
            return;
        }
        const float scale = 1.f / std::sqrt(length2);
        for (float& v : dir) v *= scale;
        memcpy(eye->dir, dir, sizeof(dir));
        eye->valid = valid;
        eye->seen = true;
        if (numbers.size() >= 5 && std::isfinite(numbers[4])) eye->openness = float(std::fmin(std::fmax(numbers[4], 0.0), 1.0));
        if (valid && _logged < 1) {
            _logged = 1;
            Log("raw gaze: SRanibro's raw gaze arriving (%s: %.3f %.3f %.3f)", which.c_str(), dir[0], dir[1], dir[2]);
        }
    }

    void RawGazeLink::publish() {
        if (!_block) return;
        _block->sequence = ++_sequence; // Odd: writing.
        MemoryBarrier();
        _block->leftValid = _left.valid ? 1 : 0;
        _block->rightValid = _right.valid ? 1 : 0;
        _block->leftX = _left.dir[0];
        _block->leftY = _left.dir[1];
        _block->leftZ = _left.dir[2];
        _block->rightX = _right.dir[0];
        _block->rightY = _right.dir[1];
        _block->rightZ = _right.dir[2];
        _block->leftOpenness = _left.openness;
        _block->rightOpenness = _right.openness;
        _block->writtenMs = LONGLONG(GetTickCount64());
        MemoryBarrier();
        _block->sequence = ++_sequence; // Even: done.
    }

} // namespace gaze_mirror
