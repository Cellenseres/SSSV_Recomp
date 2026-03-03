#include "sssv_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

std::mutex g_log_mutex;
std::unordered_set<std::string> g_once_keys;
constexpr size_t kMaxDebugOnceKeys = 4096;
bool g_once_key_limit_warned = false;

#if defined(NDEBUG)
std::atomic_bool g_debug_enabled { false };
#else
std::atomic_bool g_debug_enabled { true };
#endif

void vprint_locked(const char* prefix, const char* fmt, va_list args) {
    std::fputs(prefix, stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

namespace sssv::log {

bool debug_enabled() {
    return g_debug_enabled.load(std::memory_order_relaxed);
}

void set_debug_enabled(bool enabled) {
    g_debug_enabled.store(enabled, std::memory_order_relaxed);
}

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        vprint_locked("[SSSV] ", fmt, args);
    }
    va_end(args);
}

void debug(const char* fmt, ...) {
    if (!g_debug_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_debug_enabled.load(std::memory_order_relaxed)) {
            vprint_locked("[SSSV:DBG] ", fmt, args);
        }
    }
    va_end(args);
}

void debug_once(const char* key, const char* fmt, ...) {
    if (key == nullptr || !g_debug_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_debug_enabled.load(std::memory_order_relaxed)) {
            if (g_once_keys.find(key) != g_once_keys.end()) {
                // Already printed.
            } else if (g_once_keys.size() >= kMaxDebugOnceKeys) {
                if (!g_once_key_limit_warned) {
                    std::fputs(
                        "[SSSV:DBG] debug_once key registry full; suppressing new debug_once keys.\n",
                        stderr
                    );
                    std::fflush(stderr);
                    g_once_key_limit_warned = true;
                }
            } else {
                g_once_keys.emplace(key);
                vprint_locked("[SSSV:DBG] ", fmt, args);
            }
        }
    }
    va_end(args);
}

} // namespace sssv::log
