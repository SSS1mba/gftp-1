#include "cli.hpp"
#include "gftp_client.hpp"
#include "../gftp_log.hpp"
#include "../cli_common.hpp"

#include <iostream>

int main() {
    setlocale(LC_ALL, "ru");
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[client] WSAStartup failed\n";
        return 1;
    }
    LOG_INFO("Winsock initialized");
#endif

    gftp::Config cfg;
    const bool from_file = gftp::read_from_config("config.txt", cfg);
    if (!from_file || cfg.mode != "client") {
        LOG_INFO("config.txt not found or mode != client, switching to interactive input");
        if (!gftp::prompt_client(cfg)) { 
            std::cerr << "[client] no configuration provided, exiting\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
    }
    else {
        LOG_INFO("config loaded from config.txt");
    }

    LOG_INFO("mode=" << cfg.mode
        << " path=" << cfg.path
        << " ip=" << cfg.ip
        << " port=" << cfg.port);

    gftp::GftpClient client;
    auto result = client.send(cfg.path, cfg.ip, cfg.port);

    if (result.ok) {
        std::cout << "[client] transfer completed successfully\n";
    }
    else {
        std::cerr << "[client] transfer failed: " << result.error << "\n";
    }
    system("pause");
#ifdef _WIN32
    WSACleanup();
#endif
    return result.ok ? 0 : 1;
}