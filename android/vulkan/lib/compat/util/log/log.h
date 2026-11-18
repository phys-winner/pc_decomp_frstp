/* Stand-in for DXVK's logger (util/log/log.h): messages go to the port's native log. */
#pragma once
#include <cstdint>
#include <string>

namespace dxvk {
  enum class LogLevel : uint32_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, None = 5 };

  class Logger {
  public:
    static void trace(const std::string& message) { log(LogLevel::Trace, message); }
    static void debug(const std::string& message) { log(LogLevel::Debug, message); }
    static void info (const std::string& message) { log(LogLevel::Info, message); }
    static void warn (const std::string& message) { log(LogLevel::Warn, message); }
    static void err  (const std::string& message) { log(LogLevel::Error, message); }
    static void log  (LogLevel level, const std::string& message);
    static LogLevel logLevel() { return LogLevel::Warn; }
  };
}
