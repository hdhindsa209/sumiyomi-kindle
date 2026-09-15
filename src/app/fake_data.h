#pragma once
#include <string>
#include <vector>

namespace sumi::app {

// Fake library data for the M2 shell. Replaced by SQLite repositories in M3.

struct FakeChapter {
    std::string name;
    std::string date;
    int         pages;
    bool        read;
    bool        downloaded;
};

struct FakeManga {
    std::string title;
    std::string author;
    std::string status;
    std::string source;
    std::string description;
    std::vector<std::string> genres;
    int category;               // index into kCategories
    std::vector<FakeChapter> chapters;
};

inline const std::vector<std::string> kCategories = {"Reading", "On Hold", "Plan to Read", "Completed"};

std::vector<FakeManga> fake_library();

struct FakeUpdate  { std::string group, manga, chapter; bool downloaded; };
struct FakeHistory { std::string group, manga, chapter, time; };
struct FakeSource  { std::string lang, name; bool nsfw; };

std::vector<FakeUpdate>  fake_updates();
std::vector<FakeHistory> fake_history();
std::vector<FakeSource>  fake_sources();

int unread_count(const FakeManga& m);

} // namespace sumi::app
