#pragma once
#include "../protocol.hpp"
#include "../gftp_log.hpp"

#include <string>

namespace gftp {

    struct ClientResult {
        bool ok = false;
        std::string error;
    };

    class GftpClient {
    public:
        ClientResult send(const std::string& local_path,
            const std::string& server_ip,
            uint16_t port);

    private:
        bool send_single_file(socket_t s, const std::string& local_path);
        bool send_directory(socket_t s, const std::string& local_path);
        bool send_file_data(socket_t s, uint16_t file_index,
            const std::string& path, uint64_t size);
        bool wait_ack(socket_t s, uint8_t expected_type);

        std::string last_error_;
    };

} // namespace gftp