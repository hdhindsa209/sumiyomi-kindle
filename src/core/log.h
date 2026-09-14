#pragma once
#include <cstdint>

namespace sumi {

enum class LogLevel : uint8_t { E, W, I, D };

// CLOCK_MONOTONIC milliseconds. Async-signal-safe.
uint64_t mono_ms() noexcept;

// One line: "LEVEL ts_ms subsystem message". Not async-signal-safe (uses vsnprintf).
void log_write(LogLevel level, const char* subsys, const char* fmt, ...) noexcept
    __attribute__((format(printf, 3, 4)));

// Same line format, fixed message, no formatting. Async-signal-safe
// (clock_gettime + write only) — the only logging allowed in signal handlers.
void log_raw(LogLevel level, const char* subsys, const char* msg) noexcept;

} // namespace sumi

#define SUMI_LOGE(sub, ...) ::sumi::log_write(::sumi::LogLevel::E, sub, __VA_ARGS__)
#define SUMI_LOGW(sub, ...) ::sumi::log_write(::sumi::LogLevel::W, sub, __VA_ARGS__)
#define SUMI_LOGI(sub, ...) ::sumi::log_write(::sumi::LogLevel::I, sub, __VA_ARGS__)
#ifdef NDEBUG
#define SUMI_LOGD(sub, ...) ((void)0)
#else
#define SUMI_LOGD(sub, ...) ::sumi::log_write(::sumi::LogLevel::D, sub, __VA_ARGS__)
#endif
