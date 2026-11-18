/* DXVK compiler messages -> the port's native log (rdvk::log). */
#include "util/log/log.h"
namespace rdvk { void log(const char* format, ...); }
namespace dxvk {
  void Logger::log(LogLevel level, const std::string& message) {
    static const char* const names[] = {"trace", "debug", "info", "warn", "err", "none"};
    if (level >= LogLevel::Warn)
      rdvk::log("RDVK_DXBC %s: %s", names[uint32_t(level)], message.c_str());
  }
}
