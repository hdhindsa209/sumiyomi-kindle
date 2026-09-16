// Drive an Aidoku source end to end: listing, search, details, chapters, pages.
//   aix_runner <file.aix> [--search <query>] [--pick <n>]
#include "core/log.h"
#include "net/http.h"
#include "source/aidoku_source.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

using namespace sumi;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.aix> [--search q] [--pick n]\n", argv[0]);
        return 2;
    }
    std::string query = "";
    size_t pick = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--search") && i + 1 < argc) query = argv[++i];
        else if (!std::strcmp(argv[i], "--pick") && i + 1 < argc) pick = std::strtoul(argv[++i], nullptr, 10);
    }

    source::AixPackage pkg;
    std::string err;
    if (!source::read_aix(argv[1], pkg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    // The certificates sit next to the app on the device; SUMI_ASSETS overrides for other layouts.
    std::string assets = std::getenv("SUMI_ASSETS") ? std::getenv("SUMI_ASSETS") : "";
    std::string ca = assets.empty() ? "" : assets + "/certs/cacert.pem";
    for (const char* guess : {"/mnt/us/extensions/sumiyomi/assets/certs/cacert.pem", "assets/certs/cacert.pem"}) {
        if (!ca.empty()) break;
        std::ifstream f(guess);
        if (f) ca = guess;
    }
    auto transport = net::make_curl_transport({ca, "", "Sumiyomi/0.2.1"}, err);
    if (!transport) {
        std::fprintf(stderr, "network: %s\n", err.c_str());
        return 1;
    }
    net::Client http(*transport);

    std::map<std::string, std::string> settings;
    source::AidokuSource::Settings s;
    s.get = [&](const std::string& key) { return settings.count(key) ? settings[key] : std::string(); };
    s.set = [&](const std::string& key, const std::string& value) { settings[key] = value; };

    auto src = source::AidokuSource::load(pkg, &http, s, err);
    if (!src) {
        std::fprintf(stderr, "%s: %s\n", pkg.name.c_str(), err.c_str());
        return 1;
    }
    std::printf("%s v%d (%s)\n\n", pkg.name.c_str(), pkg.version, pkg.language.c_str());

    source::SMangaPage page;
    bool ok = query.empty() ? src->popular(1, page, err) : src->search(1, query, page, err);
    std::printf("%s: %s%zu results, has_next=%d\n", query.empty() ? "listing" : "search",
                ok ? "" : ("FAILED: " + err + " ").c_str(), page.mangas.size(), page.has_next_page);
    for (size_t i = 0; i < page.mangas.size() && i < 5; ++i)
        std::printf("  [%zu] %s  %s\n", i, page.mangas[i].title.c_str(), page.mangas[i].url.c_str());
    if (page.mangas.empty()) return ok ? 0 : 1;

    source::SManga seed = page.mangas[std::min(pick, page.mangas.size() - 1)];
    source::SManga details;
    if (!src->details(seed, details, err)) std::printf("\ndetails FAILED: %s\n", err.c_str());
    else std::printf("\ndetails: %s\n  author=%s status=%d tags=%zu\n  %.140s\n", details.title.c_str(),
                     details.author.c_str(), details.status, details.genres.size(), details.description.c_str());

    std::vector<source::SChapter> chapters;
    if (!src->chapters(seed, chapters, err)) {
        std::printf("\nchapters FAILED: %s\n", err.c_str());
        return 1;
    }
    std::printf("\nchapters: %zu\n", chapters.size());
    for (size_t i = 0; i < chapters.size() && i < 5; ++i)
        std::printf("  %s  #%.1f  %s  %lld\n", chapters[i].name.c_str(), chapters[i].chapter_number,
                    chapters[i].url.c_str(), static_cast<long long>(chapters[i].date_upload));
    if (chapters.empty()) return 0;

    std::vector<source::SPage> pages;
    if (!src->pages(seed, chapters.front(), pages, err)) {
        std::printf("\npages FAILED: %s\n", err.c_str());
        return 1;
    }
    std::printf("\npages: %zu\n", pages.size());
    for (size_t i = 0; i < pages.size() && i < 3; ++i) std::printf("  [%d] %s\n", pages[i].index, pages[i].url.c_str());
    return 0;
}
