// Decode + process page images and report per-stage timing (M4, design doc §7.2 budget).
//   page_bench [--fit WxH] [--repeat N] [--dither sharp|balanced|smooth] [--out out.pgm] image...
// --out writes the processed page (the first one, for a split spread).
// On the device: scp a few real pages and run it to get the §7.2 budget numbers.
#include "core/log.h"
#include "image/decode.h"
#include "image/process.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sumi;

int main(int argc, char** argv)
{
    image::DecodeOptions opt;
    opt.fit_w = 1072;
    opt.fit_h = 1448;
    int repeat = 5;
    image::ProcessOptions popt;
    std::string out;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fit") && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &opt.fit_w, &opt.fit_h) != 2) opt.fit_w = opt.fit_h = 0;
        } else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--dither") && i + 1 < argc) {
            std::string d = argv[++i];
            popt.dither = d == "sharp" ? image::Dither::Sharp : d == "smooth" ? image::Dither::Smooth : image::Dither::Balanced;
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            out = argv[++i];
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s [--fit WxH] [--repeat N] [--out out.pgm] image...\n", argv[0]);
        return 2;
    }

    popt.screen_w = opt.fit_w > 0 ? opt.fit_w : 1072;
    popt.screen_h = opt.fit_h > 0 ? opt.fit_h : 1448;
    std::printf("file,format,src_w,src_h,dec_w,dec_h,bytes,decode_ms,crop_ms,resize_ms,tone_ms,quantize_ms,total_ms,pages,page_w,page_h\n");
    int failures = 0;
    for (const std::string& path : files) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string err;
        image::Info info;
        if (!image::probe(bytes.data(), bytes.size(), info, err)) {
            std::printf("%s,error,,,,,%zu,,%s\n", path.c_str(), bytes.size(), err.c_str());
            ++failures;
            continue;
        }
        // Min over repeats for each stage (the least disturbed run).
        uint64_t t_dec = ~0ull, t_crop = ~0ull, t_resize = ~0ull, t_tone = ~0ull, t_quant = ~0ull, t_total = ~0ull;
        image::Gray g, page;
        size_t page_count = 0;
        bool ok = true;
        for (int r = 0; r < repeat && ok; ++r) {
            uint64_t t0 = mono_ms();
            if (!image::decode_gray(bytes.data(), bytes.size(), opt, g, err)) { ok = false; break; }
            uint64_t t1 = mono_ms();
            image::Gray c = popt.crop_borders ? image::crop(g, image::content_bounds(g)) : g;
            uint64_t t2 = mono_ms();
            int32_t w = 0, h = 0;
            image::fit_size(c.w, c.h, popt, w, h);
            page = image::resize(c, w, h);
            uint64_t t3 = mono_ms();
            image::apply_tone(page, popt);
            uint64_t t4 = mono_ms();
            image::quantize16(page, popt.dither);
            uint64_t t5 = mono_ms();
            t_dec = std::min(t_dec, t1 - t0); t_crop = std::min(t_crop, t2 - t1); t_resize = std::min(t_resize, t3 - t2);
            t_tone = std::min(t_tone, t4 - t3); t_quant = std::min(t_quant, t5 - t4); t_total = std::min(t_total, t5 - t0);
            page_count = image::process_page(g, popt).size();   // spread detection, not timed
        }
        if (!ok) {
            std::printf("%s,error,,,,,%zu,,%s\n", path.c_str(), bytes.size(), err.c_str());
            ++failures;
            continue;
        }
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::printf("%s,%s,%d,%d,%d,%d,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%d,%d\n", path.c_str(),
                    image::format_name(info.format), info.w, info.h, g.w, g.h, bytes.size(), u(t_dec), u(t_crop),
                    u(t_resize), u(t_tone), u(t_quant), u(t_total), page_count, page.w, page.h);
        if (!out.empty()) {
            std::ofstream o(out, std::ios::binary);
            o << "P5\n" << page.w << " " << page.h << "\n255\n";
            o.write(reinterpret_cast<const char*>(page.px.data()), static_cast<std::streamsize>(page.px.size()));
        }
    }
    return failures ? 1 : 0;
}
