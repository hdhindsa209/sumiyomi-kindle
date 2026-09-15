#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "image/process.h"

namespace sumi::image {

// Processed pages, cached (design doc §7.4). Two tiers:
//   RAM:  the last few pages (current page ± neighbors), so turning back is instant.
//   Disk: one file per processed page, 4 bits per pixel (1072×1448 ≈ 776 KB), least recently
//         used evicted past a byte cap. Reading one back is a single read + unpack.
// Keys include every processing setting, so changing fit/dither/tone never serves stale output.
// The index lives in memory, rebuilt from the directory on init() (file size + mtime), rather
// than in SQLite: the cache is disposable and only the worker thread touches it.
// Not thread-safe: use from one thread (the worker).
class PageCache {
public:
    struct Entry {
        Gray    page;
        uint8_t parts = 1;   // pages the source image produced (2 for a split spread)
    };

    PageCache(std::string dir, uint64_t cap_bytes = 512ull << 20, size_t ram_pages = 5);

    bool init(std::string& err);

    // Key for part `part` of the image at `url` processed with `opt`.
    static std::string key(const std::string& url, int part, const ProcessOptions& opt);

    bool get(const std::string& key, Entry& out);
    // `into_ram` false: disk only (background chapter loading mustn't push the pages being read out of RAM).
    bool put(const std::string& key, const Entry& entry, std::string& err, bool into_ram = true);
    // In RAM or on disk, without reading it.
    bool contains(const std::string& key) const;

    uint64_t disk_bytes() const { return disk_bytes_; }
    size_t   disk_files() const { return index_.size(); }
    size_t   ram_pages() const { return ram_.size(); }
    void     clear_ram() { ram_.clear(); }

private:
    struct DiskEntry { uint64_t size; uint64_t stamp; };

    std::string path_for(const std::string& key) const;
    void remember(const std::string& key, const Entry& e);
    void touch(const std::string& key);
    void evict();

    std::string dir_;
    uint64_t    cap_;
    size_t      ram_cap_;
    uint64_t    disk_bytes_ = 0;
    uint64_t    clock_ = 0;                          // LRU order: larger = more recent
    std::map<std::string, DiskEntry> index_;         // file name -> size, stamp
    std::list<std::pair<std::string, Entry>> ram_;   // front = most recent
};

} // namespace sumi::image
