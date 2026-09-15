#pragma once
#include <cstdint>
#include <string>

namespace sumi::app {

// "Today", "Yesterday", "3 days ago", "2 weeks ago", else "Sep 14, 2026" (UTC dates). 0 = "".
std::string relative_date(int64_t unix_ms, int64_t now_ms);
// "Today", "Yesterday", else "Sep 14" / "Sep 14, 2025" for other years — Updates/History group headers.
std::string day_header(int64_t unix_ms, int64_t now_ms);
// Unix millis, wall clock.
int64_t wall_ms();
std::string status_name(int status);   // data::MangaStatus

} // namespace sumi::app
