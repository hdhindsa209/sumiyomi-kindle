#include "image/page_cache.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

namespace sumi::image {
namespace {

constexpr char kMagic[4] = {'S', 'U', 'M', 'P'};
constexpr uint8_t kVersion = 1;
constexpr const char* kExt = ".page";

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool mkdirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

void put_u16(std::vector<uint8_t>& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) { put_u16(b, v & 0xFFFF); put_u16(b, v >> 16); }
uint32_t get_u16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t get_u32(const uint8_t* p) { return get_u16(p) | get_u16(p + 2) << 16; }

bool ends_with(const std::string& s, const char* suffix)
{
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

PageCache::PageCache(std::string dir, uint64_t cap_bytes, size_t ram_pages)
    : dir_(std::move(dir)), cap_(cap_bytes), ram_cap_(ram_pages)
{
}

std::string PageCache::key(const std::string& url, int part, const ProcessOptions& o)
{
    char variant[160];
    std::snprintf(variant, sizeof variant, "|p%d|v1|%dx%d|f%d|d%d|c%d|s%d|r%d|t%u-%u-%.3f|m%d", part, o.screen_w, o.screen_h,
                  static_cast<int>(o.fit), static_cast<int>(o.dither), o.crop_borders, o.split_spreads, o.rtl,
                  o.black_point, o.white_point, static_cast<double>(o.gamma), o.margin);
    return url + variant;
}

std::string PageCache::path_for(const std::string& k) const
{
    char name[32];
    std::snprintf(name, sizeof name, "%016llx%s", static_cast<unsigned long long>(fnv1a(k)), kExt);
    return dir_ + "/" + name;
}

bool PageCache::init(std::string& err)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!mkdirs(dir_)) {
        err = "page cache: cannot create " + dir_ + ": " + std::strerror(errno);
        return false;
    }
    DIR* d = opendir(dir_.c_str());
    if (!d) {
        err = "page cache: cannot open " + dir_ + ": " + std::strerror(errno);
        return false;
    }
    struct Found { std::string name; uint64_t size; int64_t mtime; };
    std::vector<Found> found;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        std::string path = dir_ + "/" + name;
        if (ends_with(name, ".tmp")) {   // interrupted write
            unlink(path.c_str());
            continue;
        }
        if (!ends_with(name, kExt)) continue;
        struct stat st {};
        if (stat(path.c_str(), &st) == 0) found.push_back({name, static_cast<uint64_t>(st.st_size), static_cast<int64_t>(st.st_mtime)});
    }
    closedir(d);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mtime < b.mtime; });
    index_.clear();
    disk_bytes_ = 0;
    for (const Found& f : found) {
        index_[f.name] = {f.size, ++clock_};
        disk_bytes_ += f.size;
    }
    evict();
    SUMI_LOGI("cache", "page cache %s: %zu pages, %llu KB", dir_.c_str(), index_.size(),
              static_cast<unsigned long long>(disk_bytes_ / 1024));
    return true;
}

void PageCache::remember(const std::string& k, const Entry& e)
{
    for (auto it = ram_.begin(); it != ram_.end(); ++it)
        if (it->first == k) {
            ram_.erase(it);
            break;
        }
    ram_.emplace_front(k, e);
    while (ram_.size() > ram_cap_) ram_.pop_back();
}

void PageCache::touch(const std::string& k)
{
    std::string path = path_for(k);
    std::string name = path.substr(dir_.size() + 1);
    auto it = index_.find(name);
    if (it != index_.end()) it->second.stamp = ++clock_;
    utime(path.c_str(), nullptr);   // survives restarts: init() orders by mtime
}

void PageCache::evict()
{
    while (disk_bytes_ > cap_ && !index_.empty()) {
        auto oldest = std::min_element(index_.begin(), index_.end(),
                                       [](const auto& a, const auto& b) { return a.second.stamp < b.second.stamp; });
        unlink((dir_ + "/" + oldest->first).c_str());
        disk_bytes_ -= std::min(disk_bytes_, oldest->second.size);
        index_.erase(oldest);
    }
}

