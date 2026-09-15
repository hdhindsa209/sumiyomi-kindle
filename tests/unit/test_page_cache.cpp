// Page cache (M4 S3): exact round trip at 4 bpp, variant keys, RAM tier, LRU eviction by bytes,
// restart (index rebuilt from disk, LRU order kept), corrupt files.
#include "image/page_cache.h"

#include "check.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace sumi::image;

namespace {

std::string temp_dir(const char* name)
{
    std::string dir = std::string("page_cache_test_") + name + "_" + std::to_string(getpid());
    std::string cmd = "rm -rf '" + dir + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
    return dir + "/nested/pages";   // init() creates intermediate directories
}

PageCache::Entry page(int32_t w, int32_t h, uint8_t seed, uint8_t parts = 1)
{
    PageCache::Entry e;
    e.parts = parts;
    e.page.w = w;
    e.page.h = h;
    e.page.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (size_t i = 0; i < e.page.px.size(); ++i) e.page.px[i] = static_cast<uint8_t>(((i * 7 + seed) % 16) * 17);
    return e;
}

size_t file_size(const PageCache& c) { return c.disk_files() ? static_cast<size_t>(c.disk_bytes() / c.disk_files()) : 0; }

void test_round_trip_is_exact_and_compact()
{
    std::string dir = temp_dir("rt");
    PageCache cache(dir);
    std::string err;
    CHECK(cache.init(err));
    ProcessOptions o;
    std::string k = PageCache::key("https://scans.example/p1.png", 0, o);
    PageCache::Entry in = page(1071, 1448, 3, 2);   // odd width: last nibble of each row matters
    CHECK(cache.put(k, in, err));
    CHECK_EQ(cache.disk_files(), 1);
    CHECK(cache.disk_bytes() < 1071ull * 1448 / 2 + 200);   // 4 bpp + small header

    cache.clear_ram();                                        // force a disk read
    PageCache::Entry out;
    CHECK(cache.get(k, out));
    CHECK(out.page.w == in.page.w && out.page.h == in.page.h && out.parts == 2);
    CHECK(out.page.px == in.page.px);
}

void test_keys_include_every_setting()
{
    ProcessOptions a, b;
    const std::string url = "https://x/1.jpg";
    CHECK(PageCache::key(url, 0, a) == PageCache::key(url, 0, b));
    CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 1, a));
    b.dither = Dither::Smooth;
    CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));
    b = a; b.fit = Fit::Width;          CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));
    b = a; b.crop_borders = false;      CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));
    b = a; b.rtl = false;               CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));
    b = a; b.gamma = 1.5f;              CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));
    b = a; b.screen_w = 1236;           CHECK(PageCache::key(url, 0, a) != PageCache::key(url, 0, b));

    std::string dir = temp_dir("keys");
    PageCache cache(dir);
    std::string err;
    CHECK(cache.init(err));
    CHECK(cache.put(PageCache::key(url, 0, a), page(10, 10, 1), err));
    PageCache::Entry e;
    CHECK(!cache.get(PageCache::key(url, 0, b), e));          // other settings: miss
}

void test_ram_tier_and_lru_eviction()
{
    std::string dir = temp_dir("lru");
    std::string err;
    {
        PageCache probe(dir);
        CHECK(probe.init(err));
        CHECK(probe.put("size", page(100, 100, 0), err));
    }
    PageCache sizing(dir);
    CHECK(sizing.init(err));
    uint64_t one = sizing.disk_bytes();

    // Room for three pages on disk, two in RAM.
    std::string dir2 = temp_dir("lru2");
    PageCache cache(dir2, one * 3 + one / 2, 2);
    CHECK(cache.init(err));
    for (int i = 0; i < 3; ++i) CHECK(cache.put("k" + std::to_string(i), page(100, 100, static_cast<uint8_t>(i)), err));
    CHECK_EQ(cache.ram_pages(), 2);
    CHECK_EQ(cache.disk_files(), 3);

    PageCache::Entry e;
    CHECK(cache.get("k0", e));                                 // k0 becomes most recent
    CHECK(cache.put("k3", page(100, 100, 3), err));            // over the cap: evicts the LRU one, k1
    CHECK_EQ(cache.disk_files(), 3);
    cache.clear_ram();
    CHECK(!cache.get("k1", e));
    CHECK(cache.get("k0", e) && cache.get("k2", e) && cache.get("k3", e));
    CHECK(e.page.px == page(100, 100, 3).page.px);
    (void)file_size;
}

