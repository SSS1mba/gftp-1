#include "cli.hpp"
#include <iostream>

namespace gftp {
    bool prompt_server(Config& cfg) {
        std::string s;
        std::cout << "Save directory: ";
        std::getline(std::cin, s);
        if (s.empty()) return false;
        cfg.path = s;

        std::cout << "Port [" << DEFAULT_PORT << "]: ";
        std::getline(std::cin, s);
        cfg.port = s.empty() ? DEFAULT_PORT : (uint16_t)std::stoi(s);

        cfg.mode = "server";
        return true;
    }
}