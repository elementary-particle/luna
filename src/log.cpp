#include "log.h"

#include <SDL3/SDL_log.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

namespace luna::log {
namespace {

std::mutex g_log_mutex;
FILE *g_log_file = nullptr;

const char *PriorityName(SDL_LogPriority priority) {
  switch (priority) {
  case SDL_LOG_PRIORITY_TRACE:
    return "TRACE";
  case SDL_LOG_PRIORITY_VERBOSE:
    return "VERBOSE";
  case SDL_LOG_PRIORITY_DEBUG:
    return "DEBUG";
  case SDL_LOG_PRIORITY_INFO:
    return "INFO";
  case SDL_LOG_PRIORITY_WARN:
    return "WARN";
  case SDL_LOG_PRIORITY_ERROR:
    return "ERROR";
  case SDL_LOG_PRIORITY_CRITICAL:
    return "CRITICAL";
  default:
    return "UNKNOWN";
  }
}

SDL_LogPriority ToPriority(Level level) {
  switch (level) {
  case Level::Debug:
    return SDL_LOG_PRIORITY_DEBUG;
  case Level::Info:
    return SDL_LOG_PRIORITY_INFO;
  case Level::Warn:
    return SDL_LOG_PRIORITY_WARN;
  case Level::Error:
    return SDL_LOG_PRIORITY_ERROR;
  }
  return SDL_LOG_PRIORITY_INFO;
}

std::optional<SDL_LogPriority> ParsePriority(const char *value) {
  if (!value || !*value) {
    return std::nullopt;
  }

  std::string level = value;
  for (char &ch : level) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }

  if (level == "trace") {
    return SDL_LOG_PRIORITY_TRACE;
  }
  if (level == "verbose") {
    return SDL_LOG_PRIORITY_VERBOSE;
  }
  if (level == "debug") {
    return SDL_LOG_PRIORITY_DEBUG;
  }
  if (level == "info") {
    return SDL_LOG_PRIORITY_INFO;
  }
  if (level == "warn" || level == "warning") {
    return SDL_LOG_PRIORITY_WARN;
  }
  if (level == "error") {
    return SDL_LOG_PRIORITY_ERROR;
  }
  if (level == "critical") {
    return SDL_LOG_PRIORITY_CRITICAL;
  }

  return std::nullopt;
}

std::string TimestampNow() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif

  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm) == 0) {
    return "0000-00-00 00:00:00";
  }
  return buffer;
}

void Output(void * /*userdata*/, int category, SDL_LogPriority priority,
            const char *message) {
  const char *priority_name = PriorityName(priority);
  const std::string timestamp = TimestampNow();

  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::fprintf(stderr,
               "%s [%s] %s\n",
               timestamp.c_str(),
               priority_name,
               message ? message : "");
  std::fflush(stderr);

  if (g_log_file) {
    std::fprintf(g_log_file,
                 "%s [%s] %s\n",
                 timestamp.c_str(),
                 priority_name,
                 message ? message : "");
    std::fflush(g_log_file);
  }
  (void)category;
}

} // namespace

void Init() {
  SDL_SetLogOutputFunction(Output, nullptr);

#if !defined(NDEBUG)
  SDL_LogPriority default_priority = SDL_LOG_PRIORITY_DEBUG;
#else
  SDL_LogPriority default_priority = SDL_LOG_PRIORITY_INFO;
#endif
  if (const char *value = std::getenv("LUNA_LOG_LEVEL")) {
    if (std::optional<SDL_LogPriority> parsed = ParsePriority(value)) {
      default_priority = *parsed;
    } else {
      std::fprintf(stderr,
                   "%s [WARN] invalid LUNA_LOG_LEVEL '%s', using default\n",
                   TimestampNow().c_str(),
                   value);
      std::fflush(stderr);
    }
  }

  SDL_SetLogPriorities(default_priority);

  const char *path = std::getenv("LUNA_LOG_FILE");
  if (path && *path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_file = std::fopen(path, "a");
    if (!g_log_file) {
      std::fprintf(stderr,
                   "%s [WARN] failed to open log file: %s\n",
                   TimestampNow().c_str(),
                   path);
      std::fflush(stderr);
    }
  }

  Write(Level::Info,
        "log",
        fmt::format("log level set to {}", PriorityName(default_priority)));
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_file) {
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
}

void Write(Level level, std::string_view subsystem, std::string_view message) {
  const std::string line = fmt::format("[{}] {}", subsystem, message);
  SDL_LogMessage(
      SDL_LOG_CATEGORY_APPLICATION, ToPriority(level), "%s", line.c_str());
}

} // namespace luna::log
