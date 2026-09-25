// VR Gaze Mirror 2.0 - the OpenVR helper: the little of OSC's wire format the two receivers need (VRChat's avatar
// parameters, SRanibro's raw gaze). Big-endian numbers, 4-byte padded strings, "#bundle" containers.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gaze_mirror::osc {

    // A 4-byte padded string at `pos`; false when it does not end inside the packet.
    inline bool ReadString(const uint8_t* p, int length, int& pos, std::string& out) {
        const int start = pos;
        while (pos < length && p[pos] != 0) pos++;
        if (pos >= length) return false;
        out.assign(reinterpret_cast<const char*>(p + start), pos - start);
        pos = (pos + 4) & ~3; // The terminator plus padding to a multiple of four.
        return pos <= length;
    }
    inline float ReadFloat(const uint8_t* p) {
        const uint32_t u = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    inline int32_t ReadInt(const uint8_t* p) {
        return int32_t((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
    }
    inline int64_t ReadInt64(const uint8_t* p) {
        return (int64_t(uint32_t(ReadInt(p))) << 32) | uint32_t(ReadInt(p + 4));
    }
    inline double ReadDouble(const uint8_t* p) {
        const uint64_t u = uint64_t(ReadInt64(p));
        double d;
        memcpy(&d, &u, 8);
        return d;
    }

    // Every argument of a message as a number, in order: floats and doubles as they are, ints as floats, T/F as 1/0,
    // time tags as their raw 64-bit value; strings and blobs are skipped. Returns the address; empty on a malformed message.
    inline std::string ReadMessage(const uint8_t* data, int length, std::vector<double>& numbers) {
        numbers.clear();
        int pos = 0;
        std::string address, tags;
        if (!ReadString(data, length, pos, address) || address.empty() || address[0] != '/') return "";
        if (!ReadString(data, length, pos, tags) || tags.empty() || tags[0] != ',') return "";
        for (size_t i = 1; i < tags.size(); i++) {
            switch (tags[i]) {
            case 'f': if (pos + 4 > length) return address; numbers.push_back(ReadFloat(data + pos)); pos += 4; break;
            case 'i': if (pos + 4 > length) return address; numbers.push_back(ReadInt(data + pos)); pos += 4; break;
            case 'd': if (pos + 8 > length) return address; numbers.push_back(ReadDouble(data + pos)); pos += 8; break;
            case 'h': case 't': if (pos + 8 > length) return address; numbers.push_back(double(ReadInt64(data + pos))); pos += 8; break;
            case 'T': numbers.push_back(1); break;
            case 'F': case 'N': numbers.push_back(0); break;
            case 's': case 'S': { std::string s; if (!ReadString(data, length, pos, s)) return address; break; }
            case 'b': { if (pos + 4 > length) return address; const int n = ReadInt(data + pos); pos += 4 + ((n + 3) & ~3); if (pos > length) return address; break; }
            default: return address; // An argument type this reader does not know: the rest cannot be walked.
            }
        }
        return address;
    }

} // namespace gaze_mirror::osc
