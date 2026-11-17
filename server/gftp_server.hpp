#pragma once
#include "../protocol.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gftp {

    class GftpServer {
    public:
        GftpServer() = default;
        ~GftpServer();

        bool start(const std::string& save_dir, uint16_t port, std::string& error);
        void stop();
        void run_blocking();

    private:
        void handle_client(socket_t client);
        bool process_manifest(socket_t s, const Header& h, const std::vector<uint8_t>& payload);
        bool recv_file(socket_t s, uint16_t file_index,
            const std::string& path, uint64_t expected_size);

        socket_t listen_sock_ = INVALID_SOCK;
        std::string save_dir_;
        std::atomic<bool> running_{ false };
        std::mutex threads_mutex_;
        std::vector<std::thread> threads_;
    };

} // namespace gftp