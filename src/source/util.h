#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sumi::source {

// Host helpers exposed to extensions (design doc §6.3). Plain C++ so they're testable on their own.

// RFC 3986 §5.2 reference resolution. `base` must be absolute.
std::string url_resolve(std::string_view base, std::string_view ref);
// Percent-encode everything except RFC 3986 unreserved characters (A-Z a-z 0-9 - . _ ~).
std::string url_encode(std::string_view s);

std::string trim(std::string_view s);

// Parse `s` with a strptime-like `format` into unix millis (UTC).
// Supported: %Y %m %d %H %M %S %f(fraction) %b (English month name/abbr) %z (Z, +HH:MM, +HHMM),
// %% and literal characters. Times without %z are taken as UTC. nullopt if it doesn't match.
std::optional<int64_t> parse_time(std::string_view format, std::string_view s);

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d);

} // namespace sumi::source