void test_restart_rebuilds_index()
{
    std::string dir = temp_dir("restart");
    std::string err;
    {
        PageCache cache(dir);
        CHECK(cache.init(err));
        CHECK(cache.put("a", page(64, 64, 1), err));
        CHECK(cache.put("b", page(64, 64, 2), err));
    }
    // A leftover temp file from a crash mid-write is cleaned up.
    std::FILE* f = std::fopen((dir + "/deadbeef.page.tmp").c_str(), "wb");
    if (f) std::fclose(f);

    PageCache again(dir);
    CHECK(again.init(err));
    CHECK_EQ(again.disk_files(), 2);
    PageCache::Entry e;
    CHECK(again.get("b", e) && e.page.px == page(64, 64, 2).page.px);
    struct stat st {};
    CHECK(stat((dir + "/deadbeef.page.tmp").c_str(), &st) != 0);
}

void test_corrupt_file_is_a_miss_and_removed()
{
    std::string dir = temp_dir("corrupt");
    PageCache cache(dir);
    std::string err;
    CHECK(cache.init(err));
    CHECK(cache.put("x", page(32, 32, 5), err));
    cache.clear_ram();

    // Truncate every page file.
    std::string cmd = "for f in '" + dir + "'/*.page; do head -c 20 \"$f\" > \"$f.cut\" && mv \"$f.cut\" \"$f\"; done";
    int rc = std::system(cmd.c_str());
    (void)rc;
    PageCache::Entry e;
    CHECK(!cache.get("x", e));
    CHECK_EQ(cache.disk_files(), 0);
    CHECK(cache.put("x", page(32, 32, 5), err));               // and it can be written again
    cache.clear_ram();
    CHECK(cache.get("x", e));
}

} // namespace

void test_cap_change_and_clear()
{
    std::string dir = temp_dir("cap");
    std::string err;
    PageCache cache(dir);
    CHECK(cache.init(err));
    for (int i = 0; i < 4; ++i) CHECK(cache.put("k" + std::to_string(i), page(100, 100, static_cast<uint8_t>(i)), err));
    uint64_t one = cache.disk_bytes() / 4;
    cache.set_cap(one * 2);                                    // smaller limit: the oldest go now
    CHECK_EQ(cache.disk_files(), 2);
    CHECK_EQ(cache.cap(), one * 2);
    PageCache::Entry e;
    cache.clear_ram();
    CHECK(!cache.get("k0", e) && cache.get("k3", e));
    cache.clear();
    CHECK_EQ(cache.disk_files(), 0);
    CHECK_EQ(cache.disk_bytes(), 0);
    CHECK(!cache.get("k3", e));
    PageCache again(dir);                                      // nothing left on disk either
    CHECK(again.init(err));
    CHECK_EQ(again.disk_files(), 0);
}

int main()
{
    RUN(test_round_trip_is_exact_and_compact);
    RUN(test_keys_include_every_setting);
    RUN(test_ram_tier_and_lru_eviction);
    RUN(test_restart_rebuilds_index);
    RUN(test_corrupt_file_is_a_miss_and_removed);
    RUN(test_cap_change_and_clear);
    int rc = check_result();
    int cleanup = std::system("rm -rf page_cache_test_*");
    (void)cleanup;
    return rc;
}
