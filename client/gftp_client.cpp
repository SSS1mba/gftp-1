#include "gftp_client.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace gftp {

    ClientResult GftpClient::send(const std::string& local_path,
        const std::string& server_ip,
        uint16_t port) {
        ClientResult r;
        last_error_.clear();

        std::error_code ec;
        fs::path src = fs::u8path(local_path);
        if (!fs::exists(src, ec)) {
            r.error = "path does not exist: " + local_path;
            return r;
        }

        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCK) {
            r.error = "socket() failed";
            return r;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) != 1) {
            r.error = "invalid IP: " + server_ip;
            closesocket(s);
            return r;
        }

        LOG_INFO("connecting to " << server_ip << ":" << port);
        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            r.error = "connect() failed";
            closesocket(s);
            return r;
        }
        LOG_INFO("connected");

        const bool is_dir = fs::is_directory(src, ec);
        LOG_INFO("source type: " << (is_dir ? "directory" : "file"));

        bool ok = is_dir ? send_directory(s, local_path)
            : send_single_file(s, local_path);

        if (ok) {
            std::vector<uint8_t> payload;
            put_u8(payload, 0);
            LOG_INFO("sending TRANSFER_END");
            ok = send_frame(s, MsgType::TRANSFER_END, payload)
                && wait_ack(s, (uint8_t)MsgType::TRANSFER_END);
        }

        closesocket(s);

        if (ok) {
            LOG_INFO("transfer completed successfully");
            r.ok = true;
        }
        else {
            r.error = last_error_.empty() ? "transfer failed" : last_error_;
            LOG_ERROR("transfer failed: " << r.error);
        }
        return r;
    }

    bool GftpClient::send_single_file(socket_t s, const std::string& local_path) {
        fs::path src = fs::u8path(local_path);
        std::string name = src.filename().u8string();
        uint64_t size = fs::file_size(src);

        LOG_INFO("FILE_MANIFEST: '" << name << "', size=" << size);

        std::vector<uint8_t> payload;
        put_u16(payload, (uint16_t)name.size());
        put_u64(payload, size);
        put_bytes(payload, name.data(), name.size());

        if (!send_frame(s, MsgType::FILE_MANIFEST, payload, 1)) {
            last_error_ = "send FILE_MANIFEST failed";
            return false;
        }
        if (!wait_ack(s, (uint8_t)MsgType::FILE_MANIFEST)) return false;

        return send_file_data(s, 0, local_path, size);
    }

    bool GftpClient::send_directory(socket_t s, const std::string& local_path) {
        fs::path root = fs::u8path(local_path);
        std::string root_name = root.filename().u8string();

        struct Entry {
            uint8_t     flags;
            std::string rel_path;   // '/' разделители
            uint64_t    size;
            std::string full_path;  // как есть, для открытия файла
        };
        std::vector<Entry> entries;

        std::error_code ec;
        auto opts = fs::directory_options::skip_permission_denied;
        for (auto it = fs::recursive_directory_iterator(root, opts, ec);
            it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                LOG_WARN("skip: " << ec.message());
                ec.clear();
                continue;
            }
            const fs::directory_entry& de = *it;
            fs::path rel = fs::relative(de.path(), root, ec);
            if (ec) { ec.clear(); continue; }
            std::string rel_str = rel.generic_u8string();   // '/' везде

            if (de.is_directory(ec)) {
                entries.push_back({ FLAG_IS_DIR, rel_str, 0, de.path().u8string() });
            }
            else if (de.is_regular_file(ec)) {
                entries.push_back({ 0, rel_str, de.file_size(ec), de.path().u8string() });
            }
            ec.clear();
        }

        if (entries.size() > MAX_FILES) {
            last_error_ = "too many entries: " + std::to_string(entries.size());
            return false;
        }

        LOG_INFO("DIRECTORY_MANIFEST: '" << root_name
            << "', entries=" << entries.size());

        std::vector<uint8_t> payload;
        put_u16(payload, (uint16_t)root_name.size());
        put_bytes(payload, root_name.data(), root_name.size());
        for (const auto& e : entries) {
            put_u8(payload, e.flags);
            put_u16(payload, (uint16_t)e.rel_path.size());
            put_u64(payload, e.size);
            put_bytes(payload, e.rel_path.data(), e.rel_path.size());
        }

        if (!send_frame(s, MsgType::DIRECTORY_MANIFEST, payload,
            (uint16_t)entries.size())) {
            last_error_ = "send DIRECTORY_MANIFEST failed";
            return false;
        }
        if (!wait_ack(s, (uint8_t)MsgType::DIRECTORY_MANIFEST)) return false;

        for (uint16_t i = 0; i < entries.size(); ++i) {
            if (entries[i].flags & FLAG_IS_DIR) {
                LOG_INFO("[" << i << "] [DIR]  " << entries[i].rel_path);
                continue;
            }
            LOG_INFO("[" << i << "] [FILE] " << entries[i].rel_path
                << " (" << entries[i].size << " bytes)");
            if (!send_file_data(s, i, entries[i].full_path, entries[i].size))
                return false;
        }
        return true;
    }

    bool GftpClient::send_file_data(socket_t s, uint16_t file_index,
        const std::string& path, uint64_t size) {
        std::ifstream f(fs::u8path(path), std::ios::binary);
        if (!f) {
            last_error_ = "cannot open: " + path;
            return false;
        }

        std::vector<uint8_t> buf(DATA_CHUNK);
        uint64_t offset = 0;
        uint32_t crc = 0;
        size_t   blocks = 0;

        while (offset < size) {
            size_t to_read = (size_t)std::min<uint64_t>(DATA_CHUNK, size - offset);
            f.read((char*)buf.data(), (std::streamsize)to_read);
            if ((size_t)f.gcount() != to_read) {
                last_error_ = "short read at offset " + std::to_string(offset);
                return false;
            }
            crc = crc32_update(crc, buf.data(), to_read);

            std::vector<uint8_t> payload;
            put_u16(payload, file_index);
            put_u64(payload, offset);
            put_bytes(payload, buf.data(), to_read);

            if (!send_frame(s, MsgType::DATA, payload)) {
                last_error_ = "send DATA failed at offset " + std::to_string(offset);
                return false;
            }
            if (!wait_ack(s, (uint8_t)MsgType::DATA)) return false;

            offset += to_read;
            ++blocks;
        }

        LOG_INFO("file[" << file_index << "] sent: blocks=" << blocks
            << ", bytes=" << offset << ", crc=0x"
            << std::hex << crc << std::dec);

        std::vector<uint8_t> payload;
        put_u16(payload, file_index);
        put_u32(payload, crc);
        if (!send_frame(s, MsgType::FILE_END_MSG, payload)) {
            last_error_ = "send FILE_END failed";
            return false;
        }
        return wait_ack(s, (uint8_t)MsgType::FILE_END_MSG);
    }

    bool GftpClient::wait_ack(socket_t s, uint8_t expected_type) {
        Header h;
        std::vector<uint8_t> payload;
        if (!recv_frame(s, h, payload)) {
            last_error_ = "recv_frame failed while waiting for ACK";
            return false;
        }

        if (h.msg_type == (uint8_t)MsgType::ERROR_MSG) {
            uint16_t code = 0;
            if (payload.size() >= 2) code = get_u16(payload.data());
            last_error_ = "server error 0x" + std::to_string(code);
            LOG_ERROR("server returned ERROR_MSG, code=0x" << std::hex << code << std::dec);
            return false;
        }

        if (h.msg_type != (uint8_t)MsgType::ACK) {
            last_error_ = "expected ACK, got msg_type=0x" + std::to_string(h.msg_type);
            LOG_ERROR(last_error_);
            return false;
        }
        if (payload.size() < 12) {
            last_error_ = "ACK payload too short";
            LOG_ERROR(last_error_);
            return false;
        }

        uint8_t ack_type = payload[0];
        uint8_t status = payload[11];
        if (ack_type != expected_type) {
            last_error_ = "ACK type mismatch";
            LOG_ERROR("ACK type mismatch: got 0x" << std::hex << (int)ack_type
                << ", expected 0x" << (int)expected_type << std::dec);
            return false;
        }
        if (status != 0) {
            last_error_ = "ACK status != 0";
            LOG_ERROR("ACK status=" << (int)status);
            return false;
        }
        return true;
    }

} // namespace gftp