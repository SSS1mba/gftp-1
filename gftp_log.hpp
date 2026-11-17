#pragma once
#include <iostream>
#include <sstream>
#include <mutex>
#include <string>

namespace gftp {

    inline std::mutex& log_mutex() {
        static std::mutex m;
        return m;
    }

    inline void log_line(const std::string& line) {
        std::lock_guard<std::mutex> lk(log_mutex());
        std::cerr << line << "\n";
    }

} // namespace gftp

// Включение логов: раскомментируй строку или задай /DLOG в настройках проекта
 #define LOG

#ifdef LOG
#define LOG_INFO(msg)  do { \
      std::ostringstream _oss; \
      _oss << "[INFO]  " << __FILE__ << ":" << __LINE__ << "  " << msg; \
      ::gftp::log_line(_oss.str()); \
  } while (0)

#define LOG_WARN(msg)  do { \
      std::ostringstream _oss; \
      _oss << "[WARN]  " << __FILE__ << ":" << __LINE__ << "  " << msg; \
      ::gftp::log_line(_oss.str()); \
  } while (0)

#define LOG_ERROR(msg) do { \
      std::ostringstream _oss; \
      _oss << "[ERROR] " << __FILE__ << ":" << __LINE__ << "  " << msg; \
      ::gftp::log_line(_oss.str()); \
  } while (0)
#else
#define LOG_INFO(msg)  ((void)0)
#define LOG_WARN(msg)  ((void)0)
#define LOG_ERROR(msg) ((void)0)
#endif