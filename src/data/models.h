#pragma once
#include <cstdint>
#include <string>

namespace sumi::data {

// Mirrors design doc §4 (and Mihon's domain model).

enum class MangaStatus : int { Unknown = 0, Ongoing, Completed, Licensed, PublishingFinished, Cancelled, Hiatus };

struct Source {
    int64_t     id = 0;          // stable hash of package name + lang
    std::string name;
    std::string lang;
    std::string version;
    bool        enabled = true;
    bool        nsfw    = false;
};

struct Manga {
    int64_t     id = 0;
    int64_t     source_id = 0;
    std::string url;             // source-relative identifier
    std::string title;
    std::string artist;
    std::string author;
    std::string description;
    std::string genre;           // newline-separated, as Mihon
    MangaStatus status = MangaStatus::Unknown;
    std::string thumbnail_url;
    bool        favorite = false;
    int64_t     last_update = 0; // unix millis
    int64_t     date_added  = 0;
};

struct Chapter {
    int64_t     id = 0;
    int64_t     manga_id = 0;
    std::string url;
    std::string name;
    std::string scanlator;
    bool        read = false;
    bool        bookmark = false;
    int         last_page_read = 0;
    int         pages_total = 0;
    double      chapter_number = -1;
    int         source_order = 0;   // 0 = first in the source's list (usually newest)
    int64_t     date_fetch = 0;
    int64_t     date_upload = 0;
};

struct Category {
    int64_t     id = 0;
    std::string name;
    int         sort_order = 0;
    int         count = 0;      // library entries in it
};

struct LibraryItem {
    Manga manga;
    int   unread = 0;
    int   total  = 0;
};

struct UpdateItem {
    int64_t     chapter_id = 0;
    int64_t     manga_id = 0;
    std::string manga_title;
    std::string chapter_name;
    bool        read = false;
    int64_t     date_fetch = 0;   // when the update found it
};

enum class DownloadState : int { Queued = 0, Downloading = 1, Done = 2, Error = 3 };

struct DownloadItem {
    int64_t       chapter_id = 0;
    int64_t       manga_id = 0;
    int64_t       source_id = 0;
    std::string   manga_title;
    std::string   chapter_name;
    std::string   chapter_url;
    DownloadState state = DownloadState::Queued;
    int           pages_done = 0;
    int           pages_total = 0;
    std::string   error;
    int64_t       queued_at = 0;
};

struct HistoryItem {
    int64_t     chapter_id = 0;
    int64_t     manga_id = 0;
    std::string manga_title;
    std::string chapter_name;
    int64_t     last_read = 0;   // unix millis
    int64_t     time_read = 0;   // millis spent
};

} // namespace sumi::data
