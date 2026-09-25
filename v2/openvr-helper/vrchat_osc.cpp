#include "vrchat_osc.h"

#include "osc.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../core/log.h"

#pragma comment(lib, "ws2_32.lib")

namespace gaze_mirror {

    namespace {

        constexpr const char* ServiceJson = "_oscjson._tcp.local";
        constexpr const char* ServiceOsc = "_osc._udp.local";
        constexpr const char* HostName = "gazemirror.local"; // The A record: 127.0.0.1 - VRChat and the helper share the machine.
        constexpr uint16_t MdnsPort = 5353;
        constexpr uint32_t MdnsGroup = 0xE00000FB; // 224.0.0.251

        // ---- DNS wire format, the little we need (RFC 1035 names with compression pointers, RFC 6762 answers).

        // Reads a name at `pos` into dotted form; follows compression pointers. False on a malformed packet.
        bool ReadName(const uint8_t* p, int length, int& pos, std::string& out) {
            int cursor = pos;
            int jumped = 0;
            out.clear();
            for (int guard = 0; guard < 64; guard++) {
                if (cursor >= length) return false;
                const int label = p[cursor];
                if (label == 0) {
                    cursor++;
                    if (!jumped) pos = cursor;
                    return true;
                }
                if ((label & 0xC0) == 0xC0) {
                    if (cursor + 1 >= length) return false;
                    const int target = ((label & 0x3F) << 8) | p[cursor + 1];
                    if (!jumped) pos = cursor + 2;
                    jumped++;
                    cursor = target;
                    continue;
                }
                if (cursor + 1 + label > length) return false;
                if (!out.empty()) out += '.';
                out.append(reinterpret_cast<const char*>(p + cursor + 1), label);
                cursor += 1 + label;
            }
            return false;
        }

        void PutU16(std::vector<uint8_t>& b, uint16_t v) {
            b.push_back(uint8_t(v >> 8));
            b.push_back(uint8_t(v));
        }
        void PutU32(std::vector<uint8_t>& b, uint32_t v) {
            PutU16(b, uint16_t(v >> 16));
            PutU16(b, uint16_t(v));
        }
        void PutName(std::vector<uint8_t>& b, const std::string& dotted) {
            size_t start = 0;
            while (start <= dotted.size()) {
                size_t end = dotted.find('.', start);
                if (end == std::string::npos) end = dotted.size();
                const size_t n = std::min<size_t>(end - start, 63);
                if (n > 0) {
                    b.push_back(uint8_t(n));
                    b.insert(b.end(), dotted.begin() + start, dotted.begin() + start + n);
                }
                start = end + 1;
            }
            b.push_back(0);
        }
        // One resource record: name, type, class (cache-flush bit as mDNS answers carry), TTL, then the data.
        void PutRecord(std::vector<uint8_t>& b, const std::string& name, uint16_t type, uint32_t ttl, const std::vector<uint8_t>& data, bool flush) {
            PutName(b, name);
            PutU16(b, type);
            PutU16(b, flush ? 0x8001 : 0x0001);
            PutU32(b, ttl);
            PutU16(b, uint16_t(data.size()));
            b.insert(b.end(), data.begin(), data.end());
        }

        bool IsAscii(const std::string& s) {
            for (const char c : s) if (c < 0x20 || c > 0x7E) return false;
            return true;
        }

        // OSC's wire format: osc.h (shared with the raw-gaze receiver).
        const auto ReadOscString = osc::ReadString;
        const auto ReadBigFloat = osc::ReadFloat;
        const auto ReadBigInt = osc::ReadInt;

    } // namespace

    VrchatOscLink::~VrchatOscLink() {
        stop();
    }

    bool VrchatOscLink::start() {
        if (_running) return true;
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Log("vrchat osc: Winsock is not available");
            return false;
        }
        _winsock = true;

