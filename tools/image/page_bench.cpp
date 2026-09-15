// Decode page images and report timing (M4). Grows a stage per M4 S2 step.
//   page_bench [--fit WxH] [--repeat N] [--out out.pgm] image...
// On the device: scp a few real pages and run it to get the §7.2 budget numbers.
#include "core/log.h"
#include "image/decode.h"

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
    std::string out;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fit") && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &opt.fit_w, &opt.fit_h) != 2) opt.fit_w = opt.fit_h = 0;
        } else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
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

    std::printf("file,format,src_w,src_h,out_w,out_h,bytes,decode_ms_min,decode_ms_avg\n");
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
        image::Gray g;
        uint64_t best = ~0ull, total = 0;
        for (int r = 0; r < repeat; ++r) {
            uint64_t t0 = mono_ms();
            if (!image::decode_gray(bytes.data(), bytes.size(), opt, g, err)) break;
            uint64_t ms = mono_ms() - t0;
            best = std::min(best, ms);
            total += ms;
        }
        if (g.px.empty()) {
            std::printf("%s,error,,,,,%zu,,%s\n", path.c_str(), bytes.size(), err.c_str());
            ++failures;
            continue;
        }
        std::printf("%s,%s,%d,%d,%d,%d,%zu,%llu,%.1f\n", path.c_str(), image::format_name(info.format), info.w, info.h,
                    g.w, g.h, bytes.size(), static_cast<unsigned long long>(best), static_cast<double>(total) / repeat);
        if (!out.empty()) {
            std::ofstream o(out, std::ios::binary);
            o << "P5\n" << g.w << " " << g.h << "\n255\n";
            o.write(reinterpret_cast<const char*>(g.px.data()), static_cast<std::streamsize>(g.px.size()));
        }
    }
    return failures ? 1 : 0;
}
