#pragma once

namespace sssv::log {

// Debug logging is on by default in non-release builds.
bool debug_enabled();
void set_debug_enabled(bool enabled);

void info(const char* fmt, ...);
void debug(const char* fmt, ...);
void debug_once(const char* key, const char* fmt, ...);

} // namespace sssv::log