        // The block, created (or joined) so the mirror can read it. Stamped once; the writer's name says who fills it.
        _mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ExternalGaze), ExternalGazeMappingName);
        if (_mapping) _block = static_cast<volatile ExternalGaze*>(MapViewOfFile(_mapping, FILE_MAP_WRITE, 0, 0, sizeof(ExternalGaze)));
        if (!_block) {
            Log("vrchat osc: the shared block could not be created");
            stop();
            return false;
        }
        if (_block->magic != ExternalGazeMagic) {
            _block->version = ExternalGazeVersion;
            _block->structSize = sizeof(ExternalGaze);
            _block->magic = ExternalGazeMagic;
        }
        _sequence = _block->sequence & ~1LL;
        _block->source = ExternalGazeSourceVrchatOsc;
        _block->writerPid = LONG(GetCurrentProcessId());
        {
            const char name[] = "VRChat OSC";
            for (int i = 0; i < 32; i++) _block->writerName[i] = i < int(sizeof(name)) ? name[i] : 0;
        }

        // OSC: any free UDP port on the loopback address.
        _osc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        int nameLength = sizeof(local);
        if (_osc == INVALID_SOCKET || bind(_osc, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
            getsockname(_osc, reinterpret_cast<sockaddr*>(&local), &nameLength) != 0) {
            Log("vrchat osc: no UDP port for OSC (%d)", WSAGetLastError());
            stop();
            return false;
        }
        _oscPort = ntohs(local.sin_port);

        // HTTP: any free TCP port on the loopback address.
        _http = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        local.sin_port = 0;
        nameLength = sizeof(local);
        if (_http == INVALID_SOCKET || bind(_http, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
            getsockname(_http, reinterpret_cast<sockaddr*>(&local), &nameLength) != 0 || listen(_http, 8) != 0) {
            Log("vrchat osc: no TCP port for OSCQuery (%d)", WSAGetLastError());
            stop();
            return false;
        }
        _httpPort = ntohs(local.sin_port);

        // mDNS: the multicast group on port 5353, shared with everything else that speaks mDNS on this PC.
        _mdns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL reuse = TRUE;
        setsockopt(_mdns, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(MdnsPort);
        if (_mdns == INVALID_SOCKET || bind(_mdns, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
            Log("vrchat osc: port 5353 could not be shared (%d)", WSAGetLastError());
            stop();
            return false;
        }
        // Join the group on every IPv4 interface, so that VRChat's queries arrive whichever one they come in on.
        {
            ip_mreq loopback{};
            loopback.imr_multiaddr.s_addr = htonl(MdnsGroup);
            loopback.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(_mdns, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&loopback), sizeof(loopback));
            char host[256] = {};
            if (gethostname(host, sizeof(host) - 1) == 0) {
                addrinfo hints{};
                hints.ai_family = AF_INET;
                addrinfo* list = nullptr;
                if (getaddrinfo(host, nullptr, &hints, &list) == 0) {
                    for (addrinfo* a = list; a; a = a->ai_next) {
                        ip_mreq req{};
                        req.imr_multiaddr.s_addr = htonl(MdnsGroup);
                        req.imr_interface = reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr;
                        setsockopt(_mdns, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&req), sizeof(req));
                    }
                    freeaddrinfo(list);
                }
            }
            BOOL loop = TRUE;
            setsockopt(_mdns, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        }

        char instance[64];
        snprintf(instance, sizeof(instance), "GazeMirror-%lu", GetCurrentProcessId());
        _instance = instance;
        _running = true;
        _mdnsThread = std::thread([this] { runMdns(); });
        _httpThread = std::thread([this] { runHttp(); });
        _oscThread = std::thread([this] { runOsc(); });
        Log("vrchat osc: listening as %s (OSCQuery http://127.0.0.1:%u, OSC udp 127.0.0.1:%u)", _instance.c_str(), _httpPort, _oscPort);
        announce();
        return true;
    }

    void VrchatOscLink::stop() {
        _running = false;
        for (SOCKET* s : {&_mdns, &_http, &_osc}) {
            if (*s != INVALID_SOCKET) closesocket(*s); // Wakes the thread blocked on it.
            *s = INVALID_SOCKET;
        }
        for (std::thread* t : {&_mdnsThread, &_httpThread, &_oscThread}) {
            if (t->joinable()) t->join();
        }
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

    // ---- mDNS

    void VrchatOscLink::runMdns() {
        uint8_t packet[2048];
        while (_running) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int n = recvfrom(_mdns, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (n <= 0) {
                if (!_running) break;
                continue;
            }
            answerQuery(packet, n, from);
        }
    }

    // A query for one of our two service types gets the standard answer: PTR to our instance, with SRV, TXT and A
    // alongside. Sent to the group, as mDNS answers are (VRChat listens there); an unsolicited one at start too.
    void VrchatOscLink::answerQuery(const uint8_t* p, int length, const sockaddr_in&) {
        if (length < 12) return;
        const uint16_t flags = uint16_t((p[2] << 8) | p[3]);
        if (flags & 0x8000) return; // A response, not a query.
        const int questions = (p[4] << 8) | p[5];
        int pos = 12;
        bool wantJson = false, wantOsc = false;
        for (int i = 0; i < questions; i++) {
            std::string name;
            if (!ReadName(p, length, pos, name) || pos + 4 > length) return;
            const uint16_t type = uint16_t((p[pos] << 8) | p[pos + 1]);
            pos += 4;
            if (type != 12 && type != 255) continue; // PTR or ANY.
            if (_stricmp(name.c_str(), ServiceJson) == 0) wantJson = true;
            if (_stricmp(name.c_str(), ServiceOsc) == 0) wantOsc = true;
        }
        if (!wantJson && !wantOsc) return;

        std::vector<uint8_t> b;
        PutU16(b, 0);      // ID
        PutU16(b, 0x8400); // Response, authoritative.
        PutU16(b, 0);      // Questions
        const int answers = (wantJson ? 1 : 0) + (wantOsc ? 1 : 0);
        PutU16(b, uint16_t(answers));
        PutU16(b, 0);                                     // Authorities
        PutU16(b, uint16_t(answers * 2 + 1));             // Additionals: SRV + TXT per service, one A record.
        const auto putService = [&](const char* service, uint16_t port, bool answerSection) {
            const std::string full = _instance + "." + service;
            if (answerSection) {
                std::vector<uint8_t> ptr;
                PutName(ptr, full);
                PutRecord(b, service, 12, 4500, ptr, false);
            } else {
                std::vector<uint8_t> srv;
                PutU16(srv, 0);    // Priority
                PutU16(srv, 0);    // Weight
                PutU16(srv, port);
                PutName(srv, HostName);
                PutRecord(b, full, 33, 120, srv, true);
                std::vector<uint8_t> txt;
                const char text[] = "txtvers=1";
                txt.push_back(uint8_t(sizeof(text) - 1));
                txt.insert(txt.end(), text, text + sizeof(text) - 1);
                PutRecord(b, full, 16, 4500, txt, true);
            }
        };
        if (wantJson) putService(ServiceJson, _httpPort, true);
        if (wantOsc) putService(ServiceOsc, _oscPort, true);
        if (wantJson) putService(ServiceJson, _httpPort, false);
        if (wantOsc) putService(ServiceOsc, _oscPort, false);
        const std::vector<uint8_t> a = {127, 0, 0, 1};
        PutRecord(b, HostName, 1, 120, a, true);

        sockaddr_in group{};
        group.sin_family = AF_INET;
        group.sin_addr.s_addr = htonl(MdnsGroup);
        group.sin_port = htons(MdnsPort);
        sendto(_mdns, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<sockaddr*>(&group), sizeof(group));
        if (_logged < 1) {
            _logged = 1;
            Log("vrchat osc: answered a service query (%s%s)", wantJson ? "oscjson" : "", wantOsc ? " osc" : "");
        }
    }

    // An unsolicited answer at start: a VRChat that is already running learns about us without waiting to ask again.
    void VrchatOscLink::announce() {
        // Built as if VRChat had asked for both services.
        std::vector<uint8_t> q;
        PutU16(q, 0);
        PutU16(q, 0);
        PutU16(q, 2);
        PutU16(q, 0);
        PutU16(q, 0);
        PutU16(q, 0);
        PutName(q, ServiceJson);
        PutU16(q, 12);
        PutU16(q, 1);
        PutName(q, ServiceOsc);
        PutU16(q, 12);
        PutU16(q, 1);
        sockaddr_in none{};
        answerQuery(q.data(), int(q.size()), none);
    }

    // ---- OSCQuery over HTTP: two questions, both answered from memory.

    void VrchatOscLink::runHttp() {
        while (_running) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const SOCKET client = accept(_http, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (client == INVALID_SOCKET) {
                if (!_running) break;
                continue;
            }
            char request[2048] = {};
            const int n = recv(client, request, sizeof(request) - 1, 0);
            if (n > 0) {
                const bool hostInfo = strstr(request, "HOST_INFO") != nullptr;
                char body[4096];
                if (hostInfo) {
                    snprintf(body, sizeof(body),
                             "{\"NAME\":\"%s\",\"EXTENSIONS\":{\"ACCESS\":true,\"CLIPMODE\":false,\"RANGE\":true,\"TYPE\":true,\"VALUE\":true},"
                             "\"OSC_IP\":\"127.0.0.1\",\"OSC_PORT\":%u,\"OSC_TRANSPORT\":\"UDP\"}",
                             _instance.c_str(), _oscPort);
                    if (_hostInfoAsked++ == 0) Log("vrchat osc: VRChat found us");
                } else {
                    // The tree: an /avatar node is what makes VRChat send /avatar/change and avatar parameters; the eye
                    // parameters are listed one by one as well, in case VRChat only sends what it finds named here.
                    std::string tree =
                        "{\"FULL_PATH\":\"/\",\"ACCESS\":0,\"CONTENTS\":{\"avatar\":{\"FULL_PATH\":\"/avatar\",\"ACCESS\":0,\"CONTENTS\":{"
                        "\"change\":{\"FULL_PATH\":\"/avatar/change\",\"ACCESS\":2,\"TYPE\":\"s\",\"DESCRIPTION\":\"Avatar ID\"},"
                        "\"parameters\":{\"FULL_PATH\":\"/avatar/parameters\",\"ACCESS\":0,\"CONTENTS\":{";
                    const auto leaf = [](const char* parent, const char* name) {
                        char item[200];
                        snprintf(item, sizeof(item), "\"%s\":{\"FULL_PATH\":\"/avatar/parameters/%s%s\",\"ACCESS\":2,\"TYPE\":\"f\"}", name, parent, name);
                        return std::string(item);
                    };
                    const char* names[] = {"EyeLeftX", "EyeRightX", "EyeLeftY", "EyeRightY", "EyeX", "EyeY", "EyeLidLeft", "EyeLidRight", "EyeLid"};
                    const char* oldNames[] = {"LeftEyeX", "RightEyeX", "LeftEyeY", "RightEyeY", "EyesX", "EyesY", "LeftEyeLid", "RightEyeLid", "CombinedEyeLid"};
                    tree += "\"v2\":{\"FULL_PATH\":\"/avatar/parameters/v2\",\"ACCESS\":0,\"CONTENTS\":{";
                    for (size_t i = 0; i < std::size(names); i++) tree += (i ? "," : "") + leaf("v2/", names[i]);
                    tree += "}}";
                    for (const char* name : oldNames) tree += "," + leaf("", name);
                    tree += "}}}}}}";
                    snprintf(body, sizeof(body), "%s", tree.c_str());
                }
                char header[256];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %u\r\nPragma: no-cache\r\nConnection: close\r\n\r\n",
                         unsigned(strlen(body)));
                send(client, header, int(strlen(header)), 0);
                send(client, body, int(strlen(body)), 0);
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }

    // ---- OSC

    void VrchatOscLink::runOsc() {
        uint8_t packet[4096];
        while (_running) {
            const int n = recv(_osc, reinterpret_cast<char*>(packet), sizeof(packet), 0);
            if (n <= 0) {
                if (!_running) break;
                continue;
            }
            handleOscPacket(packet, n);
            publish();
        }
    }

    void VrchatOscLink::handleOscPacket(const uint8_t* data, int length) {
        if (length >= 16 && memcmp(data, "#bundle", 8) == 0) {
            int pos = 16; // "#bundle\0" and the 8-byte time tag.
            while (pos + 4 <= length) {
                const int size = ReadBigInt(data + pos);
                pos += 4;
                if (size < 0 || pos + size > length) break;
                handleOscPacket(data + pos, size);
                pos += size;
            }
            return;
        }
        handleOscMessage(data, length);
    }

    void VrchatOscLink::handleOscMessage(const uint8_t* data, int length) {
        int pos = 0;
        std::string address, tags;
        if (!ReadOscString(data, length, pos, address) || address.empty() || address[0] != '/') return;
        if (!ReadOscString(data, length, pos, tags) || tags.empty() || tags[0] != ',') return;
        // The first argument only; the parameters we care about carry one float.
        float value = 0.f;
        bool haveValue = false;
        if (tags.size() >= 2) {
            switch (tags[1]) {
            case 'f': if (pos + 4 <= length) { value = ReadBigFloat(data + pos); haveValue = true; } break;
            case 'i': if (pos + 4 <= length) { value = float(ReadBigInt(data + pos)); haveValue = true; } break;
            case 'T': value = 1.f; haveValue = true; break;
            case 'F': value = 0.f; haveValue = true; break;
            default: break;
            }
        }
        // What VRChat sends at all: the first addresses of each kind, once, so a setup that sends no eye parameters
        // can be told apart from one that sends them under other names.
        if (_seen < 80) {
            bool known = false;
            for (const std::string& s : _addresses) if (s == address) { known = true; break; }
            if (!known) {
                _addresses.push_back(address);
                _seen++;
                Log("vrchat osc: receiving %s (%s%s)", address.c_str(), tags.c_str(), haveValue ? "" : ", no value read");
            }
        }
        if (address == "/avatar/change") {
            _eyes = {};
            Log("vrchat osc: avatar changed - waiting for its eye parameters");
            return;
        }
        constexpr char Prefix[] = "/avatar/parameters/";
        if (!haveValue || address.compare(0, sizeof(Prefix) - 1, Prefix) != 0) return;
        // Avatars put the face-tracking parameters under any prefix they like ("v2/", "FT/v2/", ...): the last part counts.
        const char* name = address.c_str() + sizeof(Prefix) - 1;
        if (const char* slash = strrchr(name, '/')) name = slash + 1;

        // Eye lids come as 0..0.75 open, 0.75..1 widened; the ring only wants "open or not".
        const auto lid = [](float v) { return std::clamp(v / 0.75f, 0.f, 1.f); };
        bool eye = true;
        if (strcmp(name, "EyeLeftX") == 0 || strcmp(name, "LeftEyeX") == 0) { _eyes.leftX = value; _eyes.haveX = true; }
        else if (strcmp(name, "EyeRightX") == 0 || strcmp(name, "RightEyeX") == 0) { _eyes.rightX = value; _eyes.haveX = true; }
        else if (strcmp(name, "EyeLeftY") == 0 || strcmp(name, "LeftEyeY") == 0) { _eyes.leftY = value; _eyes.haveY = true; }
        else if (strcmp(name, "EyeRightY") == 0 || strcmp(name, "RightEyeY") == 0) { _eyes.rightY = value; _eyes.haveY = true; }
        else if (strcmp(name, "EyeX") == 0 || strcmp(name, "EyesX") == 0) { _eyes.leftX = _eyes.rightX = value; _eyes.haveX = true; }
        else if (strcmp(name, "EyeY") == 0 || strcmp(name, "EyesY") == 0) { _eyes.leftY = _eyes.rightY = value; _eyes.haveY = true; }
        else if (strcmp(name, "EyeLidLeft") == 0 || strcmp(name, "LeftEyeLid") == 0) _eyes.leftOpen = lid(value);
        else if (strcmp(name, "EyeLidRight") == 0 || strcmp(name, "RightEyeLid") == 0) _eyes.rightOpen = lid(value);
        else if (strcmp(name, "EyeLid") == 0 || strcmp(name, "CombinedEyeLid") == 0) _eyes.leftOpen = _eyes.rightOpen = lid(value);
        else eye = false;
        if (eye && _logged < 2) {
            _logged = 2;
            Log("vrchat osc: eye parameters arriving (%s)", address.c_str());
        }
    }

    VrchatOscLink::EyeSample VrchatOscLink::sample() const {
        std::lock_guard<std::mutex> lock(_sampleMutex);
        return _latest;
    }

    // The block: what is known, stamped now; valid once both axes have been seen since the avatar loaded.
    void VrchatOscLink::publish() {
        const bool valid = _eyes.haveX && _eyes.haveY;
        {
            std::lock_guard<std::mutex> lock(_sampleMutex);
            _latest = {0.5f * (_eyes.leftX + _eyes.rightX), 0.5f * (_eyes.leftY + _eyes.rightY), valid};
        }
        if (!_block) return;
        _block->sequence = ++_sequence; // Odd: writing.
        MemoryBarrier();
        _block->leftValid = valid ? 1 : 0;
        _block->rightValid = valid ? 1 : 0;
        _block->leftX = _eyes.leftX;
        _block->leftY = _eyes.leftY;
        _block->rightX = _eyes.rightX;
        _block->rightY = _eyes.rightY;
        _block->leftOpenness = _eyes.leftOpen;
        _block->rightOpenness = _eyes.rightOpen;
        _block->writtenMs = LONGLONG(GetTickCount64());
        MemoryBarrier();
        _block->sequence = ++_sequence; // Even: done.
    }

} // namespace gaze_mirror
