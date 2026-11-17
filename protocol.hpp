#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK (-1)
#define closesocket close
#define SOCKET_ERROR (-1)
#endif

#include <array> 
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gftp {

    constexpr uint8_t  VERSION = 1;
    constexpr uint16_t DEFAULT_PORT = 20010;
    constexpr size_t   HEADER_SIZE = 12;
    constexpr size_t   MAX_PAYLOAD = 1024 * 1024;   // 1 МиБ
    constexpr size_t   DATA_CHUNK = 64 * 1024;     // 64 КиБ
    constexpr size_t   MAX_PATH_LEN = 4096;
    constexpr uint16_t MAX_FILES = 65535;

    enum class MsgType : uint8_t {
        FILE_MANIFEST = 0x00,
        DIRECTORY_MANIFEST = 0x01,
        DATA = 0x10,
        FILE_END_MSG = 0x11,
        TRANSFER_END = 0x12,
        ACK = 0x20,
        ERROR_MSG = 0x7F
    };

    enum class ErrCode : uint16_t {
        NONE = 0,
        BAD_VERSION = 0x0001,
        BAD_FRAME = 0x0002,
        BAD_PATH = 0x0003,
        DISK_WRITE = 0x0004,
        CRC_MISMATCH = 0x0005,
        TIMEOUT = 0x0006,
        PAYLOAD_BIG = 0x0007
    };

    enum EntryFlag : uint8_t {
        FLAG_IS_DIR = 0x01
    };

    // ---------- 64-битный network byte order ----------
    inline uint64_t htonll(uint64_t v) {
        static const int one = 1;
        if (*(const char*)&one == 1) {   // little-endian
            uint32_t hi = (uint32_t)(v >> 32);
            uint32_t lo = (uint32_t)(v & 0xFFFFFFFFu);
            return ((uint64_t)htonl(lo) << 32) | htonl(hi);
        }
        return v;
    }
    inline uint64_t ntohll(uint64_t v) { return htonll(v); }

    // ---------- CRC32 (IEEE 802.3, как в zlib) ----------
    inline const uint32_t* crc32_table() {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
            }();
        return table.data();
    }

    inline uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
        const uint32_t* t = crc32_table();
        crc = ~crc;
        for (size_t i = 0; i < len; ++i)
            crc = t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    inline uint32_t crc32_buf(const uint8_t* data, size_t len) {
        return crc32_update(0, data, len);
    }

    // ---------- Заголовок кадра ----------
    struct Header {
        uint8_t  version = VERSION;
        uint8_t  msg_type = 0;
        uint16_t flags = 0;
        uint32_t payload_len = 0;
        uint16_t file_count = 0;
        uint16_t reserved = 0;
    };

    inline void serialize_header(const Header& h, uint8_t out[HEADER_SIZE]) {
        out[0] = h.version;
        out[1] = h.msg_type;
        uint16_t flags = htons(h.flags);        std::memcpy(out + 2, &flags, 2);
        uint32_t plen = htonl(h.payload_len);   std::memcpy(out + 4, &plen, 4);
        uint16_t fc = htons(h.file_count);      std::memcpy(out + 8, &fc, 2);
        uint16_t res = htons(h.reserved);       std::memcpy(out + 10, &res, 2);
    }

    inline Header deserialize_header(const uint8_t in[HEADER_SIZE]) {
        Header h;
        h.version = in[0];
        h.msg_type = in[1];
        uint16_t flags; std::memcpy(&flags, in + 2, 2);     h.flags = ntohs(flags);
        uint32_t plen;  std::memcpy(&plen, in + 4, 4);      h.payload_len = ntohl(plen);
        uint16_t fc;    std::memcpy(&fc, in + 8, 2);        h.file_count = ntohs(fc);
        uint16_t res;   std::memcpy(&res, in + 10, 2);      h.reserved = ntohs(res);
        return h;
    }

    // ---------- Обмен байтами поверх сокета ----------
    inline bool send_all(socket_t s, const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < len) {
            int n = ::send(s, p + sent, (int)(len - sent), 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    inline bool recv_all(socket_t s, void* data, size_t len) {
        char* p = static_cast<char*>(data);
        size_t got = 0;
        while (got < len) {
            int n = ::recv(s, p + got, (int)(len - got), 0);
            if (n <= 0) return false;
            got += (size_t)n;
        }
        return true;
    }

    // ---------- Кадр целиком ----------
    inline bool send_frame(socket_t s, MsgType type,
        const std::vector<uint8_t>& payload,
        uint16_t file_count = 0) {
        Header h;
        h.version = VERSION;
        h.msg_type = (uint8_t)type;
        h.flags = 0;
        h.payload_len = (uint32_t)payload.size();
        h.file_count = file_count;
        h.reserved = 0;

        uint8_t buf[HEADER_SIZE];
        serialize_header(h, buf);
        if (!send_all(s, buf, HEADER_SIZE)) return false;
        if (!payload.empty() && !send_all(s, payload.data(), payload.size())) return false;
        return true;
    }

    inline bool recv_frame(socket_t s, Header& h, std::vector<uint8_t>& payload) {
        uint8_t buf[HEADER_SIZE];
        if (!recv_all(s, buf, HEADER_SIZE)) return false;
        h = deserialize_header(buf);
        if (h.payload_len > MAX_PAYLOAD) return false;
        payload.resize(h.payload_len);
        if (h.payload_len > 0 && !recv_all(s, payload.data(), h.payload_len)) return false;
        return true;
    }

    // ---------- Утилиты путей ----------
    inline bool is_safe_relative_path(const std::string& p) {
        if (p.empty() || p.size() > MAX_PATH_LEN) return false;
        if (p[0] == '/' || p[0] == '\\') return false;                 // абсолютный
        if (p.size() >= 2 && p[1] == ':') return false;                // "C:..."
        size_t i = 0;
        while (i < p.size()) {
            size_t j = p.find_first_of("/\\", i);
            if (j == std::string::npos) j = p.size();
            if (p.compare(i, j - i, "..") == 0) return false;
            i = j + 1;
        }
        return true;
    }

    // ---------- Сериализация полей ----------
    inline void put_u8(std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
    inline void put_u16(std::vector<uint8_t>& v, uint16_t x) {
        uint16_t n = htons(x); size_t o = v.size(); v.resize(o + 2);
        std::memcpy(v.data() + o, &n, 2);
    }
    inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
        uint32_t n = htonl(x); size_t o = v.size(); v.resize(o + 4);
        std::memcpy(v.data() + o, &n, 4);
    }
    inline void put_u64(std::vector<uint8_t>& v, uint64_t x) {
        uint64_t n = htonll(x); size_t o = v.size(); v.resize(o + 8);
        std::memcpy(v.data() + o, &n, 8);
    }
    inline void put_bytes(std::vector<uint8_t>& v, const void* p, size_t n) {
        size_t o = v.size(); v.resize(o + n);
        std::memcpy(v.data() + o, p, n);
    }

    inline uint8_t  get_u8(const uint8_t* p) { return p[0]; }
    inline uint16_t get_u16(const uint8_t* p) { uint16_t x; std::memcpy(&x, p, 2); return ntohs(x); }
    inline uint32_t get_u32(const uint8_t* p) { uint32_t x; std::memcpy(&x, p, 4); return ntohl(x); }
    inline uint64_t get_u64(const uint8_t* p) { uint64_t x; std::memcpy(&x, p, 8); return ntohll(x); }

} // namespace gftp