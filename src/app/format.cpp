#include "app/format.h"

#include <chrono>

namespace sumi::app {
namespace {

constexpr int64_t kDayMs = 86400000;
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct Civil { int64_t y; unsigned m, d; };

// Inverse of days_from_civil (Howard Hinnant).
Civil civil_from_days(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<int64_t>(yoe) + era * 400 + (m <= 2), m, d};
}

int64_t day_of(int64_t ms) { return (ms >= 0 ? ms : ms - kDayMs + 1) / kDayMs; }

} // namespace

int64_t wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string day_header(int64_t unix_ms, int64_t now_ms)
{
    int64_t diff = day_of(now_ms) - day_of(unix_ms);
    if (diff == 0) return "Today";
    if (diff == 1) return "Yesterday";
    Civil c = civil_from_days(day_of(unix_ms)), now = civil_from_days(day_of(now_ms));
    std::string out = std::string(kMonths[c.m - 1]) + " " + std::to_string(c.d);
    if (c.y != now.y) out += ", " + std::to_string(c.y);
    return out;
}

std::string relative_date(int64_t unix_ms, int64_t now_ms)
{
    if (unix_ms <= 0) return "";
    int64_t diff = day_of(now_ms) - day_of(unix_ms);
    if (diff <= 0) return "Today";
    if (diff == 1) return "Yesterday";
    if (diff < 7) return std::to_string(diff) + " days ago";
    if (diff < 30) return std::to_string(diff / 7) + (diff / 7 == 1 ? " week ago" : " weeks ago");
    Civil c = civil_from_days(day_of(unix_ms));
    return std::string(kMonths[c.m - 1]) + " " + std::to_string(c.d) + ", " + std::to_string(c.y);
}

std::string status_name(int status)
{
    switch (status) {
    case 1: return "Ongoing";
    case 2: return "Completed";
    case 3: return "Licensed";
    case 4: return "Publishing finished";
    case 5: return "Cancelled";
    case 6: return "On hiatus";
    default: return "Unknown status";
    }
}

} // namespace sumi::app
