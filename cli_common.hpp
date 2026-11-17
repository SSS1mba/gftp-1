#pragma once
#include "protocol.hpp"
#include <string>
#include <fstream> 

namespace gftp {

    struct Config {
        std::string mode;
        std::string path;
        std::string ip;
        uint16_t    port = DEFAULT_PORT;
    };

    inline bool read_from_config(const std::string& filename, Config& cfg) {
        std::ifstream f(filename);
        if (!f) return false;

        bool has_mode = false, has_path = false, has_port = false, has_ip = false;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());

            if (key == "mode") { cfg.mode = val; has_mode = true; }
            else if (key == "path") { cfg.path = val; has_path = true; }
            else if (key == "ip") { cfg.ip = val; has_ip = true; }
            else if (key == "port") { cfg.port = (uint16_t)std::stoi(val); has_port = true; }
        }

        // ѕровер€ем только то, что реально используетс€.
        // —ервер игнорирует ip, клиент Ч тоже не падает без него,
        // если prompt_client потом доспросит.
        return has_mode && has_path && has_port;
    }

} // namespace gftp