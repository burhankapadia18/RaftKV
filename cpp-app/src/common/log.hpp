#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kvdb {
namespace log {

/** @brief Severity, ordered so a threshold comparison works. */
enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

/** @brief Parse a level name; anything unrecognized becomes info. */
[[nodiscard]] inline Level parse_level(const std::string &name) {
  if (name == "debug")
    return Level::kDebug;
  if (name == "warn" || name == "warning")
    return Level::kWarn;
  if (name == "error")
    return Level::kError;
  return Level::kInfo;
}

[[nodiscard]] inline const char *level_name(Level level) {
  switch (level) {
  case Level::kDebug:
    return "debug";
  case Level::kInfo:
    return "info";
  case Level::kWarn:
    return "warn";
  case Level::kError:
    return "error";
  }
  return "info";
}

/** @brief One structured field. Values are always rendered as JSON strings. */
struct Field {
  std::string key;
  std::string value;
};

/** @brief Convenience builders so call sites stay short. */
[[nodiscard]] inline Field field(std::string key, std::string value) {
  return Field{std::move(key), std::move(value)};
}
[[nodiscard]] inline Field field(std::string key, long long value) {
  return Field{std::move(key), std::to_string(value)};
}
[[nodiscard]] inline Field field(std::string key, size_t value) {
  return Field{std::move(key), std::to_string(value)};
}

/**
 * @brief Escape a string for a JSON string literal.
 *
 * Duplicated in spirit from http_server.hpp's json_escape, but deliberately not
 * shared: that one lives in the network layer and this header must not depend
 * on it (main.cpp and the storage layer log too). Both are ten lines and
 * neither is going to change — the alternative is a common/json.hpp for one
 * function.
 */
[[nodiscard]] inline std::string escape(const std::string &input) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(input.size() + 8);
  for (const char c : input) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (byte < 0x20) {
        out += "\\u00";
        out += kHex[(byte >> 4) & 0x0f];
        out += kHex[byte & 0x0f];
      } else {
        out += c;
      }
      break;
    }
  }
  return out;
}

/**
 * @brief Minimal leveled JSON-lines logger (R5.2).
 *
 * One object per line on stdout:
 * {"ts","level","node_id","component","msg",...}. JSON rather than prose for
 * the same reason as the Go side — with three nodes writing into one `docker
 * compose logs` stream, a line that cannot be filtered by node and component is
 * nearly useless during an incident, and that is the only time anyone reads it.
 *
 * Writes are serialized under a mutex. The HTTP worker pool means several
 * threads log concurrently, and interleaved partial lines would defeat the
 * point of a machine-readable format. This is not on the hot path of a request
 * — one write per event, not per byte.
 */
class Logger {
public:
  /** @brief Configure the process logger. Call once, before threads start. */
  static void configure(std::string node_id, Level threshold) {
    instance().node_id_ = std::move(node_id);
    instance().threshold_ = threshold;
  }

  static Logger &instance() {
    static Logger logger;
    return logger;
  }

  void emit(Level level, const std::string &component, const std::string &msg,
            const std::vector<Field> &fields = {}) {
    if (static_cast<int>(level) < static_cast<int>(threshold_)) {
      return;
    }

    std::string line = "{\"ts\":\"";
    line += timestamp();
    line += "\",\"level\":\"";
    line += level_name(level);
    line += "\",\"node_id\":\"";
    line += escape(node_id_);
    line += "\",\"component\":\"";
    line += escape(component);
    line += "\",\"msg\":\"";
    line += escape(msg);
    line += "\"";
    for (const Field &f : fields) {
      line += ",\"";
      line += escape(f.key);
      line += "\":\"";
      line += escape(f.value);
      line += "\"";
    }
    line += "}\n";

    std::lock_guard<std::mutex> lock(mutex_);
    // Written as ONE string, not streamed piecewise: a single << of the whole
    // line is what keeps two threads from interleaving fragments even if the
    // stream's own locking differs between implementations.
    std::cout << line;
    std::cout.flush();
  }

private:
  /**
   * @brief RFC3339 UTC timestamp with millisecond precision.
   *
   * gmtime_r rather than gmtime: gmtime returns a pointer to a shared static
   * buffer, so two threads logging at once would corrupt each other's
   * timestamp.
   */
  [[nodiscard]] static std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;

    std::tm tm{};
    gmtime_r(&secs, &tm);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(millis));
    return std::string(buffer);
  }

  std::string node_id_;
  Level threshold_ = Level::kInfo;
  std::mutex mutex_;
};

/** @brief Free functions so call sites read as one line. */
inline void debug(const std::string &component, const std::string &msg,
                  const std::vector<Field> &fields = {}) {
  Logger::instance().emit(Level::kDebug, component, msg, fields);
}
inline void info(const std::string &component, const std::string &msg,
                 const std::vector<Field> &fields = {}) {
  Logger::instance().emit(Level::kInfo, component, msg, fields);
}
inline void warn(const std::string &component, const std::string &msg,
                 const std::vector<Field> &fields = {}) {
  Logger::instance().emit(Level::kWarn, component, msg, fields);
}
inline void error(const std::string &component, const std::string &msg,
                  const std::vector<Field> &fields = {}) {
  Logger::instance().emit(Level::kError, component, msg, fields);
}

/** @brief Component names, so a typo is a compile error. */
inline constexpr const char *kComponentMain = "main";
inline constexpr const char *kComponentHttp = "http";
inline constexpr const char *kComponentStateMachine = "state_machine";
inline constexpr const char *kComponentStore = "store";
inline constexpr const char *kComponentRaftClient = "raft_client";

} // namespace log
} // namespace kvdb
