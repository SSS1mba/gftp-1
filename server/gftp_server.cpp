#include "gftp_server.hpp"
#include "../gftp_log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace gftp {

    // ---------- StageGuard: транзакция на папку ----------
    namespace {

        class StageGuard {
        public:
            StageGuard(fs::path staging, fs::path final)
                : staging_(std::move(staging)), final_(std::move(final)) {
                LOG_INFO("staging dir: " << staging_.u8string()
                    << " -> final: " << final_.u8string());
            }

            ~StageGuard() {
                if (!committed_) {
                    std::error_code ec;
                    fs::remove_all(staging_, ec);
                    LOG_WARN("rollback: removed staging " << staging_.u8string()
                        << (ec ? (" (ec: " + ec.message() + ")") : ""));
                }
            }

            StageGuard(const StageGuard&) = delete;
            StageGuard& operator=(const StageGuard&) = delete;

            bool commit() {
                std::error_code ec;
                fs::rename(staging_, final_, ec);
                if (ec) {
                    LOG_ERROR("commit rename failed: " << ec.message());
                    return false;
                }
                committed_ = true;
                LOG_INFO("commit OK -> " << final_.u8string());
                return true;
            }

            const fs::path& staging() const { return staging_; }

        private:
            fs::path staging_;
            fs::path final_;
            bool committed_ = false;
        };

        std::string make_tx_id() {
            using namespace std::chrono;
            auto now = system_clock::now().time_since_epoch().count();
            static std::mt19937_64 rng(now);
            std::ostringstream oss;
            oss << "tx_" << std::hex << now << "_" << rng();
            return oss.str();
        }

    } // namespace

    // ---------- GftpServer ----------

    GftpServer::~GftpServer() { stop(); }

    bool GftpServer::start(const std::string& save_dir, uint16_t port, std::string& error) {
        save_dir_ = save_dir;
        std::error_code ec;
        fs::create_directories(fs::u8path(save_dir_), ec);
        if (ec) {
            error = "cannot create save dir: " + ec.message();
            LOG_ERROR("start: " << error);
            return false;
        }
        fs::create_directories(fs::u8path(save_dir_) / ".gftp_staging", ec);

        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == INVALID_SOCK) {
            error = "socket() failed";
            LOG_ERROR("start: " << error);
            return false;
        }

        int yes = 1;
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
            (const char*)&yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(listen_sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            error = "bind() failed";
            LOG_ERROR("start: " << error);
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return false;
        }
        if (::listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR) {
            error = "listen() failed";
            LOG_ERROR("start: " << error);
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCK;
            return false;
        }
        LOG_INFO("server listening on :" << port << ", save_dir=" << save_dir_);
        return true;
    }

    void GftpServer::stop() {
        LOG_INFO("stop requested");
        running_ = false;
        if (listen_sock_ != INVALID_SOCK) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCK;
        }
    }

    void GftpServer::run_blocking() {
        running_ = true;
        while (running_) {
            sockaddr_in client_addr{};
            int len = sizeof(client_addr);
            socket_t c = ::accept(listen_sock_, (sockaddr*)&client_addr, &len);
            if (c == INVALID_SOCK) {
                if (!running_) break;
                continue;
            }
            char ip[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            LOG_INFO("client connected: " << ip << ":" << ntohs(client_addr.sin_port));

            std::lock_guard<std::mutex> lk(threads_mutex_);
            threads_.emplace_back(&GftpServer::handle_client, this, c);
        }
        std::lock_guard<std::mutex> lk(threads_mutex_);
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
    }

    void GftpServer::handle_client(socket_t c) {
        LOG_INFO("handle_client: start");

        Header h;
        std::vector<uint8_t> payload;
        if (!recv_frame(c, h, payload)) {
            LOG_ERROR("handle_client: failed to read first frame");
            closesocket(c);
            return;
        }
        if (h.version != VERSION) {
            LOG_ERROR("handle_client: bad version " << (int)h.version);
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::BAD_VERSION);
            put_u16(err, 0);
            send_frame(c, MsgType::ERROR_MSG, err);
            closesocket(c);
            return;
        }
        if (h.msg_type != (uint8_t)MsgType::FILE_MANIFEST &&
            h.msg_type != (uint8_t)MsgType::DIRECTORY_MANIFEST) {
            LOG_ERROR("handle_client: expected manifest, got 0x"
                << std::hex << (int)h.msg_type << std::dec);
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::BAD_FRAME);
            put_u16(err, 0);
            send_frame(c, MsgType::ERROR_MSG, err);
            closesocket(c);
            return;
        }

        LOG_INFO("handle_client: manifest type=0x" << std::hex << (int)h.msg_type
            << std::dec << ", file_count=" << h.file_count);

        if (!process_manifest(c, h, payload)) {
            LOG_WARN("handle_client: process_manifest failed, closing");
            closesocket(c);
            return;
        }

        if (!recv_frame(c, h, payload)) {
            LOG_ERROR("handle_client: failed to read TRANSFER_END");
            closesocket(c);
            return;
        }
        if (h.msg_type != (uint8_t)MsgType::TRANSFER_END) {
            LOG_ERROR("handle_client: expected TRANSFER_END, got 0x"
                << std::hex << (int)h.msg_type << std::dec);
            closesocket(c);
            return;
        }

        std::vector<uint8_t> ack;
        put_u8(ack, (uint8_t)MsgType::TRANSFER_END);
        put_u16(ack, 0xFFFF);
        put_u64(ack, 0);
        put_u8(ack, 0);
        send_frame(c, MsgType::ACK, ack);

        LOG_INFO("handle_client: transfer completed, closing");
        closesocket(c);
    }

    bool GftpServer::process_manifest(socket_t s, const Header& h,
        const std::vector<uint8_t>& payload) {
        const uint8_t* p = payload.data();
        size_t off = 0;

        // ---------- Один файл ----------
        if (h.msg_type == (uint8_t)MsgType::FILE_MANIFEST) {
            if (payload.size() < 10) return false;
            uint16_t plen = get_u16(p + off); off += 2;
            uint64_t size = get_u64(p + off); off += 8;
            if (off + plen > payload.size()) return false;
            std::string name((const char*)p + off, plen); off += plen;

            LOG_INFO("manifest: single file '" << name << "', size=" << size);

            if (!is_safe_relative_path(name)) {
                LOG_ERROR("unsafe path: " << name);
                std::vector<uint8_t> err;
                put_u16(err, (uint16_t)ErrCode::BAD_PATH);
                put_u16(err, 0);
                send_frame(s, MsgType::ERROR_MSG, err);
                return false;
            }

            std::vector<uint8_t> ack;
            put_u8(ack, (uint8_t)MsgType::FILE_MANIFEST);
            put_u16(ack, 0xFFFF);
            put_u64(ack, 0);
            put_u8(ack, 0);
            if (!send_frame(s, MsgType::ACK, ack)) return false;

            // Транзакция для одного файла: staging + rename
            fs::path final_path = fs::u8path(save_dir_) / fs::u8path(name);
            if (fs::exists(final_path)) {
                LOG_ERROR("destination exists: " << final_path.u8string());
                std::vector<uint8_t> err;
                put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
                put_u16(err, 0);
                send_frame(s, MsgType::ERROR_MSG, err);
                return false;
            }

            fs::path staging = fs::u8path(save_dir_) / ".gftp_staging"
                / (make_tx_id() + ".part");
            StageGuard guard(staging, final_path);

            if (!recv_file(s, 0, guard.staging().u8string(), size)) return false;
            return guard.commit();
        }

        // ---------- Директория ----------
        if (payload.size() < 2) return false;
        uint16_t root_len = get_u16(p + off); off += 2;
        if (off + root_len > payload.size()) return false;
        std::string root_name((const char*)p + off, root_len); off += root_len;

        LOG_INFO("manifest: directory '" << root_name
            << "', file_count=" << h.file_count);

        if (!is_safe_relative_path(root_name)) {
            LOG_ERROR("unsafe root path: " << root_name);
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::BAD_PATH);
            put_u16(err, 0);
            send_frame(s, MsgType::ERROR_MSG, err);
            return false;
        }

        struct EntryInfo {
            uint8_t     flags;
            std::string path;
            uint64_t    size;
        };
        std::vector<EntryInfo> entries;
        entries.reserve(h.file_count);

        for (uint16_t i = 0; i < h.file_count; ++i) {
            if (off + 11 > payload.size()) return false;
            uint8_t  eflags = get_u8(p + off); off += 1;
            uint16_t plen = get_u16(p + off); off += 2;
            uint64_t size = get_u64(p + off); off += 8;
            if (off + plen > payload.size()) return false;
            std::string epath((const char*)p + off, plen); off += plen;

            LOG_INFO("  entry[" << i << "] "
                << ((eflags & FLAG_IS_DIR) ? "[DIR]  " : "[FILE] ")
                << epath
                << (eflags & FLAG_IS_DIR ? "" : (" size=" + std::to_string(size))));

            if (!is_safe_relative_path(epath)) {
                LOG_ERROR("unsafe entry path: " << epath);
                std::vector<uint8_t> err;
                put_u16(err, (uint16_t)ErrCode::BAD_PATH);
                put_u16(err, 0);
                send_frame(s, MsgType::ERROR_MSG, err);
                return false;
            }
            entries.push_back({ eflags, epath, size });
        }

        // ACK на манифест — только после проверки всех путей
        std::vector<uint8_t> ack;
        put_u8(ack, (uint8_t)MsgType::DIRECTORY_MANIFEST);
        put_u16(ack, 0xFFFF);
        put_u64(ack, 0);
        put_u8(ack, 0);
        if (!send_frame(s, MsgType::ACK, ack)) return false;

        // Транзакция: staging-дерево, потом rename целиком
        fs::path final_root = fs::u8path(save_dir_) / fs::u8path(root_name);
        if (fs::exists(final_root)) {
            LOG_ERROR("destination exists: " << final_root.u8string());
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
            put_u16(err, 0);
            send_frame(s, MsgType::ERROR_MSG, err);
            return false;
        }

        fs::path staging = fs::u8path(save_dir_) / ".gftp_staging" / make_tx_id();
        StageGuard guard(staging, final_root);

        std::error_code ec;
        fs::create_directories(guard.staging(), ec);
        if (ec) {
            LOG_ERROR("staging create failed: " << ec.message());
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
            put_u16(err, 0);
            send_frame(s, MsgType::ERROR_MSG, err);
            return false;
        }

        for (uint16_t i = 0; i < (uint16_t)entries.size(); ++i) {
            const auto& e = entries[i];
            fs::path dst = guard.staging() / fs::u8path(e.path);

            if (e.flags & FLAG_IS_DIR) {
                LOG_INFO("mkdir " << e.path);
                fs::create_directories(dst, ec);
                if (ec) {
                    LOG_ERROR("mkdir failed: " << e.path << " (" << ec.message() << ")");
                    std::vector<uint8_t> err;
                    put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
                    put_u16(err, 0);
                    send_frame(s, MsgType::ERROR_MSG, err);
                    return false;
                }
            }
            else {
                fs::create_directories(dst.parent_path(), ec);
                LOG_INFO("recv file[" << i << "] " << e.path
                    << " (" << e.size << " bytes)");
                if (!recv_file(s, i, dst.u8string(), e.size)) {
                    LOG_WARN("recv_file failed for " << e.path << " — rolling back");
                    return false;   // StageGuard удалит staging
                }
            }
        }

        return guard.commit();
    }

    bool GftpServer::recv_file(socket_t s, uint16_t file_index,
        const std::string& path, uint64_t expected_size) {
        fs::path dst = fs::u8path(path);
        std::ofstream f(dst, std::ios::binary);
        if (!f) {
            LOG_ERROR("cannot open for write: " << path);
            std::vector<uint8_t> err;
            put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
            put_u16(err, 0);
            send_frame(s, MsgType::ERROR_MSG, err);
            return false;
        }

        uint64_t expected_offset = 0;
        uint32_t crc = 0;
        size_t   blocks = 0;

        while (true) {
            Header h;
            std::vector<uint8_t> payload;
            if (!recv_frame(s, h, payload)) {
                LOG_ERROR("recv_file[" << file_index
                    << "]: recv_frame failed at offset " << expected_offset);
                return false;
            }

            if (h.msg_type == (uint8_t)MsgType::DATA) {
                if (payload.size() < 10) {
                    LOG_ERROR("recv_file[" << file_index << "]: DATA too short");
                    return false;
                }
                const uint8_t* p = payload.data();
                uint16_t idx = get_u16(p);
                uint64_t offset = get_u64(p + 2);

                if (idx != file_index) {
                    LOG_ERROR("recv_file: idx mismatch (got " << idx
                        << ", expected " << file_index << ")");
                    return false;
                }
                if (offset != expected_offset) {
                    LOG_ERROR("recv_file[" << file_index
                        << "]: offset mismatch (got " << offset
                        << ", expected " << expected_offset << ")");
                    return false;
                }

                size_t data_len = payload.size() - 10;
                if (data_len > DATA_CHUNK) {
                    LOG_ERROR("recv_file: data_len " << data_len
                        << " > " << DATA_CHUNK);
                    return false;
                }

                f.write((const char*)p + 10, (std::streamsize)data_len);
                if (!f) {
                    LOG_ERROR("recv_file[" << file_index
                        << "]: disk write failed at offset " << expected_offset);
                    std::vector<uint8_t> err;
                    put_u16(err, (uint16_t)ErrCode::DISK_WRITE);
                    put_u16(err, 0);
                    send_frame(s, MsgType::ERROR_MSG, err);
                    return false;
                }
                crc = crc32_update(crc, p + 10, data_len);
                expected_offset += data_len;
                ++blocks;

                std::vector<uint8_t> ack;
                put_u8(ack, (uint8_t)MsgType::DATA);
                put_u16(ack, file_index);
                put_u64(ack, expected_offset);
                put_u8(ack, 0);
                if (!send_frame(s, MsgType::ACK, ack)) {
                    LOG_ERROR("recv_file[" << file_index << "]: ACK send failed");
                    return false;
                }
            }
            else if (h.msg_type == (uint8_t)MsgType::FILE_END_MSG) {
                if (payload.size() < 6) return false;
                const uint8_t* p = payload.data();
                uint16_t idx = get_u16(p);
                uint32_t got_crc = get_u32(p + 2);
                if (idx != file_index) {
                    LOG_ERROR("FILE_END: idx mismatch");
                    return false;
                }

                LOG_INFO("recv_file[" << file_index << "]: FILE_END, size="
                    << expected_offset << "/" << expected_size
                    << ", blocks=" << blocks
                    << ", crc=0x" << std::hex << crc
                    << "/0x" << got_crc << std::dec);

                if (expected_offset != expected_size) {
                    LOG_ERROR("size mismatch: " << expected_offset
                        << " != " << expected_size);
                    std::vector<uint8_t> err;
                    put_u16(err, (uint16_t)ErrCode::BAD_FRAME);
                    put_u16(err, 0);
                    send_frame(s, MsgType::ERROR_MSG, err);
                    return false;
                }
                if (got_crc != crc) {
                    LOG_ERROR("CRC mismatch: got 0x" << std::hex << got_crc
                        << ", computed 0x" << crc << std::dec);
                    std::vector<uint8_t> err;
                    put_u16(err, (uint16_t)ErrCode::CRC_MISMATCH);
                    put_u16(err, 0);
                    send_frame(s, MsgType::ERROR_MSG, err);
                    return false;
                }

                std::vector<uint8_t> ack;
                put_u8(ack, (uint8_t)MsgType::FILE_END_MSG);
                put_u16(ack, file_index);
                put_u64(ack, expected_offset);
                put_u8(ack, 0);
                return send_frame(s, MsgType::ACK, ack);
            }
            else {
                LOG_ERROR("recv_file[" << file_index
                    << "]: unexpected type 0x" << std::hex
                    << (int)h.msg_type << std::dec);
                return false;
            }
        }
    }

} // namespace gftp