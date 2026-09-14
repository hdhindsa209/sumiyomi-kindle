#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace sumi {
namespace {

constexpr char kLevelChar[] = {'E', 'W', 'I', 'D'};
constexpr size_t kLineMax = 512;

// run.sh redirects stderr to the log file; the binary itself always writes fd 2.
constexpr int kLogFd = STDERR_FILENO;

void write_all(const char* buf, size_t len) noexcept
{
    while (len > 0) {
        ssize_t n = write(kLogFd, buf, len);
        if (n <= 0) return;
        buf += n;
        len -= static_cast<size_t>(n);
    }
}

// Async-signal-safe decimal formatting. Returns chars written (no NUL).
size_t format_u64(char* out, uint64_t v) noexcept
{
    char tmp[20];
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

// Writes "L ts subsys " into out. Async-signal-safe. Returns length.
size_t format_prefix(char* out, size_t cap, LogLevel level, const char* subsys) noexcept
{
    size_t sub_len = std::strlen(subsys);
    if (cap < 24 + sub_len + 1) return 0;
    size_t len = 0;
    out[len++] = kLevelChar[static_cast<size_t>(level)];
    out[len++] = ' ';
    len += format_u64(out + len, mono_ms());
    out[len++] = ' ';
    std::memcpy(out + len, subsys, sub_len);
    len += sub_len;
    out[len++] = ' ';
    return len;
}

} // namespace

uint64_t mono_ms() noexcept
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u
         + static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

void log_write(LogLevel level, const char* subsys, const char* fmt, ...) noexcept
{
    char buf[kLineMax];
    size_t len = format_prefix(buf, sizeof buf, level, subsys);

    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf + len, sizeof buf - len, fmt, ap);
    va_end(ap);
    if (n > 0) len += static_cast<size_t>(n);
    if (len > sizeof buf - 1) len = sizeof buf - 1;   // truncated: keep room for '\n'

    buf[len++] = '\n';
    write_all(buf, len);
}

void log_raw(LogLevel level, const char* subsys, const char* msg) noexcept
{
    char buf[kLineMax];
    size_t len = format_prefix(buf, sizeof buf, level, subsys);
    size_t msg_len = std::strlen(msg);
    if (msg_len > sizeof buf - 1 - len) msg_len = sizeof buf - 1 - len;
    std::memcpy(buf + len, msg, msg_len);
    len += msg_len;
    buf[len++] = '\n';
    write_all(buf, len);
}

} // namespace sumi
