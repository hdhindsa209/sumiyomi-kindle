#include "app/fake_data.h"

#include <algorithm>

namespace sumi::app {
namespace {

constexpr const char* kDash = " \xE2\x80\x94 ";   // " — "

std::vector<FakeChapter> chapters(int count, int read_up_to, const std::vector<std::string>& names)
{
    static const char* kDates[] = {"2 days ago", "5 days ago", "1 week ago", "2 weeks ago", "3 weeks ago", "1 month ago"};
    std::vector<FakeChapter> out;
    for (int n = count; n >= 1; --n) {
        std::string name = "Chapter " + std::to_string(n);
        size_t i = static_cast<size_t>(count - n);
        if (i < names.size()) name += kDash + names[i];
        out.push_back({name, kDates[std::min<size_t>(i, 5)], 18 + (n * 7) % 9, n <= read_up_to, n > count - 3});
    }
    return out;
}

} // namespace

std::vector<FakeManga> fake_library()
{
    return {
        {"Chainsaw Man", "Tatsuki Fujimoto", "Ongoing", "MangaDex",
         "Denji is a teenage boy living with a Chainsaw Devil named Pochita. Due to the debt his father left behind, "
         "he has been living a rock-bottom life while repaying his debt by harvesting devil corpses with Pochita.",
         {"Action", "Horror", "Supernatural"}, 0, chapters(24, 22, {"Curse", "Gun Devil", "Kon", "Rescue", "Katana Man"})},
        {"Frieren: Beyond Journey's End", "Kanehito Yamada", "Ongoing", "MangaDex",
         "The adventure is over but life goes on for an elf mage just beginning to learn what living is all about.",
         {"Adventure", "Fantasy", "Drama"}, 0, chapters(40, 37, {"The Journey's End", "Priest's Lie"})},
        {"Dandadan", "Yukinobu Tatsu", "Ongoing", "MangaDex",
         "A high school girl who believes in ghosts and a boy who believes in aliens make a bet that changes everything.",
         {"Action", "Comedy", "Sci-Fi"}, 0, chapters(30, 30, {})},
        {"Blue Period", "Tsubasa Yamaguchi", "Ongoing", "MangaDex",
         "A popular but bored high school student discovers the joy of painting and sets his sights on art school.",
         {"Drama", "School Life"}, 0, chapters(18, 12, {})},
        {"The Apothecary Diaries", "Natsu Hyuuga", "Ongoing", "MangaDex",
         "Maomao, a young woman trained in medicine, is kidnapped and sold to the imperial palace as a servant.",
         {"Mystery", "Historical", "Drama"}, 0, chapters(52, 51, {})},
        {"Vinland Saga", "Makoto Yukimura", "Completed", "MangaDex",
         "Thorfinn joins the band of the man who killed his father, seeking the chance to challenge him to a duel.",
         {"Action", "Historical"}, 0, chapters(60, 60, {})},
        {"Oshi no Ko", "Aka Akasaka", "Completed", "MangaDex",
         "A doctor is reborn as the child of the idol he admired, and is drawn into the entertainment industry.",
         {"Drama", "Mystery"}, 0, chapters(33, 20, {})},
        {"Kaiju No. 8", "Naoya Matsumoto", "Ongoing", "MangaDex",
         "Kafka Hibino, a man who cleans up after kaiju battles, suddenly gains the power to become one himself.",
         {"Action", "Sci-Fi"}, 0, chapters(22, 22, {})},
        {"Spy x Family", "Tatsuya Endo", "Ongoing", "MangaDex",
         "A spy, an assassin and a telepath pose as a family, each keeping their true identity secret.",
         {"Action", "Comedy"}, 0, chapters(28, 25, {})},
        {"Look Back", "Tatsuki Fujimoto", "Completed", "MangaDex",
         "Two girls bond over their shared love of drawing manga.", {"Drama"}, 1, chapters(1, 0, {})},
        {"Goodnight Punpun", "Inio Asano", "Completed", "MangaDex",
         "The story of Punpun Onodera, from childhood through adulthood.", {"Drama", "Psychological"}, 1, chapters(40, 13, {})},
        {"Berserk", "Kentaro Miura", "Hiatus", "MangaDex",
         "Guts, a lone mercenary, searches for purpose in a brutal medieval world.", {"Action", "Dark Fantasy"}, 2, chapters(30, 0, {})},
        {"Monster", "Naoki Urasawa", "Completed", "MangaDex",
         "A brilliant surgeon's life unravels after he saves a boy who grows up to be a killer.", {"Mystery", "Thriller"}, 3, chapters(20, 20, {})},
    };
}

std::vector<FakeUpdate> fake_updates()
{
    return {
        {"Today", "Chainsaw Man", "Chapter 24" + std::string(kDash) + "Curse", true},
        {"Today", "The Apothecary Diaries", "Chapter 52", true},
        {"Today", "Frieren: Beyond Journey's End", "Chapter 40", false},
        {"Yesterday", "Spy x Family", "Chapter 28", false},
        {"Yesterday", "Dandadan", "Chapter 30", true},
        {"Yesterday", "Blue Period", "Chapter 18", false},
        {"Sep 10", "Oshi no Ko", "Chapter 33", false},
        {"Sep 10", "Kaiju No. 8", "Chapter 22", true},
        {"Sep 9", "Chainsaw Man", "Chapter 23" + std::string(kDash) + "Gun Devil", true},
        {"Sep 9", "Frieren: Beyond Journey's End", "Chapter 39", false},
        {"Sep 8", "The Apothecary Diaries", "Chapter 51", true},
        {"Sep 7", "Spy x Family", "Chapter 27", false},
        {"Sep 6", "Dandadan", "Chapter 29", true},
        {"Sep 5", "Blue Period", "Chapter 17", false},
    };
}

std::vector<FakeHistory> fake_history()
{
    return {
        {"Today", "Chainsaw Man", "Chapter 22" + std::string(kDash) + "Kon", "21:40"},
        {"Today", "Frieren: Beyond Journey's End", "Chapter 37", "08:15"},
        {"Yesterday", "The Apothecary Diaries", "Chapter 51", "22:02"},
        {"Yesterday", "Dandadan", "Chapter 30", "19:30"},
        {"Sep 11", "Blue Period", "Chapter 12", "23:11"},
        {"Sep 10", "Oshi no Ko", "Chapter 20", "12:47"},
        {"Sep 9", "Goodnight Punpun", "Chapter 13", "01:20"},
        {"Sep 8", "Spy x Family", "Chapter 25", "18:05"},
        {"Sep 6", "Kaiju No. 8", "Chapter 22", "20:30"},
    };
}

std::vector<FakeSource> fake_sources()
{
    return {
        {"English", "MangaDex", false},     {"English", "MangaSee", false},     {"English", "Comick", false},
        {"English", "Bato.to", false},      {"Japanese", "MangaDex", false},    {"Japanese", "Rawkuma", false},
        {"Spanish", "MangaDex", false},     {"Spanish", "TuMangaOnline", false}, {"French", "MangaDex", false},
        {"Portuguese", "MangaDex", false},
    };
}

int unread_count(const FakeManga& m)
{
    int n = 0;
    for (const FakeChapter& c : m.chapters) n += !c.read;
    return n;
}

} // namespace sumi::app
