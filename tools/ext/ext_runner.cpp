// Drive one extension end to end: popular, latest, search, details, chapters, pages.
//   ext_runner <source_dir> [--live | --record <dir> | --replay <dir>] [--search <query>] [--pick <n>]
// --pick chooses which search result is used for details/chapters/pages (default 0).
#include "core/log.h"
#include "fixture_transport.h"
#include "source/extension.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace sumi;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <source_dir> [--live|--record <dir>|--replay <dir>] [--search q] [--pick n]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1], mode = "--live", fixtures, query = "frieren";
    size_t pick = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--live")) mode = argv[i];
        else if ((!std::strcmp(argv[i], "--record") || !std::strcmp(argv[i], "--replay")) && i + 1 < argc) { mode = argv[i]; fixtures = argv[++i]; }
        else if (!std::strcmp(argv[i], "--search") && i + 1 < argc) query = argv[++i];
        else if (!std::strcmp(argv[i], "--pick") && i + 1 < argc) pick = std::strtoul(argv[++i], nullptr, 10);
    }

    std::string err;
    std::unique_ptr<net::Transport> real, wrapper;
    net::Transport* transport = nullptr;
    if (mode == "--replay") {
        wrapper = std::make_unique<fixtures::ReplayTransport>(fixtures);
        transport = wrapper.get();
    } else {
        real = net::make_curl_transport({SUMI_ASSETS_DIR "/certs/cacert.pem", "", "Sumiyomi/0.3 (manga reader for Kindle)"}, err);
        if (!real) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        transport = real.get();
        if (mode == "--record") {
            wrapper = std::make_unique<fixtures::RecordingTransport>(*real, fixtures);
            transport = wrapper.get();
        }
    }
    net::Client client(*transport);
    auto ext = source::Extension::load(dir, &client, err);
    if (!ext) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    std::printf("%s %s (%s) id=%lld\n", ext->manifest().name.c_str(), ext->manifest().version.c_str(),
                ext->manifest().lang.c_str(), static_cast<long long>(ext->id()));

    auto show_page = [](const char* what, const source::SMangaPage& p) {
        std::printf("\n%s: %zu results, has_next=%d\n", what, p.mangas.size(), p.has_next_page);
        for (size_t i = 0; i < p.mangas.size() && i < 5; ++i)
            std::printf("  [%zu] %s  %s\n       %s\n", i, p.mangas[i].title.c_str(), p.mangas[i].url.c_str(), p.mangas[i].thumbnail_url.c_str());
    };

    int failures = 0;
    auto step = [&](bool ok, const char* what) {
        if (!ok) { std::printf("\n%s FAILED: %s\n", what, err.c_str()); ++failures; }
        return ok;
    };

    source::SMangaPage page;
    uint64_t t0 = mono_ms();
    if (step(ext->popular(1, page, err), "popular")) show_page("popular", page);
    if (step(ext->latest(1, page, err), "latest")) show_page("latest", page);
    source::SMangaPage found;
    if (!step(ext->search(1, query, found, err), "search")) return 1;
    show_page(("search '" + query + "'").c_str(), found);
    if (found.mangas.size() <= pick) { std::printf("nothing to pick\n"); return 1; }

    source::SManga details;
    if (step(ext->details(found.mangas[pick], details, err), "details")) {
        std::printf("\ndetails: %s\n  author=%s artist=%s status=%d genres=%zu\n  %.160s...\n", details.title.c_str(),
                    details.author.c_str(), details.artist.c_str(), details.status, details.genres.size(), details.description.c_str());
    }
    std::vector<source::SChapter> chapters;
    if (step(ext->chapters(found.mangas[pick], chapters, err), "chapters")) {
        std::printf("\nchapters: %zu\n", chapters.size());
        for (size_t i = 0; i < chapters.size() && i < 5; ++i)
            std::printf("  %s  #%.1f  %s  [%s]  %lld\n", chapters[i].name.c_str(), chapters[i].chapter_number, chapters[i].url.c_str(),
                        chapters[i].scanlator.c_str(), static_cast<long long>(chapters[i].date_upload));
    }
    if (!chapters.empty()) {
        std::vector<source::SPage> pages;
        if (step(ext->pages(found.mangas[pick], chapters.front(), pages, err), "pages")) {
            std::printf("\npages: %zu\n", pages.size());
            if (!pages.empty()) std::printf("  [%d] %s\n", pages.front().index, pages.front().url.c_str());
        }
    }
    std::printf("\ndone in %llums, %d failure(s)\n", static_cast<unsigned long long>(mono_ms() - t0), failures);
    return failures ? 1 : 0;
}
