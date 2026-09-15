// Test transport for page images: serves generated JPEG pages for image-host URLs and passes
// everything else to another transport (recorded source fixtures). Counts image requests, and can
// fail specific URLs. Pages are 784x1145 (a real WeebCentral size) with the page number drawn as
// that many black bars, so tests can tell pages apart after processing. Page 5 is a 784x4000 strip.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <jpeglib.h>

#include "net/http.h"

namespace fake_images {

inline std::string jpeg_page(int number, int w = 784, int h = 1145)
{
    std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h), 255);
    for (int y = 40; y < h - 40; ++y)                                    // a light gray panel
        for (int x = 40; x < w - 40; ++x) px[static_cast<size_t>(y * w + x)] = 200;
    for (int b = 0; b < number && b < 20; ++b)                           // `number` black bars
        for (int y = 100; y < 400; ++y)
            for (int x = 80 + b * 30; x < 100 + b * 30; ++x) px[static_cast<size_t>(y * w + x)] = 0;

    jpeg_compress_struct c{};
    jpeg_error_mgr e{};
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    unsigned char* buf = nullptr;
    unsigned long len = 0;
    jpeg_mem_dest(&c, &buf, &len);
    c.image_width = static_cast<JDIMENSION>(w);
    c.image_height = static_cast<JDIMENSION>(h);
    c.input_components = 1;
    c.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 90, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = px.data() + static_cast<size_t>(c.next_scanline) * static_cast<size_t>(w);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    std::string out(reinterpret_cast<const char*>(buf), len);
    jpeg_destroy_compress(&c);
    std::free(buf);
    return out;
}

// Page number from a URL like ".../0025-003.png" -> 3.
inline int page_number(const std::string& url)
{
    size_t dash = url.rfind('-'), dot = url.rfind('.');
    if (dash == std::string::npos || dot == std::string::npos || dot < dash) return 1;
    return std::atoi(url.substr(dash + 1, dot - dash - 1).c_str());
}

class Transport final : public sumi::net::Transport {
public:
    explicit Transport(sumi::net::Transport& other) : other_(other) {}

    std::map<std::string, int> hits;   // image URL -> request count
    std::set<std::string>      fail;   // image URLs that answer 404
    std::string                last_referer;

    sumi::net::Response perform(const sumi::net::Request& req) override
    {
        if (req.url.find("://scans") == std::string::npos) return other_.perform(req);
        ++hits[req.url];
        for (const auto& h : req.headers)
            if (h.first == "Referer") last_referer = h.second;
        sumi::net::Response r;
        r.final_url = req.url;
        if (fail.count(req.url)) {
            r.status = 404;
            return r;
        }
        r.status = 200;
        int n = page_number(req.url);
        r.body = n == 5 ? jpeg_page(n, 784, 4000) : jpeg_page(n);   // page 5 is a long strip
        return r;
    }

    int total_hits() const
    {
        int n = 0;
        for (const auto& kv : hits) n += kv.second;
        return n;
    }

private:
    sumi::net::Transport& other_;
};

} // namespace fake_images