bool PageCache::get(const std::string& k, Entry& out)
{
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = ram_.begin(); it != ram_.end(); ++it) {
        if (it->first != k) continue;
        ram_.splice(ram_.begin(), ram_, it);
        out = ram_.front().second;
        return true;
    }

    std::string path = path_for(k);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);

    auto corrupt = [&] {
        SUMI_LOGW("cache", "dropping unreadable cache file %s", path.c_str());
        unlink(path.c_str());
        auto it = index_.find(path.substr(dir_.size() + 1));
        if (it != index_.end()) {
            disk_bytes_ -= std::min(disk_bytes_, it->second.size);
            index_.erase(it);
        }
        return false;
    };
    constexpr size_t kHeader = 4 + 1 + 1 + 2 + 4 + 4;
    if (bytes.size() < kHeader || std::memcmp(bytes.data(), kMagic, 4) != 0 || bytes[4] != kVersion) return corrupt();
    uint8_t parts = bytes[5];
    uint32_t key_len = get_u16(bytes.data() + 6), w = get_u32(bytes.data() + 8), h = get_u32(bytes.data() + 12);
    uint64_t data_len = (static_cast<uint64_t>(w) * h + 1) / 2;
    if (w == 0 || h == 0 || w > 16384 || h > 65536 || bytes.size() != kHeader + key_len + data_len) return corrupt();
    if (std::string(bytes.begin() + kHeader, bytes.begin() + kHeader + key_len) != k) return false;   // hash collision

    Entry e;
    e.parts = std::max<uint8_t>(1, parts);
    e.page.w = static_cast<int32_t>(w);
    e.page.h = static_cast<int32_t>(h);
    e.page.px.resize(static_cast<size_t>(w) * h);
    const uint8_t* data = bytes.data() + kHeader + key_len;
    for (size_t i = 0; i < e.page.px.size(); ++i) {
        uint8_t nib = (data[i / 2] >> ((i & 1) ? 0 : 4)) & 0x0F;
        e.page.px[i] = static_cast<uint8_t>(nib * 17);
    }
    remember(k, e);
    touch(k);
    out = std::move(e);
    return true;
}

bool PageCache::contains(const std::string& k) const
{
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : ram_)
        if (e.first == k) return true;
    std::string path = path_for(k);
    return index_.count(path.substr(dir_.size() + 1)) != 0;
}

bool PageCache::put(const std::string& k, const Entry& entry, std::string& err, bool into_ram)
{
    const Gray& g = entry.page;
    if (g.w <= 0 || g.h <= 0 || g.px.size() != static_cast<size_t>(g.w) * static_cast<size_t>(g.h)) {
        err = "page cache: empty page";
        return false;
    }

    std::vector<uint8_t> bytes(kMagic, kMagic + 4);
    bytes.push_back(kVersion);
    bytes.push_back(entry.parts);
    put_u16(bytes, static_cast<uint32_t>(k.size()));
    put_u32(bytes, static_cast<uint32_t>(g.w));
    put_u32(bytes, static_cast<uint32_t>(g.h));
    bytes.insert(bytes.end(), k.begin(), k.end());
    size_t base = bytes.size();
    bytes.resize(base + (g.px.size() + 1) / 2, 0);
    for (size_t i = 0; i < g.px.size(); ++i) {
        auto level = static_cast<uint8_t>((g.px[i] + 8) / 17);   // pages are already 16-level
        bytes[base + i / 2] |= static_cast<uint8_t>((i & 1) ? level : level << 4);
    }

    std::string path = path_for(k), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "page cache: cannot write " + tmp + ": " + std::strerror(errno);
        return false;
    }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "page cache: write failed for " + path + ": " + std::strerror(errno);
        unlink(tmp.c_str());
        return false;
    }

    // Encoding and writing ran unlocked: a page read on the reader's thread never waits for a write.
    std::lock_guard<std::mutex> lock(mu_);
    if (into_ram) remember(k, entry);
    std::string name = path.substr(dir_.size() + 1);
    auto it = index_.find(name);
    if (it != index_.end()) disk_bytes_ -= std::min(disk_bytes_, it->second.size);
    index_[name] = {bytes.size(), ++clock_};
    disk_bytes_ += bytes.size();
    evict();
    return true;
}

} // namespace sumi::image
