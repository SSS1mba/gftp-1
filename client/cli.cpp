#include "cli.hpp"
#include <iostream>

namespace gftp {
    bool prompt_client(Config& cfg) {
        std::string s;
        std::cout << "Source path (file or folder): ";
        std::getline(std::cin, s);
        if (s.empty()) return false;
        cfg.path = s;

        std::cout << "Server IP: ";
        std::getline(std::cin, s);
        if (s.empty()) return false;
        cfg.ip = s;

        std::cout << "Port [" << DEFAULT_PORT << "]: ";
        std::getline(std::cin, s);
        cfg.port = s.empty() ? DEFAULT_PORT : (uint16_t)std::stoi(s);

        cfg.mode = "client";
        return true;
    }
}