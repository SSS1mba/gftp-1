#include "cli.hpp"
#include "gftp_server.hpp"
#include "../gftp_log.hpp"

#include <csignal>
#include <iostream>
#include <string>

namespace {

    gftp::GftpServer* g_server = nullptr;

    void on_sigint(int) {
        LOG_INFO("SIGINT received, stopping server");
        if (g_server) g_server->stop();
    }

} // namespace

int main() {
    setlocale(LC_ALL, "ru");
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[server] WSAStartup failed\n";
        return 1;
    }
    LOG_INFO("Winsock initialized");
#endif

    gftp::Config cfg;
    const bool from_file = gftp::read_from_config("config.txt", cfg);
    if (!from_file || cfg.mode != "server") {
        LOG_INFO("config.txt not found or mode != server, switching to interactive input");
        if (!gftp::prompt_server(cfg)) {
            std::cerr << "[server] no configuration provided, exiting\n";
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
        << " port=" << cfg.port);

    gftp::GftpServer server;
    g_server = &server;
    std::signal(SIGINT, on_sigint);

    std::string err;
    if (!server.start(cfg.path, cfg.port, err)) {
        std::cerr << "[server] start failed: " << err << "\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "[server] listening on port " << cfg.port << "\n";
    std::cout << "[server] save dir: " << cfg.path << "\n";
    std::cout << "[server] press Ctrl+C to stop\n";

    server.run_blocking();

    LOG_INFO("server stopped");
    system("pause");
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}