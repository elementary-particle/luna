#ifndef LUNA_LOG_H
#define LUNA_LOG_H

#include <fmt/core.h>

#include <string_view>
#include <utility>

namespace luna::log {

enum class Level {
  Debug = 0,
  Info,
  Warn,
  Error,
};

void Init();
void Shutdown();

void Write(Level level, std::string_view subsystem, std::string_view message);

template <typename... Args>
inline void Debug(std::string_view subsystem, fmt::format_string<Args...> fmt_str,
                  Args &&...args) {
  Write(Level::Debug, subsystem,
        fmt::format(fmt_str, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Info(std::string_view subsystem, fmt::format_string<Args...> fmt_str,
                 Args &&...args) {
  Write(Level::Info, subsystem, fmt::format(fmt_str, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Warn(std::string_view subsystem, fmt::format_string<Args...> fmt_str,
                 Args &&...args) {
  Write(Level::Warn, subsystem, fmt::format(fmt_str, std::forward<Args>(args)...));
}

template <typename... Args>
inline void Error(std::string_view subsystem,
                  fmt::format_string<Args...> fmt_str, Args &&...args) {
  Write(Level::Error, subsystem,
        fmt::format(fmt_str, std::forward<Args>(args)...));
}

} // namespace luna::log

#endif
